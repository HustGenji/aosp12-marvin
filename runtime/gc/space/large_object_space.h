/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_GC_SPACE_LARGE_OBJECT_SPACE_H_
#define ART_RUNTIME_GC_SPACE_LARGE_OBJECT_SPACE_H_

#include "base/allocator.h"
#include "base/safe_map.h"
#include "base/tracking_safe_map.h"
#include "dlmalloc_space.h"
#include "space.h"
#include "thread-current-inl.h"

#include <set>
#include <vector>

namespace art {
namespace gc {
namespace space {

class AllocationInfo;

enum class LargeObjectSpaceType {
  kDisabled,
  kMap,
  kFreeList,
};

// Abstraction implemented by all large object spaces.
class LargeObjectSpace : public DiscontinuousSpace, public AllocSpace {
 public:
  SpaceType GetType() const override {
    return kSpaceTypeLargeObjectSpace;
  }
  void SwapBitmaps();
  void CopyLiveToMarked();
  virtual void Walk(DlMallocSpace::WalkCallback, void* arg) = 0;
  virtual ~LargeObjectSpace() {}

  uint64_t GetBytesAllocated() override {
    MutexLock mu(Thread::Current(), lock_);
    return num_bytes_allocated_;
  }
  uint64_t GetObjectsAllocated() override {
    MutexLock mu(Thread::Current(), lock_);
    return num_objects_allocated_;
  }
  uint64_t GetTotalBytesAllocated() const {
    MutexLock mu(Thread::Current(), lock_);
    return total_bytes_allocated_;
  }
  uint64_t GetTotalObjectsAllocated() const {
    MutexLock mu(Thread::Current(), lock_);
    return total_objects_allocated_;
  }
  size_t FreeList(Thread* self, size_t num_ptrs, mirror::Object** ptrs) override;
  // LargeObjectSpaces don't have thread local state.
  size_t RevokeThreadLocalBuffers(art::Thread*) override {
    return 0U;
  }
  size_t RevokeAllThreadLocalBuffers() override {
    return 0U;
  }
  bool IsAllocSpace() const override {
    return true;
  }
  AllocSpace* AsAllocSpace() override {
    return this;
  }
  collector::ObjectBytePair Sweep(bool swap_bitmaps);
  bool CanMoveObjects() const override {
    return false;
  }
  // Current address at which the space begins, which may vary as the space is filled.
  uint8_t* Begin() const {
    return begin_;
  }
  // Current address at which the space ends, which may vary as the space is filled.
  uint8_t* End() const {
    return end_;
  }
  // Current size of space
  size_t Size() const {
    return End() - Begin();
  }
  // Return true if we contain the specified address.
  bool Contains(const mirror::Object* obj) const override {
    const uint8_t* byte_obj = reinterpret_cast<const uint8_t*>(obj);
    return Begin() <= byte_obj && byte_obj < End();
  }
  bool LogFragmentationAllocFailure(std::ostream& os, size_t failed_alloc_bytes) override
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Return true if the large object is a zygote large object. Potentially slow.
  virtual bool IsZygoteLargeObject(Thread* self, mirror::Object* obj) const = 0;
  // Called when we create the zygote space, mark all existing large objects as zygote large
  // objects. Set mark-bit if called from PreZygoteFork() for ConcurrentCopying
  // GC to avoid dirtying the first page.
  virtual void SetAllLargeObjectsAsZygoteObjects(Thread* self, bool set_mark_bit) = 0;

  virtual void ForEachMemMap(std::function<void(const MemMap&)> func) const = 0;
  // GetRangeAtomic returns Begin() and End() atomically, that is, it never returns Begin() and
  // End() from different allocations.
  virtual std::pair<uint8_t*, uint8_t*> GetBeginEndAtomic() const = 0;

 protected:
  explicit LargeObjectSpace(const std::string& name, uint8_t* begin, uint8_t* end,
                            const char* lock_name);
  static void SweepCallback(size_t num_ptrs, mirror::Object** ptrs, void* arg);

  // Used to ensure mutual exclusion when the allocation spaces data structures,
  // including the allocation counters below, are being modified.
  mutable Mutex lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;

  // Number of bytes which have been allocated into the space and not yet freed. The count is also
  // included in the identically named field in Heap. Counts actual allocated (after rounding),
  // not requested, sizes. TODO: It would be cheaper to just maintain total allocated and total
  // free counts.
  uint64_t num_bytes_allocated_ GUARDED_BY(lock_);
  uint64_t num_objects_allocated_ GUARDED_BY(lock_);

  // Totals for large objects ever allocated, including those that have since been deallocated.
  // Never decremented.
  uint64_t total_bytes_allocated_ GUARDED_BY(lock_);
  uint64_t total_objects_allocated_ GUARDED_BY(lock_);

  // Begin and end, may change as more large objects are allocated.
  uint8_t* begin_;
  uint8_t* end_;

  friend class Space;

 private:
  DISALLOW_COPY_AND_ASSIGN(LargeObjectSpace);
};

// A discontinuous large object space implemented by individual mmap/munmap calls.
class LargeObjectMapSpace : public LargeObjectSpace {
 public:
  // Creates a large object space. Allocations into the large object space use memory maps instead
  // of malloc.
  static LargeObjectMapSpace* Create(const std::string& name);

