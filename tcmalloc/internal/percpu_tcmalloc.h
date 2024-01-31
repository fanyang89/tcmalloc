// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_INTERNAL_PERCPU_TCMALLOC_H_
#define TCMALLOC_INTERNAL_PERCPU_TCMALLOC_H_

#if defined(__linux__)
#include <linux/param.h>
#else
#include <sys/param.h>
#endif
#include <sys/mman.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/casts.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/base/optimization.h"
#include "absl/functional/function_ref.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/sysinfo.h"

#if defined(TCMALLOC_INTERNAL_PERCPU_USE_RSEQ)
#if !defined(__clang__)
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO 1
#elif __clang_major__ >= 9 && !__has_feature(speculative_load_hardening)
// asm goto requires the use of Clang 9 or newer:
// https://releases.llvm.org/9.0.0/tools/clang/docs/ReleaseNotes.html#c-language-changes-in-clang
//
// SLH (Speculative Load Hardening) builds do not support asm goto.  We can
// detect these compilation modes since
// https://github.com/llvm/llvm-project/commit/379e68a763097bed55556c6dc7453e4b732e3d68.
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO 1
#if __clang_major__ >= 11
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT 1
#endif

#else
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO 0
#endif
#else
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO 0
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

struct PerCPUMetadataState {
  size_t virtual_size;
  size_t resident_size;
};

// The bit denotes that tcmalloc_slabs contains valid slabs offset.
constexpr inline uintptr_t kCachedSlabsBit = 63;

struct ResizeSlabsInfo {
  void* old_slabs;
  size_t old_slabs_size;
};

namespace subtle {
namespace percpu {

enum class Shift : uint8_t;
constexpr uint8_t ToUint8(Shift shift) { return static_cast<uint8_t>(shift); }
constexpr Shift ToShiftType(size_t shift) {
  ASSERT(ToUint8(static_cast<Shift>(shift)) == shift);
  return static_cast<Shift>(shift);
}

// The allocation size for the slabs array.
inline size_t GetSlabsAllocSize(Shift shift, int num_cpus) {
  return static_cast<size_t>(num_cpus) << ToUint8(shift);
}

// Since we lazily initialize our slab, we expect it to be mmap'd and not
// resident.  We align it to a page size so neighboring allocations (from
// TCMalloc's internal arena) do not necessarily cause the metadata to be
// faulted in.
//
// We prefer a small page size (EXEC_PAGESIZE) over the anticipated huge page
// size to allow small-but-slow to allocate the slab in the tail of its
// existing Arena block.
static constexpr std::align_val_t kPhysicalPageAlign{EXEC_PAGESIZE};

// Tcmalloc slab for per-cpu caching mode.
// Conceptually it is equivalent to an array of NumClasses PerCpuSlab's,
// and in fallback implementation it is implemented that way. But optimized
// implementation uses more compact layout and provides faster operations.
//
// Methods of this type must only be used in threads where it is known that the
// percpu primitives are available and percpu::IsFast() has previously returned
// 'true'.
class TcmallocSlab {
 public:
  using DrainHandler = absl::FunctionRef<void(
      int cpu, size_t size_class, void** batch, size_t size, size_t cap)>;
  using ShrinkHandler =
      absl::FunctionRef<void(size_t size_class, void** batch, size_t size)>;

  // We use a single continuous region of memory for all slabs on all CPUs.
  // This region is split into NumCPUs regions of a power-of-2 size
  // (32/64/128/256/512k).
  // First num_classes_ words of each CPU region are occupied by slab
  // headers (Header struct). The remaining memory contain slab arrays.
  // struct Slabs {
  //  std::atomic<int64_t> header[NumClasses];
  //  void* mem[];
  // };

  constexpr TcmallocSlab() = default;

  // Init must be called before any other methods.
  // <slabs> is memory for the slabs with size corresponding to <shift>.
  // <capacity> callback returns max capacity for size class <size_class>.
  // <shift> indicates the number of bits to shift the CPU ID in order to
  //     obtain the location of the per-CPU slab.
  //
  // Initial capacity is 0 for all slabs.
  void Init(size_t num_classes,
            absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
            void* slabs, absl::FunctionRef<size_t(size_t)> capacity,
            Shift shift);

  // Lazily initializes the slab for a specific cpu.
  // <capacity> callback returns max capacity for size class <size_class>.
  //
  // Prior to InitCpu being called on a particular `cpu`, non-const operations
  // other than Push/Pop/PushBatch/PopBatch are invalid.
  void InitCpu(int cpu, absl::FunctionRef<size_t(size_t)> capacity);

