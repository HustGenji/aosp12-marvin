/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_RUNTIME_GC_SPACE_BUMP_POINTER_SPACE_H_
#define ART_RUNTIME_GC_SPACE_BUMP_POINTER_SPACE_H_

#include "space.h"

#include "base/mutex.h"

// jiacheng start
#include <android-base/logging.h>
// jiacheng end

namespace art {

namespace mirror {
class Object;
}

namespace gc {

namespace collector {
class MarkSweep;
}  // namespace collector

namespace space {

// A bump pointer space allocates by incrementing a pointer, it doesn't provide a free
// implementation as its intended to be evacuated.
class BumpPointerSpace final : public ContinuousMemMapAllocSpace {
 public:
  typedef void(*WalkCallback)(void *start, void *end, size_t num_bytes, void* callback_arg);

  SpaceType GetType() const override {
    return kSpaceTypeBumpPointerSpace;
  }

  // Create a bump pointer space with the requested sizes. The requested base address is not
  // guaranteed to be granted, if it is required, the caller should call Begin on the returned
  // space to confirm the request was granted.
  static BumpPointerSpace* Create(const std::string& name, size_t capacity);
  static BumpPointerSpace* CreateFromMemMap(const std::string& name, MemMap&& mem_map);

  // Allocate num_bytes, returns null if the space is full.
  mirror::Object* Alloc(Thread* self, size_t num_bytes, size_t* bytes_allocated,
                        size_t* usable_size, size_t* bytes_tl_bulk_allocated) override;
  // Thread-unsafe allocation for when mutators are suspended, used by the semispace collector.
  mirror::Object* AllocThreadUnsafe(Thread* self, size_t num_bytes, size_t* bytes_allocated,
                                    size_t* usable_size, size_t* bytes_tl_bulk_allocated)
      override REQUIRES(Locks::mutator_lock_);

  mirror::Object* AllocNonvirtual(size_t num_bytes);
  mirror::Object* AllocNonvirtualWithoutAccounting(size_t num_bytes);

  // Return the storage space required by obj.
  size_t AllocationSize(mirror::Object* obj, size_t* usable_size) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return AllocationSizeNonvirtual(obj, usable_size);
  }

  // NOPS unless we support free lists.
  size_t Free(Thread*, mirror::Object*) override {
    return 0;
  }

  size_t FreeList(Thread*, size_t, mirror::Object**) override {
    return 0;
  }

  size_t AllocationSizeNonvirtual(mirror::Object* obj, size_t* usable_size)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Removes the fork time growth limit on capacity, allowing the application to allocate up to the
  // maximum reserved size of the heap.
  void ClearGrowthLimit() {
    growth_end_ = Limit();
  }

  // Override capacity so that we only return the possibly limited capacity
  size_t Capacity() const override {
    return growth_end_ - begin_;
  }

  // The total amount of memory reserved for the space.
  size_t NonGrowthLimitCapacity() const override {
    return GetMemMap()->Size();
  }

  accounting::ContinuousSpaceBitmap* GetLiveBitmap() override {
    return nullptr;
  }

  accounting::ContinuousSpaceBitmap* GetMarkBitmap() override {
    return nullptr;
  }

  // Reset the space to empty.
  void Clear() override REQUIRES(!block_lock_);

  void Dump(std::ostream& os) const override;

  size_t RevokeThreadLocalBuffers(Thread* thread) override REQUIRES(!block_lock_);
  size_t RevokeAllThreadLocalBuffers() override
      REQUIRES(!Locks::runtime_shutdown_lock_, !Locks::thread_list_lock_, !block_lock_);
  void AssertThreadLocalBuffersAreRevoked(Thread* thread) REQUIRES(!block_lock_);
  void AssertAllThreadLocalBuffersAreRevoked()
      REQUIRES(!Locks::runtime_shutdown_lock_, !Locks::thread_list_lock_, !block_lock_);

  uint64_t GetBytesAllocated() override REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*Locks::runtime_shutdown_lock_, !*Locks::thread_list_lock_, !block_lock_);
  uint64_t GetObjectsAllocated() override REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!*Locks::runtime_shutdown_lock_, !*Locks::thread_list_lock_, !block_lock_);
  bool IsEmpty() const {
    return Begin() == End();
  }

  bool CanMoveObjects() const override {
    return true;
  }

  bool Contains(const mirror::Object* obj) const override {
    const uint8_t* byte_obj = reinterpret_cast<const uint8_t*>(obj);
    return byte_obj >= Begin() && byte_obj < End();
  }

  // TODO: Change this? Mainly used for compacting to a particular region of memory.
  BumpPointerSpace(const std::string& name, uint8_t* begin, uint8_t* limit);

  // Return the object which comes after obj, while ensuring alignment.
  static mirror::Object* GetNextObject(mirror::Object* obj)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Allocate a new TLAB, returns false if the allocation failed.
  bool AllocNewTlab(Thread* self, size_t bytes) REQUIRES(!block_lock_);

  BumpPointerSpace* AsBumpPointerSpace() override {
    return this;
  }

  // Go through all of the blocks and visit the continuous objects.
  template <typename Visitor>
  ALWAYS_INLINE void Walk(Visitor&& visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!block_lock_);

  accounting::ContinuousSpaceBitmap::SweepCallback* GetSweepCallback() override;

  // Record objects / bytes freed.
  void RecordFree(int32_t objects, int32_t bytes) {
    objects_allocated_.fetch_sub(objects, std::memory_order_relaxed);
    bytes_allocated_.fetch_sub(bytes, std::memory_order_relaxed);
  }

  bool LogFragmentationAllocFailure(std::ostream& os, size_t failed_alloc_bytes) override
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Object alignment within the space.
  static constexpr size_t kAlignment = 8;

 protected:
  BumpPointerSpace(const std::string& name, MemMap&& mem_map);

  // Allocate a raw block of bytes.
  uint8_t* AllocBlock(size_t bytes) REQUIRES(block_lock_);
  void RevokeThreadLocalBuffersLocked(Thread* thread) REQUIRES(block_lock_);

  // The main block is an unbounded block where objects go when there are no other blocks. This
  // enables us to maintain tightly packed objects when you are not using thread local buffers for
  // allocation. The main block starts at the space Begin().
  void UpdateMainBlock() REQUIRES(block_lock_);

  uint8_t* growth_end_;
  AtomicInteger objects_allocated_;  // Accumulated from revoked thread local regions.
  AtomicInteger bytes_allocated_;  // Accumulated from revoked thread local regions.
  Mutex block_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  // The objects at the start of the space are stored in the main block. The main block doesn't
  // have a header, this lets us walk empty spaces which are mprotected.
  size_t main_block_size_ GUARDED_BY(block_lock_);
  // The number of blocks in the space, if it is 0 then the space has one long continuous block
  // which doesn't have an updated header.
  size_t num_blocks_ GUARDED_BY(block_lock_);

 private:
  struct BlockHeader {
    size_t size_;  // Size of the block in bytes, does not include the header.
    size_t unused_;  // Ensures alignment of kAlignment.
  };

  static_assert(sizeof(BlockHeader) % kAlignment == 0,
                "continuous block must be kAlignment aligned");

  friend class collector::MarkSweep;
  DISALLOW_COPY_AND_ASSIGN(BumpPointerSpace);
};

}  // namespace space
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_SPACE_BUMP_POINTER_SPACE_H_
