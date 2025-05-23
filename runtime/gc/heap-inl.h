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

#ifndef ART_RUNTIME_GC_HEAP_INL_H_
#define ART_RUNTIME_GC_HEAP_INL_H_

#include "heap.h"

#include "allocation_listener.h"
#include "base/quasi_atomic.h"
#include "base/time_utils.h"
#include "gc/accounting/atomic_stack.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/allocation_record.h"
#include "gc/collector/semi_space.h"
#include "gc/space/bump_pointer_space-inl.h"
#include "gc/space/dlmalloc_space-inl.h"
#include "gc/space/large_object_space.h"
#include "gc/space/region_space-inl.h"
#include "gc/space/rosalloc_space-inl.h"
#include "handle_scope-inl.h"
#include "obj_ptr-inl.h"
#include "runtime.h"
#include "thread-inl.h"
#include "verify_object.h"
#include "write_barrier-inl.h"

namespace art {
namespace gc {

template <bool kInstrumented, bool kCheckLargeObject, typename PreFenceVisitor>
inline mirror::Object* Heap::AllocObjectWithAllocator(Thread* self,
                                                      ObjPtr<mirror::Class> klass,
                                                      size_t byte_count,
                                                      AllocatorType allocator,
                                                      const PreFenceVisitor& pre_fence_visitor) {
  auto no_suspend_pre_fence_visitor =
      [&pre_fence_visitor](auto... x) REQUIRES_SHARED(Locks::mutator_lock_) {
        ScopedAssertNoThreadSuspension sants("No thread suspension during pre-fence visitor");
        pre_fence_visitor(x...);
      };

  if (kIsDebugBuild) {
    CheckPreconditionsForAllocObject(klass, byte_count);
    // Since allocation can cause a GC which will need to SuspendAll, make sure all allocations are
    // done in the runnable state where suspension is expected.
    CHECK_EQ(self->GetState(), kRunnable);
    self->AssertThreadSuspensionIsAllowable();
    self->AssertNoPendingException();
    // Make sure to preserve klass.
    StackHandleScope<1> hs(self);
    HandleWrapperObjPtr<mirror::Class> h = hs.NewHandleWrapper(&klass);
    self->PoisonObjectPointers();
  }
  auto pre_object_allocated = [&]() REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Roles::uninterruptible_ /* only suspends if kInstrumented */) {
    if constexpr (kInstrumented) {
      AllocationListener* l = alloc_listener_.load(std::memory_order_seq_cst);
      if (UNLIKELY(l != nullptr) && UNLIKELY(l->HasPreAlloc())) {
        StackHandleScope<1> hs(self);
        HandleWrapperObjPtr<mirror::Class> h_klass(hs.NewHandleWrapper(&klass));
        l->PreObjectAllocated(self, h_klass, &byte_count);
      }
    }
  };
  ObjPtr<mirror::Object> obj;
  // bytes allocated for the (individual) object.
  size_t bytes_allocated;
  size_t usable_size;
  size_t new_num_bytes_allocated = 0;
  bool need_gc = false;
  uint32_t starting_gc_num;  // o.w. GC number at which we observed need for GC.
  {
    // Bytes allocated that includes bulk thread-local buffer allocations in addition to direct
    // non-TLAB object allocations. Only set for non-thread-local allocation,
    size_t bytes_tl_bulk_allocated = 0u;
    // Do the initial pre-alloc
    // TODO: Consider what happens if the allocator is switched while suspended here.
    pre_object_allocated();

    // Need to check that we aren't the large object allocator since the large object allocation
    // code path includes this function. If we didn't check we would have an infinite loop.
    if (kCheckLargeObject && UNLIKELY(ShouldAllocLargeObject(klass, byte_count))) {
      // AllocLargeObject can suspend and will recall PreObjectAllocated if needed.
      obj = AllocLargeObject<kInstrumented, PreFenceVisitor>(self, &klass, byte_count,
                                                             pre_fence_visitor);
      if (obj != nullptr) {
      // marvin start
      // Added by Niel: we avoid swapping out LOS objects created by the zygote because
      // they might be referenced from the zygote space, and there's no easy way of
      // walking the zygote space.
      if (Runtime::Current()->IsZygote()) {
        obj->SetNoSwapFlag();
      }
      // marvin end
        return obj.Ptr();
      }
      // There should be an OOM exception, since we are retrying, clear it.
      self->ClearException();

      // If the large object allocation failed, try to use the normal spaces (main space,
      // non moving space). This can happen if there is significant virtual address space
      // fragmentation.
      // kInstrumented may be out of date, so recurse without large object checking, rather than
      // continue.
      return AllocObjectWithAllocator</*kInstrumented=*/ true, /*kCheckLargeObject=*/ false>
          (self, klass, byte_count, GetUpdatedAllocator(allocator), pre_fence_visitor);
    }
    ScopedAssertNoThreadSuspension ants("Called PreObjectAllocated, no suspend until alloc");
    if (IsTLABAllocator(allocator)) {
      byte_count = RoundUp(byte_count, space::BumpPointerSpace::kAlignment);
    }
    // If we have a thread local allocation we don't need to update bytes allocated.
    if (IsTLABAllocator(allocator) && byte_count <= self->TlabSize()) {
      obj = self->AllocTlab(byte_count);
      DCHECK(obj != nullptr) << "AllocTlab can't fail";
      obj->SetClass(klass);
      if (kUseBakerReadBarrier) {
        obj->AssertReadBarrierState();
      }
      bytes_allocated = byte_count;
      usable_size = bytes_allocated;
      no_suspend_pre_fence_visitor(obj, usable_size);
      QuasiAtomic::ThreadFenceForConstructor();
    } else if (
        !kInstrumented && allocator == kAllocatorTypeRosAlloc &&
        (obj = rosalloc_space_->AllocThreadLocal(self, byte_count, &bytes_allocated)) != nullptr &&
        LIKELY(obj != nullptr)) {
      DCHECK(!is_running_on_memory_tool_);
      obj->SetClass(klass);
      if (kUseBakerReadBarrier) {
        obj->AssertReadBarrierState();
      }
      usable_size = bytes_allocated;
      no_suspend_pre_fence_visitor(obj, usable_size);
      QuasiAtomic::ThreadFenceForConstructor();
    } else {
      obj = TryToAllocate<kInstrumented, false>(self, allocator, byte_count, &bytes_allocated,
                                                &usable_size, &bytes_tl_bulk_allocated);
      if (UNLIKELY(obj == nullptr)) {
        // AllocateInternalWithGc internally re-allows, and can cause, thread suspension, if
        // someone instruments the entrypoints or changes the allocator in a suspend point here,
        // we need to retry the allocation. It will send the pre-alloc event again.
        obj = AllocateInternalWithGc(self,
                                     allocator,
                                     kInstrumented,
                                     byte_count,
                                     &bytes_allocated,
                                     &usable_size,
                                     &bytes_tl_bulk_allocated,
                                     &klass);
        if (obj == nullptr) {
          // The only way that we can get a null return if there is no pending exception is if the
          // allocator or instrumentation changed.
          if (!self->IsExceptionPending()) {
            // Since we are restarting, allow thread suspension.
            ScopedAllowThreadSuspension ats;
            // AllocObject will pick up the new allocator type, and instrumented as true is the safe
            // default.
            return AllocObjectWithAllocator</*kInstrumented=*/true>(self,
                                                                    klass,
                                                                    byte_count,
                                                                    GetUpdatedAllocator(allocator),
                                                                    pre_fence_visitor);
          }
          return nullptr;
        }
        // Non-null result implies neither instrumentation nor allocator changed.
      }
      DCHECK_GT(bytes_allocated, 0u);
      DCHECK_GT(usable_size, 0u);
      obj->SetClass(klass);
      if (kUseBakerReadBarrier) {
        obj->AssertReadBarrierState();
      }
      if (collector::SemiSpace::kUseRememberedSet &&
          UNLIKELY(allocator == kAllocatorTypeNonMoving)) {
        // (Note this if statement will be constant folded away for the fast-path quick entry
        // points.) Because SetClass() has no write barrier, the GC may need a write barrier in the
        // case the object is non movable and points to a recently allocated movable class.
        WriteBarrier::ForFieldWrite(obj, mirror::Object::ClassOffset(), klass);
      }
      no_suspend_pre_fence_visitor(obj, usable_size);
      QuasiAtomic::ThreadFenceForConstructor();
    }
    if (bytes_tl_bulk_allocated > 0) {
      starting_gc_num = GetCurrentGcNum();
      size_t num_bytes_allocated_before =
          num_bytes_allocated_.fetch_add(bytes_tl_bulk_allocated, std::memory_order_relaxed);
      new_num_bytes_allocated = num_bytes_allocated_before + bytes_tl_bulk_allocated;
      // Only trace when we get an increase in the number of bytes allocated. This happens when
      // obtaining a new TLAB and isn't often enough to hurt performance according to golem.
      if (region_space_) {
        // With CC collector, during a GC cycle, the heap usage increases as
        // there are two copies of evacuated objects. Therefore, add evac-bytes
        // to the heap size. When the GC cycle is not running, evac-bytes
        // are 0, as required.
        TraceHeapSize(new_num_bytes_allocated + region_space_->EvacBytes());
      } else {
        TraceHeapSize(new_num_bytes_allocated);
      }
      // IsGcConcurrent() isn't known at compile time so we can optimize by not checking it for the
      // BumpPointer or TLAB allocators. This is nice since it allows the entire if statement to be
      // optimized out. And for the other allocators, AllocatorMayHaveConcurrentGC is a constant
      // since the allocator_type should be constant propagated.
      if (AllocatorMayHaveConcurrentGC(allocator) && IsGcConcurrent()
          && UNLIKELY(ShouldConcurrentGCForJava(new_num_bytes_allocated))) {
        need_gc = true;
      }
      GetMetrics()->TotalBytesAllocated()->Add(bytes_tl_bulk_allocated);
    }
  }
  if (kIsDebugBuild && Runtime::Current()->IsStarted()) {
    CHECK_LE(obj->SizeOf(), usable_size);
  }
  // TODO: Deprecate.
  if (kInstrumented) {
    if (Runtime::Current()->HasStatsEnabled()) {
      RuntimeStats* thread_stats = self->GetStats();
      ++thread_stats->allocated_objects;
      thread_stats->allocated_bytes += bytes_allocated;
      RuntimeStats* global_stats = Runtime::Current()->GetStats();
      ++global_stats->allocated_objects;
      global_stats->allocated_bytes += bytes_allocated;
    }
  } else {
    DCHECK(!Runtime::Current()->HasStatsEnabled());
  }
  if (kInstrumented) {
    if (IsAllocTrackingEnabled()) {
      // allocation_records_ is not null since it never becomes null after allocation tracking is
      // enabled.
      DCHECK(allocation_records_ != nullptr);
      allocation_records_->RecordAllocation(self, &obj, bytes_allocated);
    }
    AllocationListener* l = alloc_listener_.load(std::memory_order_seq_cst);
    if (l != nullptr) {
      // Same as above. We assume that a listener that was once stored will never be deleted.
      // Otherwise we'd have to perform this under a lock.
      l->ObjectAllocated(self, &obj, bytes_allocated);
    }
  } else {
    DCHECK(!IsAllocTrackingEnabled());
  }
  if (AllocatorHasAllocationStack(allocator)) {
    PushOnAllocationStack(self, &obj);
  }
  if (kInstrumented) {
    if (gc_stress_mode_) {
      CheckGcStressMode(self, &obj);
    }
  } else {
    DCHECK(!gc_stress_mode_);
  }
  if (need_gc) {
    // Do this only once thread suspension is allowed again, and we're done with kInstrumented.
    RequestConcurrentGCAndSaveObject(self, /*force_full=*/ false, starting_gc_num, &obj);
  }
  VerifyObject(obj);
  self->VerifyStack();
  return obj.Ptr();
}

// The size of a thread-local allocation stack in the number of references.
static constexpr size_t kThreadLocalAllocationStackSize = 128;

inline void Heap::PushOnAllocationStack(Thread* self, ObjPtr<mirror::Object>* obj) {
  if (kUseThreadLocalAllocationStack) {
    if (UNLIKELY(!self->PushOnThreadLocalAllocationStack(obj->Ptr()))) {
      PushOnThreadLocalAllocationStackWithInternalGC(self, obj);
    }
  } else if (UNLIKELY(!allocation_stack_->AtomicPushBack(obj->Ptr()))) {
    PushOnAllocationStackWithInternalGC(self, obj);
  }
}

template <bool kInstrumented, typename PreFenceVisitor>
inline mirror::Object* Heap::AllocLargeObject(Thread* self,
                                              ObjPtr<mirror::Class>* klass,
                                              size_t byte_count,
                                              const PreFenceVisitor& pre_fence_visitor) {
  // Save and restore the class in case it moves.
  StackHandleScope<1> hs(self);
  auto klass_wrapper = hs.NewHandleWrapper(klass);
  mirror::Object* obj = AllocObjectWithAllocator<kInstrumented, false, PreFenceVisitor>
                        (self, *klass, byte_count, kAllocatorTypeLOS, pre_fence_visitor);
  // Java Heap Profiler check and sample allocation.
  JHPCheckNonTlabSampleAllocation(self, obj, byte_count);
  return obj;
}

template <const bool kInstrumented, const bool kGrow>
inline mirror::Object* Heap::TryToAllocate(Thread* self,
                                           AllocatorType allocator_type,
                                           size_t alloc_size,
                                           size_t* bytes_allocated,
                                           size_t* usable_size,
                                           size_t* bytes_tl_bulk_allocated) {
  if (allocator_type != kAllocatorTypeRegionTLAB &&
      allocator_type != kAllocatorTypeTLAB &&
      allocator_type != kAllocatorTypeRosAlloc &&
      UNLIKELY(IsOutOfMemoryOnAllocation(allocator_type, alloc_size, kGrow))) {
    return nullptr;
  }
  mirror::Object* ret;
  switch (allocator_type) {
    case kAllocatorTypeBumpPointer: {
      DCHECK(bump_pointer_space_ != nullptr);
      alloc_size = RoundUp(alloc_size, space::BumpPointerSpace::kAlignment);
      ret = bump_pointer_space_->AllocNonvirtual(alloc_size);
      if (LIKELY(ret != nullptr)) {
        *bytes_allocated = alloc_size;
        *usable_size = alloc_size;
        *bytes_tl_bulk_allocated = alloc_size;
      }
      break;
    }
    case kAllocatorTypeRosAlloc: {
      if (kInstrumented && UNLIKELY(is_running_on_memory_tool_)) {
        // If running on ASan, we should be using the instrumented path.
        size_t max_bytes_tl_bulk_allocated = rosalloc_space_->MaxBytesBulkAllocatedFor(alloc_size);
        if (UNLIKELY(IsOutOfMemoryOnAllocation(allocator_type,
                                               max_bytes_tl_bulk_allocated,
                                               kGrow))) {
          return nullptr;
        }
        ret = rosalloc_space_->Alloc(self, alloc_size, bytes_allocated, usable_size,
                                     bytes_tl_bulk_allocated);
      } else {
        DCHECK(!is_running_on_memory_tool_);
        size_t max_bytes_tl_bulk_allocated =
            rosalloc_space_->MaxBytesBulkAllocatedForNonvirtual(alloc_size);
        if (UNLIKELY(IsOutOfMemoryOnAllocation(allocator_type,
                                               max_bytes_tl_bulk_allocated,
                                               kGrow))) {
          return nullptr;
        }
        if (!kInstrumented) {
          DCHECK(!rosalloc_space_->CanAllocThreadLocal(self, alloc_size));
        }
        ret = rosalloc_space_->AllocNonvirtual(self,
                                               alloc_size,
                                               bytes_allocated,
                                               usable_size,
                                               bytes_tl_bulk_allocated);
      }
      break;
    }
    case kAllocatorTypeDlMalloc: {
      if (kInstrumented && UNLIKELY(is_running_on_memory_tool_)) {
        // If running on ASan, we should be using the instrumented path.
        ret = dlmalloc_space_->Alloc(self,
                                     alloc_size,
                                     bytes_allocated,
                                     usable_size,
                                     bytes_tl_bulk_allocated);
      } else {
        DCHECK(!is_running_on_memory_tool_);
        ret = dlmalloc_space_->AllocNonvirtual(self,
                                               alloc_size,
                                               bytes_allocated,
                                               usable_size,
                                               bytes_tl_bulk_allocated);
      }
      break;
    }
    case kAllocatorTypeNonMoving: {
      ret = non_moving_space_->Alloc(self,
                                     alloc_size,
                                     bytes_allocated,
                                     usable_size,
                                     bytes_tl_bulk_allocated);
      break;
    }
    case kAllocatorTypeLOS: {
      ret = large_object_space_->Alloc(self,
                                       alloc_size,
                                       bytes_allocated,
                                       usable_size,
                                       bytes_tl_bulk_allocated);
      // Note that the bump pointer spaces aren't necessarily next to
      // the other continuous spaces like the non-moving alloc space or
      // the zygote space.
      DCHECK(ret == nullptr || large_object_space_->Contains(ret));
      break;
    }
    case kAllocatorTypeRegion: {
      DCHECK(region_space_ != nullptr);
      alloc_size = RoundUp(alloc_size, space::RegionSpace::kAlignment);
      ret = region_space_->AllocNonvirtual<false>(alloc_size,
                                                  bytes_allocated,
                                                  usable_size,
                                                  bytes_tl_bulk_allocated);
      break;
    }
    case kAllocatorTypeTLAB:
      FALLTHROUGH_INTENDED;
    case kAllocatorTypeRegionTLAB: {
      DCHECK_ALIGNED(alloc_size, kObjectAlignment);
      static_assert(space::RegionSpace::kAlignment == space::BumpPointerSpace::kAlignment,
                    "mismatched alignments");
      static_assert(kObjectAlignment == space::BumpPointerSpace::kAlignment,
                    "mismatched alignments");
      if (UNLIKELY(self->TlabSize() < alloc_size)) {
        return AllocWithNewTLAB(self,
                                allocator_type,
                                alloc_size,
                                kGrow,
                                bytes_allocated,
                                usable_size,
                                bytes_tl_bulk_allocated);
      }
      // The allocation can't fail.
      ret = self->AllocTlab(alloc_size);
      DCHECK(ret != nullptr);
      *bytes_allocated = alloc_size;
      *bytes_tl_bulk_allocated = 0;  // Allocated in an existing buffer.
      *usable_size = alloc_size;
      break;
    }
    default: {
      LOG(FATAL) << "Invalid allocator type";
      ret = nullptr;
    }
  }
  return ret;
}

inline bool Heap::ShouldAllocLargeObject(ObjPtr<mirror::Class> c, size_t byte_count) const {
  // We need to have a zygote space or else our newly allocated large object can end up in the
  // Zygote resulting in it being prematurely freed.
  // We can only do this for primitive objects since large objects will not be within the card table
  // range. This also means that we rely on SetClass not dirtying the object's card.
  return byte_count >= large_object_threshold_ && (c->IsPrimitiveArray() || c->IsStringClass());
}

inline bool Heap::IsOutOfMemoryOnAllocation(AllocatorType allocator_type,
                                            size_t alloc_size,
                                            bool grow) {
  size_t old_target = target_footprint_.load(std::memory_order_relaxed);
  while (true) {
    size_t old_allocated = num_bytes_allocated_.load(std::memory_order_relaxed);
    size_t new_footprint = old_allocated + alloc_size;
    // Tests against heap limits are inherently approximate, since multiple allocations may
    // race, and this is not atomic with the allocation.
    if (UNLIKELY(new_footprint <= old_target)) {
      return false;
    } else if (UNLIKELY(new_footprint > growth_limit_)) {
      return true;
    }
    // We are between target_footprint_ and growth_limit_ .
    if (AllocatorMayHaveConcurrentGC(allocator_type) && IsGcConcurrent()) {
      return false;
    } else {
      if (grow) {
        if (target_footprint_.compare_exchange_weak(/*inout ref*/old_target, new_footprint,
                                                    std::memory_order_relaxed)) {
          VlogHeapGrowth(old_target, new_footprint, alloc_size);
          return false;
        }  // else try again.
      } else {
        return true;
      }
    }
  }
}

inline bool Heap::ShouldConcurrentGCForJava(size_t new_num_bytes_allocated) {
  // For a Java allocation, we only check whether the number of Java allocated bytes excceeds a
  // threshold. By not considering native allocation here, we (a) ensure that Java heap bounds are
  // maintained, and (b) reduce the cost of the check here.
  return new_num_bytes_allocated >= concurrent_start_bytes_;
}

}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_HEAP_INL_H_