  // Grows or shrinks the size of the slabs to use the <new_shift> value. First
  // we initialize <new_slabs>, then lock all headers on the old slabs,
  // atomically update to use the new slabs, and teardown the old slabs. Returns
  // a pointer to old slabs to be madvised away along with the size of the old
  // slabs and the number of bytes that were reused.
  //
  // <alloc> is memory allocation callback (e.g. malloc).
  // <capacity> callback returns max capacity for size class <cl>.
  // <populated> returns whether the corresponding cpu has been populated.
  //
  // Caller must ensure that there are no concurrent calls to InitCpu,
  // ShrinkOtherCache, or Drain.
  ABSL_MUST_USE_RESULT ResizeSlabsInfo ResizeSlabs(
      Shift new_shift, void* new_slabs,
      absl::FunctionRef<size_t(size_t)> capacity,
      absl::FunctionRef<bool(size_t)> populated, DrainHandler drain_handler);

  // For tests. Returns the freed slabs pointer.
  void* Destroy(absl::FunctionRef<void(void*, size_t, std::align_val_t)> free);

  // Number of elements in cpu/size_class slab.
  size_t Length(int cpu, size_t size_class) const;

  // Number of elements (currently) allowed in cpu/size_class slab.
  size_t Capacity(int cpu, size_t size_class) const;

  // If running on cpu, increment the cpu/size_class slab's capacity to no
  // greater than min(capacity+len, max_capacity(<shift>)) and return the
  // increment applied. Otherwise return 0.
  // <max_capacity> is a callback that takes the current slab shift as input and
  // returns the max capacity of <size_class> for that shift value - this is in
  // order to ensure that the shift value used is consistent with the one used
  // in the rest of this function call. Note: max_capacity must be the same as
  // returned by capacity callback passed to Init.
  size_t Grow(int cpu, size_t size_class, size_t len,
              absl::FunctionRef<size_t(uint8_t)> max_capacity);

  // Add an item (which must be non-zero) to the current CPU's slab. Returns
  // true if add succeeds. Otherwise invokes <overflow_handler> and returns
  // false (assuming that <overflow_handler> returns negative value).
  bool Push(size_t size_class, void* item);

  // Remove an item (LIFO) from the current CPU's slab. If the slab is empty,
  // invokes <underflow_handler> and returns its result.
  ABSL_MUST_USE_RESULT void* Pop(size_t class_size);

  // Add up to <len> items to the current cpu slab from the array located at
  // <batch>. Returns the number of items that were added (possibly 0). All
  // items not added will be returned at the start of <batch>. Items are not
  // added if there is no space on the current cpu, or if the thread was
  // re-scheduled since last Push/Pop.
  // REQUIRES: len > 0.
  size_t PushBatch(size_t size_class, void** batch, size_t len);

  // Pop up to <len> items from the current cpu slab and return them in <batch>.
  // Returns the number of items actually removed. If the thread was
  // re-scheduled since last Push/Pop, the function returns 0.
  // REQUIRES: len > 0.
  size_t PopBatch(size_t size_class, void** batch, size_t len);

  // Caches the current cpu slab offset in tcmalloc_slabs if it wasn't
  // cached and the slab is not resizing. Returns the current cpu and the flag
  // if the offset was previously uncached and is now cached.
  std::pair<int, bool> CacheCpuSlab();

  // Uncaches the slab offset for the current thread, so that the next Push/Pop
  // operation will return false.
  void UncacheCpuSlab();

  // Grows the cpu/size_class slab's capacity to no greater than
  // min(capacity+len, max_capacity(<shift>)) and returns the increment
  // applied.
  // <max_capacity> is a callback that takes the current slab shift as input and
  // returns the max capacity of <size_class> for that shift value - this is in
  // order to ensure that the shift value used is consistent with the one used
  // in the rest of this function call. Note: max_capacity must be the same as
  // returned by capacity callback passed to Init.
  // This may be called from another processor, not just the <cpu>.
  size_t GrowOtherCache(int cpu, size_t size_class, size_t len,
                        absl::FunctionRef<size_t(uint8_t)> max_capacity);

  // Decrements the cpu/size_class slab's capacity to no less than
  // max(capacity-len, 0) and returns the actual decrement applied. It attempts
  // to shrink any unused capacity (i.e end-current) in cpu/size_class's slab;
  // if it does not have enough unused items, it pops up to <len> items from
  // cpu/size_class slab and then shrinks the freed capacity.
  //
  // May be called from another processor, not just the <cpu>.
  // REQUIRES: len > 0.
  size_t ShrinkOtherCache(int cpu, size_t size_class, size_t len,
                          ShrinkHandler shrink_handler);

  // Remove all items (of all classes) from <cpu>'s slab; reset capacity for all
  // classes to zero.  Then, for each sizeclass, invoke
  // DrainHandler(size_class, <items from slab>, <previous slab capacity>);
  //
  // It is invalid to concurrently execute Drain() for the same CPU; calling
  // Push/Pop/Grow/Shrink concurrently (even on the same CPU) is safe.
  void Drain(int cpu, DrainHandler drain_handler);

  PerCPUMetadataState MetadataMemoryUsage() const;

