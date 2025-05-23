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

#ifndef ART_RUNTIME_ENTRYPOINTS_QUICK_QUICK_ENTRYPOINTS_H_
#define ART_RUNTIME_ENTRYPOINTS_QUICK_QUICK_ENTRYPOINTS_H_

#include <jni.h>

#include "base/locks.h"
#include "base/macros.h"
#include "deoptimization_kind.h"
#include "offsets.h"

// marvin start
#include "niel_swap.h"
// marvin end

#define QUICK_ENTRYPOINT_OFFSET(ptr_size, x) \
    Thread::QuickEntryPointOffset<ptr_size>(OFFSETOF_MEMBER(QuickEntryPoints, x))

namespace art {

namespace mirror {
class Array;
class Class;
template<class MirrorType> class CompressedReference;
class Object;
class String;
}  // namespace mirror

class ArtMethod;
template<class MirrorType> class GcRoot;
template<class MirrorType> class StackReference;
class Thread;

// Pointers to functions that are called by quick compiler generated code via thread-local storage.
struct PACKED(4) QuickEntryPoints {
#define ENTRYPOINT_ENUM(name, rettype, ...) rettype ( * p ## name )( __VA_ARGS__ );
#include "quick_entrypoints_list.h"
  QUICK_ENTRYPOINT_LIST(ENTRYPOINT_ENUM)
#undef QUICK_ENTRYPOINT_LIST
#undef ENTRYPOINT_ENUM
};


// JNI entrypoints.
// TODO: NO_THREAD_SAFETY_ANALYSIS due to different control paths depending on fast JNI.
extern uint32_t JniMethodStart(Thread* self) NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;
extern uint32_t JniMethodFastStart(Thread* self) NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;
extern uint32_t JniMethodStartSynchronized(jobject to_lock, Thread* self)
    NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;
extern void JniMethodEnd(uint32_t saved_local_ref_cookie, Thread* self)
    NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;
extern void JniMethodFastEnd(uint32_t saved_local_ref_cookie, Thread* self)
    NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;
extern void JniMethodEndSynchronized(uint32_t saved_local_ref_cookie, jobject locked,
                                     Thread* self)
    NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;
extern mirror::Object* JniMethodEndWithReference(jobject result, uint32_t saved_local_ref_cookie,
                                                 Thread* self)
    NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;
extern mirror::Object* JniMethodFastEndWithReference(jobject result,
                                                     uint32_t saved_local_ref_cookie,
                                                     Thread* self)
    NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;


extern mirror::Object* JniMethodEndWithReferenceSynchronized(jobject result,
                                                             uint32_t saved_local_ref_cookie,
                                                             jobject locked, Thread* self)
    NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;

// JNI entrypoints when monitoring entry/exit.
extern uint32_t JniMonitoredMethodStart(Thread* self) NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;
extern uint32_t JniMonitoredMethodStartSynchronized(jobject to_lock, Thread* self)
    NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;
extern void JniMonitoredMethodEnd(uint32_t saved_local_ref_cookie, Thread* self)
    NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;
extern void JniMonitoredMethodEndSynchronized(uint32_t saved_local_ref_cookie,
                                              jobject locked,
                                              Thread* self)
    NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;
extern mirror::Object* JniMonitoredMethodEndWithReference(jobject result,
                                                          uint32_t saved_local_ref_cookie,
                                                          Thread* self)
    NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;

extern mirror::Object* JniMonitoredMethodEndWithReferenceSynchronized(
    jobject result,
    uint32_t saved_local_ref_cookie,
    jobject locked, Thread* self)
    NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;


extern "C" mirror::String* artStringBuilderAppend(uint32_t format,
                                                  const uint32_t* args,
                                                  Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) HOT_ATTR;

extern void ReadBarrierJni(mirror::CompressedReference<mirror::Class>* handle_on_stack,
                           Thread* self)
    NO_THREAD_SAFETY_ANALYSIS HOT_ATTR;

// Read barrier entrypoints.
//
// Compilers for ARM, ARM64 can insert a call to these
// functions directly.  For x86 and x86-64, compilers need a wrapper
// assembly function, to handle mismatch in ABI.

// Mark the heap reference `obj`. This entry point is used by read
// barrier fast path implementations generated by the compiler to mark
// an object that is referenced by a field of a gray object.
extern "C" mirror::Object* artReadBarrierMark(mirror::Object* obj)
    REQUIRES_SHARED(Locks::mutator_lock_) HOT_ATTR;

// Read barrier entrypoint for heap references.
// This is the read barrier slow path for instance and static fields
// and reference type arrays.
extern "C" mirror::Object* artReadBarrierSlow(mirror::Object* ref,
                                              mirror::Object* obj,
                                              uint32_t offset)
    REQUIRES_SHARED(Locks::mutator_lock_) HOT_ATTR;

// Read barrier entrypoint for GC roots.
extern "C" mirror::Object* artReadBarrierForRootSlow(GcRoot<mirror::Object>* root)
    REQUIRES_SHARED(Locks::mutator_lock_) HOT_ATTR;

}  // namespace art

#endif  // ART_RUNTIME_ENTRYPOINTS_QUICK_QUICK_ENTRYPOINTS_H_