  // jiacheng start
  static LargeObjectMapSpace* JiachengCreate(const std::string& name, uint8_t* start, size_t capacity);
  // jiacheng end

  // Return the storage space required by obj.
  size_t AllocationSize(mirror::Object* obj, size_t* usable_size) override REQUIRES(!lock_);
  mirror::Object* Alloc(Thread* self, size_t num_bytes, size_t* bytes_allocated,
                        size_t* usable_size, size_t* bytes_tl_bulk_allocated) override
      REQUIRES(!lock_);
  size_t Free(Thread* self, mirror::Object* ptr) override REQUIRES(!lock_);
  void Walk(DlMallocSpace::WalkCallback, void* arg) override REQUIRES(!lock_);
  // TODO: disabling thread safety analysis as this may be called when we already hold lock_.
  bool Contains(const mirror::Object* obj) const override NO_THREAD_SAFETY_ANALYSIS;
  void ForEachMemMap(std::function<void(const MemMap&)> func) const override REQUIRES(!lock_);
  std::pair<uint8_t*, uint8_t*> GetBeginEndAtomic() const override REQUIRES(!lock_);

 protected:
  struct LargeObject {
    MemMap mem_map;
    bool is_zygote;
  };
  explicit LargeObjectMapSpace(const std::string& name);
  virtual ~LargeObjectMapSpace() {}

  bool IsZygoteLargeObject(Thread* self, mirror::Object* obj) const override REQUIRES(!lock_);
  void SetAllLargeObjectsAsZygoteObjects(Thread* self, bool set_mark_bit) override
      REQUIRES(!lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  AllocationTrackingSafeMap<mirror::Object*, LargeObject, kAllocatorTagLOSMaps> large_objects_
      GUARDED_BY(lock_);
};

// A continuous large object space with a free-list to handle holes.
class FreeListSpace final : public LargeObjectSpace {
 public:
  static constexpr size_t kAlignment = kPageSize;

  virtual ~FreeListSpace();
  static FreeListSpace* Create(const std::string& name, size_t capacity);

  // jiacheng start
  static FreeListSpace* JiachengCreate(const std::string& name, uint8_t* start, size_t capacity);
  // jiacheng end

  size_t AllocationSize(mirror::Object* obj, size_t* usable_size) override
      REQUIRES(lock_);
  mirror::Object* Alloc(Thread* self, size_t num_bytes, size_t* bytes_allocated,
                        size_t* usable_size, size_t* bytes_tl_bulk_allocated)
      override REQUIRES(!lock_);
  size_t Free(Thread* self, mirror::Object* obj) override REQUIRES(!lock_);
  void Walk(DlMallocSpace::WalkCallback callback, void* arg) override REQUIRES(!lock_);
  void Dump(std::ostream& os) const override REQUIRES(!lock_);
  void ForEachMemMap(std::function<void(const MemMap&)> func) const override REQUIRES(!lock_);
  std::pair<uint8_t*, uint8_t*> GetBeginEndAtomic() const override REQUIRES(!lock_);

 protected:
  FreeListSpace(const std::string& name, MemMap&& mem_map, uint8_t* begin, uint8_t* end);
  size_t GetSlotIndexForAddress(uintptr_t address) const {
    DCHECK(Contains(reinterpret_cast<mirror::Object*>(address)));
    return (address - reinterpret_cast<uintptr_t>(Begin())) / kAlignment;
  }
  size_t GetSlotIndexForAllocationInfo(const AllocationInfo* info) const;
  AllocationInfo* GetAllocationInfoForAddress(uintptr_t address);
  const AllocationInfo* GetAllocationInfoForAddress(uintptr_t address) const;
  uintptr_t GetAllocationAddressForSlot(size_t slot) const {
    return reinterpret_cast<uintptr_t>(Begin()) + slot * kAlignment;
  }
  uintptr_t GetAddressForAllocationInfo(const AllocationInfo* info) const {
    return GetAllocationAddressForSlot(GetSlotIndexForAllocationInfo(info));
  }
  // Removes header from the free blocks set by finding the corresponding iterator and erasing it.
  void RemoveFreePrev(AllocationInfo* info) REQUIRES(lock_);
  bool IsZygoteLargeObject(Thread* self, mirror::Object* obj) const override;
  void SetAllLargeObjectsAsZygoteObjects(Thread* self, bool set_mark_bit) override
      REQUIRES(!lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  class SortByPrevFree {
   public:
    bool operator()(const AllocationInfo* a, const AllocationInfo* b) const;
  };
  typedef std::set<AllocationInfo*, SortByPrevFree,
                   TrackingAllocator<AllocationInfo*, kAllocatorTagLOSFreeList>> FreeBlocks;

  // There is not footer for any allocations at the end of the space, so we keep track of how much
  // free space there is at the end manually.
  MemMap mem_map_;
  // Side table for allocation info, one per page.
  MemMap allocation_info_map_;
  AllocationInfo* allocation_info_;

  // Free bytes at the end of the space.
  size_t free_end_ GUARDED_BY(lock_);
  FreeBlocks free_blocks_ GUARDED_BY(lock_);
};

}  // namespace space
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_SPACE_LARGE_OBJECT_SPACE_H_