  // Gets the current shift of the slabs. Intended for use by the thread that
  // calls ResizeSlabs().
  uint8_t GetShift() const {
    return ToUint8(GetSlabsAndShift(std::memory_order_relaxed).second);
  }

 private:
  // In order to support dynamic slab metadata sizes, we need to be able to
  // atomically update both the slabs pointer and the shift value so we store
  // both together in an atomic SlabsAndShift, which manages the bit operations.
  class SlabsAndShift {
   public:
    // These masks allow for distinguishing the shift bits from the slabs
    // pointer bits. The maximum shift value is less than kShiftMask and
    // kShiftMask is less than kPhysicalPageAlign.
    static constexpr size_t kShiftMask = 0xFF;
    static constexpr size_t kSlabsMask = ~kShiftMask;

    constexpr explicit SlabsAndShift() noexcept : raw_(0) {}
    SlabsAndShift(const void* slabs, Shift shift)
        : raw_(reinterpret_cast<uintptr_t>(slabs) | ToUint8(shift)) {
      ASSERT((raw_ & kShiftMask) == ToUint8(shift));
      ASSERT(reinterpret_cast<void*>(raw_ & kSlabsMask) == slabs);
    }

    std::pair<void*, Shift> Get() const {
      static_assert(kShiftMask >= 0 && kShiftMask <= UCHAR_MAX,
                    "kShiftMask must fit in a uint8_t");
      // Avoid expanding the width of Shift else the compiler will insert an
      // additional instruction to zero out the upper bits on the critical path
      // of alloc / free.  Not zeroing out the bits is safe because both ARM and
      // x86 only use the lowest byte for shift count in variable shifts.
      return {reinterpret_cast<void*>(raw_ & kSlabsMask),
              static_cast<Shift>(raw_ & kShiftMask)};
    }

   private:
    uintptr_t raw_;
  };

  // Slab header (packed, atomically updated 64-bit).
  // All {begin, current, end} values are pointer offsets from per-CPU region
  // start. The slot array is in [begin, end), and the occupied slots are in
  // [begin, current).
  struct Header {
    // The end offset of the currently occupied slots.
    uint16_t current;
    // Copy of end. Updated by Shrink/Grow, but is not overwritten by Drain.
    uint16_t end_copy;
    // Lock updates only begin and end with a 32-bit write.
    union {
      struct {
        // The begin offset of the slot array for this size class.
        uint16_t begin;
        // The end offset of the slot array for this size class.
        uint16_t end;
      };
      uint32_t lock_update;
    };

    // Lock is used by Drain to stop concurrent mutations of the Header.
    // Lock sets begin to 0xffff and end to 0, which makes Push and Pop fail
    // regardless of current value.
    bool IsLocked() const;
    void Lock();

    bool IsInitialized() const {
      // Once we initialize a header, begin/end are never simultaneously 0
      // to avoid pointing at the Header array.
      return lock_update != 0;
    }
  };

  // We cast Header to std::atomic<int64_t>.
  static_assert(sizeof(Header) == sizeof(std::atomic<int64_t>),
                "bad Header size");

  // It's important that we use consistent values for slabs/shift rather than
  // loading from the atomic repeatedly whenever we use one of the values.
  ABSL_MUST_USE_RESULT std::pair<void*, Shift> GetSlabsAndShift(
      std::memory_order order) const {
    return slabs_and_shift_.load(order).Get();
  }

  static void* CpuMemoryStart(void* slabs, Shift shift, int cpu);
  static std::atomic<int64_t>* GetHeader(void* slabs, Shift shift, int cpu,
                                         size_t size_class);
  static Header LoadHeader(std::atomic<int64_t>* hdrp);
  static void StoreHeader(std::atomic<int64_t>* hdrp, Header hdr);
  static void LockHeader(void* slabs, Shift shift, int cpu, size_t size_class);
  // <begins> is an array of the <begin> values for each size class.
  void DrainCpu(void* slabs, Shift shift, int cpu, DrainHandler drain_handler);
  // Stops concurrent mutations from occurring for <cpu> by locking the
  // corresponding headers. All allocations/deallocations will miss this cache
  // for <cpu> until the headers are unlocked.
  void StopConcurrentMutations(void* slabs, Shift shift, int cpu,
                               size_t virtual_cpu_id_offset);

  // Implementation of InitCpu() allowing for reuse in ResizeSlabs().
  void InitCpuImpl(void* slabs, Shift shift, int cpu,
                   size_t virtual_cpu_id_offset,
                   absl::FunctionRef<size_t(size_t)> capacity);

  std::pair<int, bool> CacheCpuSlabSlow();

  size_t num_classes_ = 0;
  // We store both a pointer to the array of slabs and the shift value together
  // so that we can atomically update both with a single store.
  std::atomic<SlabsAndShift> slabs_and_shift_{};
  // This is in units of bytes.
  size_t virtual_cpu_id_offset_ = offsetof(kernel_rseq, cpu_id);
  // In ResizeSlabs, we need to allocate space to store begin offsets on the
  // arena. We reuse this space here.
  uint16_t* resize_begins_ = nullptr;
  // ResizeSlabs is running so any Push/Pop should go to fallback
  // overflow/underflow handler.
  std::atomic<bool> resizing_{false};
};

inline size_t TcmallocSlab::Length(int cpu, size_t size_class) const {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
  return hdr.IsLocked() ? 0 : hdr.current - hdr.begin;
}

inline size_t TcmallocSlab::Capacity(int cpu, size_t size_class) const {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
  return hdr.IsLocked() ? 0 : hdr.end - hdr.begin;
}

#if defined(__x86_64__)
#define TCMALLOC_RSEQ_RELOC_TYPE "R_X86_64_NONE"
#define TCMALLOC_RSEQ_JUMP "jmp"
#if !defined(__PIC__) && !defined(__PIE__)
#define TCMALLOC_RSEQ_SET_CS(name) \
  "movq $__rseq_cs_" #name "_%=, %[rseq_cs_addr]\n"
#else
#define TCMALLOC_RSEQ_SET_CS(name) \
  "lea __rseq_cs_" #name           \
  "_%=(%%rip), %[scratch]\n"       \
  "movq %[scratch], %[rseq_cs_addr]\n"
#endif

#elif defined(__aarch64__)
// The trampoline uses a non-local branch to restart critical sections.
// The trampoline is located in the .text.unlikely section, and the maximum
// distance of B and BL branches in ARM64 is limited to 128MB. If the linker
// detects the distance being too large, it injects a thunk which may clobber
// the x16 or x17 register according to the ARMv8 ABI standard.
// The actual clobbering is hard to trigger in a test, so instead of waiting
// for clobbering to happen in production binaries, we proactively always
// clobber x16 and x17 to shake out bugs earlier.
// RSEQ critical section asm blocks should use TCMALLOC_RSEQ_CLOBBER
// in the clobber list to account for this.
#ifndef NDEBUG
#define TCMALLOC_RSEQ_TRAMPLINE_SMASH \
  "mov x16, #-2097\n"                 \
  "mov x17, #-2099\n"
#else
#define TCMALLOC_RSEQ_TRAMPLINE_SMASH
#endif
#define TCMALLOC_RSEQ_CLOBBER "x16", "x17"
#define TCMALLOC_RSEQ_RELOC_TYPE "R_AARCH64_NONE"
#define TCMALLOC_RSEQ_JUMP "b"
#define TCMALLOC_RSEQ_SET_CS(name)                     \
  TCMALLOC_RSEQ_TRAMPLINE_SMASH                        \
  "adrp %[scratch], __rseq_cs_" #name                  \
  "_%=\n"                                              \
  "add %[scratch], %[scratch], :lo12:__rseq_cs_" #name \
  "_%=\n"                                              \
  "str %[scratch], %[rseq_cs_addr]\n"
#endif

#if !defined(__clang_major__) || __clang_major__ >= 9
#define TCMALLOC_RSEQ_RELOC ".reloc 0, " TCMALLOC_RSEQ_RELOC_TYPE ", 1f\n"
#else
#define TCMALLOC_RSEQ_RELOC
#endif

// Common rseq asm prologue.
// It uses labels 1-4 and assumes the critical section ends with label 5.
// The prologue assumes there is [scratch] input with a scratch register.
#define TCMALLOC_RSEQ_PROLOGUE(name)                                          \
  /* __rseq_cs only needs to be writeable to allow for relocations.*/         \
  ".pushsection __rseq_cs, \"aw?\"\n"                                         \
  ".balign 32\n"                                                              \
  ".local __rseq_cs_" #name                                                   \
  "_%=\n"                                                                     \
  ".type __rseq_cs_" #name                                                    \
  "_%=,@object\n"                                                             \
  ".size __rseq_cs_" #name                                                    \
  "_%=,32\n"                                                                  \
  "__rseq_cs_" #name                                                          \
  "_%=:\n"                                                                    \
  ".long 0x0\n"                                                               \
  ".long 0x0\n"                                                               \
  ".quad 4f\n"                                                                \
  ".quad 5f - 4f\n"                                                           \
  ".quad 2f\n"                                                                \
  ".popsection\n" TCMALLOC_RSEQ_RELOC                                         \
  ".pushsection __rseq_cs_ptr_array, \"aw?\"\n"                               \
  "1:\n"                                                                      \
  ".balign 8\n"                                                               \
  ".quad __rseq_cs_" #name                                                    \
  "_%=\n" /* Force this section to be retained.                               \
             It is for debugging, but is otherwise not referenced. */         \
  ".popsection\n"                                                             \
  ".pushsection .text.unlikely, \"ax?\"\n" /* This is part of the upstream    \
                                              rseq ABI.  The 4 bytes prior to \
                                              the abort IP must match         \
                                              TCMALLOC_PERCPU_RSEQ_SIGNATURE  \
                                              (as configured by our rseq      \
                                              syscall's signature parameter). \
                                              This signature is used to       \
                                              annotate valid abort IPs (since \
                                              rseq_cs could live in a         \
                                              user-writable segment). */      \
  ".long %c[rseq_sig]\n"                                                      \
  ".local " #name                                                             \
  "_trampoline_%=\n"                                                          \
  ".type " #name                                                              \
  "_trampoline_%=,@function\n"                                                \
  "" #name                                                                    \
  "_trampoline_%=:\n"                                                         \
  "2:\n" TCMALLOC_RSEQ_JUMP                                                   \
  " 3f\n"                                                                     \
  ".size " #name "_trampoline_%=, . - " #name                                 \
  "_trampoline_%=\n"                                                          \
  ".popsection\n"                   /* Prepare */                             \
  "3:\n" TCMALLOC_RSEQ_SET_CS(name) /* Start */                               \
      "4:\n"

#define TCMALLOC_RSEQ_INPUTS                                                 \
  [rseq_cs_addr] "m"(__rseq_abi.rseq_cs),                                    \
      [rseq_slabs_addr] "m"(*reinterpret_cast<volatile char*>(               \
          reinterpret_cast<uintptr_t>(&__rseq_abi) +                         \
          TCMALLOC_RSEQ_SLABS_OFFSET)),                                      \
      [rseq_sig] "n"(                                                        \
          TCMALLOC_PERCPU_RSEQ_SIGNATURE), /* Also pass common consts, there \
                                              is no cost to passing unused   \
                                              consts. */                     \
      [cached_slabs_bit] "n"(TCMALLOC_CACHED_SLABS_BIT),                     \
      [cached_slabs_mask_neg] "n"(~TCMALLOC_CACHED_SLABS_MASK)

// Store v to p (*p = v) if the current thread wasn't rescheduled
// (still has the slab pointer cached). Otherwise returns false.
template <typename T>
inline bool StoreCurrentCpu(volatile void* p, T v) {
  uintptr_t scratch = 0;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__x86_64__)
  asm(TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_StoreCurrentCpu)
          R"(
      xorq %[scratch], %[scratch]
      btq $%c[cached_slabs_bit], %[rseq_slabs_addr]
      jnc 5f
      movl $1, %k[scratch]
      movq %[v], %[p]
      5 :)"
      : [scratch] "=&r"(scratch)
      : TCMALLOC_RSEQ_INPUTS, [p] "m"(*static_cast<void* volatile*>(p)),
        [v] "r"(v)
      : "cc", "memory");
#elif TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__aarch64__)
  uintptr_t tmp;
  asm(TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_StoreCurrentCpu)
          R"(
      mov %[scratch], #0
      ldr %[tmp], %[rseq_slabs_addr]
      tbz %[tmp], #%c[cached_slabs_bit], 5f
      mov %[scratch], #1
      str %[v], %[p]
      5 :)"
      : [scratch] "=&r"(scratch), [tmp] "=&r"(tmp)
      : TCMALLOC_RSEQ_INPUTS, [p] "m"(*static_cast<void* volatile*>(p)),
        [v] "r"(v)
      : TCMALLOC_RSEQ_CLOBBER, "cc", "memory");
#endif
  return scratch;
}

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__x86_64__)
static inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool TcmallocSlab_Internal_Push(
    size_t size_class, void* item) {
  uintptr_t scratch, current;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  asm goto(
#else
  bool overflow;
  asm volatile(
#endif
      TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Push)
      // scratch = tcmalloc_slabs;
      "movq %[rseq_slabs_addr], %[scratch]\n"
      // if (scratch & TCMALLOC_CACHED_SLABS_MASK>) goto overflow_label;
      // scratch &= ~TCMALLOC_CACHED_SLABS_MASK;
      "btrq $%c[cached_slabs_bit], %[scratch]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "jnc %l[overflow_label]\n"
#else
      "jae 5f\n"  // ae==c
#endif
      // current = slabs->current;
      "movzwq (%[scratch], %[size_class], 8), %[current]\n"
      // if (ABSL_PREDICT_FALSE(current >= slabs->end)) { goto overflow_label; }
      "cmp 6(%[scratch], %[size_class], 8), %w[current]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "jae %l[overflow_label]\n"
#else
      "jae 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccae)
  // If so, the above code needs to explicitly set a ccae return value.
#endif
      "mov %[item], (%[scratch], %[current], 8)\n"
      "lea 1(%[current]), %[current]\n"
      "mov %w[current], (%[scratch], %[size_class], 8)\n"
      // Commit
      "5:\n"
      :
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      [overflow] "=@ccae"(overflow),
#endif
      [scratch] "=&r"(scratch), [current] "=&r"(current)
      : TCMALLOC_RSEQ_INPUTS, [size_class] "r"(size_class), [item] "r"(item)
      : "cc", "memory"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      : overflow_label
#endif
  );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  if (ABSL_PREDICT_FALSE(overflow)) {
    return false;
  }
  return true;
#else
  return true;
overflow_label:
  return false;
#endif
}
#endif  // defined(__x86_64__)

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__aarch64__)
static inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool TcmallocSlab_Internal_Push(
    size_t size_class, void* item) {
  uintptr_t region_start, scratch, end_ptr, end;
  // Multiply size_class by the bytesize of each header
  size_t size_class_lsl3 = size_class * 8;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  asm goto(
#else
  bool overflow;
  asm volatile(
#endif
      TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Push)
      // region_start = tcmalloc_slabs;
      "ldr %[region_start], %[rseq_slabs_addr]\n"
  // if (region_start & TCMALLOC_CACHED_SLABS_MASK) goto overflow_label;
  // region_start &= ~TCMALLOC_CACHED_SLABS_MASK;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "tbz %[region_start], #%c[cached_slabs_bit], %l[overflow_label]\n"
      "and %[region_start], %[region_start], #%c[cached_slabs_mask_neg]\n"
#else
      "subs %[region_start], %[region_start], %[cached_slabs_mask]\n"
      "b.ls 5f\n"
#endif
      // end_ptr = &(slab_headers[0]->end)
      "add %[end_ptr], %[region_start], #6\n"
      // scratch = slab_headers[size_class]->current (current index)
      "ldrh %w[scratch], [%[region_start], %[size_class_lsl3]]\n"
      // end = slab_headers[size_class]->end (end index)
      "ldrh %w[end], [%[end_ptr], %[size_class_lsl3]]\n"
      // if (ABSL_PREDICT_FALSE(end <= scratch)) { goto overflow_label; }
      "cmp %[end], %[scratch]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "b.ls %l[overflow_label]\n"
#else
      "b.ls 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccls)
  // If so, the above code needs to explicitly set a ccls return value.
#endif
      "str %[item], [%[region_start], %[scratch], LSL #3]\n"
      "add %w[scratch], %w[scratch], #1\n"
      "strh %w[scratch], [%[region_start], %[size_class_lsl3]]\n"
      // Commit
      "5:\n"
      : [end_ptr] "=&r"(end_ptr), [scratch] "=&r"(scratch), [end] "=&r"(end),
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
        [overflow] "=@ccls"(overflow),
#endif
        [region_start] "=&r"(region_start)
      : TCMALLOC_RSEQ_INPUTS,
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
        [cached_slabs_mask] "r"(TCMALLOC_CACHED_SLABS_MASK),
#endif
        [size_class_lsl3] "r"(size_class_lsl3), [item] "r"(item)
      : TCMALLOC_RSEQ_CLOBBER, "cc", "memory"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      : overflow_label
#endif
  );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  if (ABSL_PREDICT_FALSE(overflow)) {
    goto overflow_label;
  }
#endif
  return true;
overflow_label:
  return false;
}
#endif  // defined (__aarch64__)

inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool TcmallocSlab::Push(size_t size_class,
                                                            void* item) {
  ASSERT(size_class != 0);
  ASSERT(item != nullptr);
  // Speculatively annotate item as released to TSan.  We may not succeed in
  // pushing the item, but if we wait for the restartable sequence to succeed,
  // it may become visible to another thread before we can trigger the
  // annotation.
  TSANRelease(item);
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  return TcmallocSlab_Internal_Push(size_class, item);
#else
  return false;
#endif
}

// PrefetchNextObject provides a common code path across architectures for
// generating a prefetch of the next object.
//
// It is in a distinct, always-lined method to make its cost more transparent
// when profiling with debug information.
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void PrefetchNextObject(
    void* prefetch_target) {
  // A note about prefetcht0 in Pop:  While this prefetch may appear costly,
  // trace analysis shows the target is frequently used (b/70294962). Stalling
  // on a TLB miss at the prefetch site (which has no deps) and prefetching the
  // line async is better than stalling at the use (which may have deps) to fill
  // the TLB and the cache miss.
  //
  // See "Beyond malloc efficiency to fleet efficiency"
  // (https://research.google/pubs/pub50370/), section 6.4 for additional
  // details.
  //
  // TODO(b/214608320): Evaluate prefetch for write.
  __builtin_prefetch(prefetch_target, 0, 3);
}

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__x86_64__)
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* TcmallocSlab::Pop(size_t size_class) {
  ASSERT(size_class != 0);
  void* next;
  void* result;
  void* scratch;
  uintptr_t current;

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  asm goto(
#else
  bool underflow;
  asm(
#endif
      TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Pop)
      // scratch = tcmalloc_slabs;
      "movq %[rseq_slabs_addr], %[scratch]\n"
  // if (scratch & TCMALLOC_CACHED_SLABS_MASK) goto overflow_label;
  // scratch &= ~TCMALLOC_CACHED_SLABS_MASK;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "btrq $%c[cached_slabs_bit], %[scratch]\n"
      "jnc %l[underflow_path]\n"
#else
      "cmpq %[cached_slabs_mask], %[scratch]\n"
      "jbe 5f\n"
      "subq %[cached_slabs_mask], %[scratch]\n"
#endif
      // current = scratch->header[size_class].current;
      "movzwq (%[scratch], %[size_class], 8), %[current]\n"
      // if (ABSL_PREDICT_FALSE(current <=
      //                        scratch->header[size_class].begin))
      //   goto underflow_path;
      "cmp 4(%[scratch], %[size_class], 8), %w[current]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "jbe %l[underflow_path]\n"
#else
      "jbe 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccbe)
  // If so, the above code needs to explicitly set a ccbe return value.
#endif
      "movq -16(%[scratch], %[current], 8), %[next]\n"
      "movq -8(%[scratch], %[current], 8), %[result]\n"
      "lea -1(%[current]), %[current]\n"
      "mov %w[current], (%[scratch], %[size_class], 8)\n"
      // Commit
      "5:\n"
      : [result] "=&r"(result),
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
        [underflow] "=@ccbe"(underflow),
#endif
        [scratch] "=&r"(scratch), [current] "=&r"(current), [next] "=&r"(next)
      : TCMALLOC_RSEQ_INPUTS,
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
        [cached_slabs_mask] "r"(TCMALLOC_CACHED_SLABS_MASK),
#endif
        [size_class] "r"(size_class)
      : "cc", "memory"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      : underflow_path
#endif
  );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  if (ABSL_PREDICT_FALSE(underflow)) {
    goto underflow_path;
  }
#endif
  ASSERT(next);
  ASSERT(result);
  TSANAcquire(result);

  PrefetchNextObject(next);
  return AssumeNotNull(result);
underflow_path:
  return nullptr;
}
#endif  // defined(__x86_64__)

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__aarch64__)
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* TcmallocSlab::Pop(size_t size_class) {
  ASSERT(size_class != 0);
  void* result;
  void* region_start;
  void* prefetch;
  uintptr_t scratch;
  uintptr_t previous;
  uintptr_t begin;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  asm goto(
#else
  bool underflow;
  asm(
#endif
      TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Pop)
      // region_start = tcmalloc_slabs;
      "ldr %[region_start], %[rseq_slabs_addr]\n"
  // if (region_start & TCMALLOC_CACHED_SLABS_MASK) goto overflow_label;
  // region_start &= ~TCMALLOC_CACHED_SLABS_MASK;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "tbz %[region_start], #%c[cached_slabs_bit], %l[underflow_path]\n"
      "and %[region_start], %[region_start], #%c[cached_slabs_mask_neg]\n"
#else
      "subs %[region_start], %[region_start], %[cached_slabs_mask]\n"
      "b.ls 5f\n"
#endif
      // scratch = slab_headers[size_class]->current (current index)
      "ldrh %w[scratch], [%[region_start], %[size_class_lsl3]]\n"
      // begin = slab_headers[size_class]->begin (begin index)
      // Temporarily use begin as scratch.
      "add %[begin], %[size_class_lsl3], #4\n"
      "ldrh %w[begin], [%[region_start], %[begin]]\n"
      // if (ABSL_PREDICT_FALSE(begin >= scratch)) { goto underflow_path; }
      "cmp %w[scratch], %w[begin]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "b.ls %l[underflow_path]\n"
#else
      "b.ls 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccls)
  // If so, the above code needs to explicitly set a ccls return value.
#endif
      // scratch--
      "sub %w[scratch], %w[scratch], #1\n"
      "ldr %[result], [%[region_start], %[scratch], LSL #3]\n"
      "sub %w[previous], %w[scratch], #1\n"
      "ldr %[prefetch], [%[region_start], %[previous], LSL #3]\n"
      "strh %w[scratch], [%[region_start], %[size_class_lsl3]]\n"
      // Commit
      "5:\n"
      :
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      [underflow] "=@ccls"(underflow),
#endif
      [result] "=&r"(result), [prefetch] "=&r"(prefetch),
      // Temps
      [region_start] "=&r"(region_start), [previous] "=&r"(previous),
      [begin] "=&r"(begin), [scratch] "=&r"(scratch)
      // Real inputs
      : TCMALLOC_RSEQ_INPUTS,
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
        [cached_slabs_mask] "r"(TCMALLOC_CACHED_SLABS_MASK),
#endif
        [size_class] "r"(size_class), [size_class_lsl3] "r"(size_class << 3)
      : TCMALLOC_RSEQ_CLOBBER, "cc", "memory"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      : underflow_path
#endif
  );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  if (ABSL_PREDICT_FALSE(underflow)) {
    goto underflow_path;
  }
#endif
  TSANAcquire(result);
  PrefetchNextObject(prefetch);
  return AssumeNotNull(result);
underflow_path:
  return nullptr;
}
#endif  // defined(__aarch64__)

#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* TcmallocSlab::Pop(size_t size_class) {
  return nullptr;
}
#endif

inline size_t TcmallocSlab::Grow(
    int cpu, size_t size_class, size_t len,
    absl::FunctionRef<size_t(uint8_t)> max_capacity) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t max_cap = max_capacity(ToUint8(shift));
  std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
  Header hdr = LoadHeader(hdrp);
  ssize_t have = static_cast<ssize_t>(max_cap - (hdr.end - hdr.begin));
  if (hdr.IsLocked() || have <= 0) {
    return 0;
  }
  uint16_t n = std::min<uint16_t>(len, have);
  hdr.end += n;
  hdr.end_copy += n;
  return StoreCurrentCpu(hdrp, hdr) ? n : 0;
}

inline std::pair<int, bool> TcmallocSlab::CacheCpuSlab() {
  int cpu = VirtualRseqCpuId(virtual_cpu_id_offset_);
  ASSERT(cpu >= 0);
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  if (ABSL_PREDICT_FALSE((tcmalloc_slabs & TCMALLOC_CACHED_SLABS_MASK) == 0)) {
    return CacheCpuSlabSlow();
  }
  // We already have slab offset cached, so the slab is indeed full/empty.
#endif
  return {cpu, false};
}

inline void TcmallocSlab::UncacheCpuSlab() {
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  tcmalloc_slabs = 0;
#endif
}

inline size_t TcmallocSlab::PushBatch(size_t size_class, void** batch,
                                      size_t len) {
  ASSERT(size_class != 0);
  ASSERT(len != 0);
  // We need to annotate batch[...] as released before running the restartable
  // sequence, since those objects become visible to other threads the moment
  // the restartable sequence is complete and before the annotation potentially
  // runs.
  //
  // This oversynchronizes slightly, since PushBatch may succeed only partially.
  TSANReleaseBatch(batch, len);
  return TcmallocSlab_Internal_PushBatch(size_class, batch, len);
}

inline size_t TcmallocSlab::PopBatch(size_t size_class, void** batch,
                                     size_t len) {
  ASSERT(size_class != 0);
  ASSERT(len != 0);
  const size_t n = TcmallocSlab_Internal_PopBatch(size_class, batch, len);
  ASSERT(n <= len);

  // PopBatch is implemented in assembly, msan does not know that the returned
  // batch is initialized.
  ANNOTATE_MEMORY_IS_INITIALIZED(batch, n * sizeof(batch[0]));
  TSANAcquireBatch(batch, n);
  return n;
}

inline void* TcmallocSlab::CpuMemoryStart(void* slabs, Shift shift, int cpu) {
  return &static_cast<char*>(slabs)[cpu << ToUint8(shift)];
}

inline std::atomic<int64_t>* TcmallocSlab::GetHeader(void* slabs, Shift shift,
                                                     int cpu,
                                                     size_t size_class) {
  ASSERT(size_class != 0);
  return &static_cast<std::atomic<int64_t>*>(
      CpuMemoryStart(slabs, shift, cpu))[size_class];
}

inline auto TcmallocSlab::LoadHeader(std::atomic<int64_t>* hdrp) -> Header {
  return absl::bit_cast<Header>(hdrp->load(std::memory_order_relaxed));
}

inline void TcmallocSlab::StoreHeader(std::atomic<int64_t>* hdrp, Header hdr) {
  hdrp->store(absl::bit_cast<int64_t>(hdr), std::memory_order_relaxed);
}

inline void TcmallocSlab::LockHeader(void* slabs, Shift shift, int cpu,
                                     size_t size_class) {
  // Note: this reinterpret_cast and write in Lock lead to undefined
  // behavior, because the actual object type is std::atomic<int64_t>. But
  // C++ does not allow to legally express what we need here: atomic writes
  // of different sizes.
  reinterpret_cast<Header*>(GetHeader(slabs, shift, cpu, size_class))->Lock();
}

inline bool TcmallocSlab::Header::IsLocked() const {
  ASSERT(end != 0 || begin == 0 || begin == 0xffffu);
  // Checking end == 0 also covers the case of MADV_DONTNEEDed slabs after
  // a call to ResizeSlabs(). Such slabs are locked for any practical purposes.
  return end == 0;
}

inline void TcmallocSlab::Header::Lock() {
  // Write 0xffff to begin and 0 to end. This blocks new Push'es and Pop's.
  // Note: we write only 4 bytes. The first 4 bytes are left intact.
  // See Drain method for details. tl;dr: C++ does not allow us to legally
  // express this without undefined behavior.
  std::atomic<int32_t>* p =
      reinterpret_cast<std::atomic<int32_t>*>(&lock_update);
  Header hdr;
  hdr.begin = 0xffffu;
  hdr.end = 0;
  p->store(absl::bit_cast<int32_t>(hdr.lock_update), std::memory_order_relaxed);
}

}  // namespace percpu
}  // namespace subtle
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_PERCPU_TCMALLOC_H_
