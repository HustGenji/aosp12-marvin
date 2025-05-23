/*
 * Copyright (C) 2011 The Android Open Source Project
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
 * 
 * // marvin start
  *
  * Note from Niel: SWAP_PREAMBLE macro calls were added to this file
  * automatically using the script insert-object-hooks.pl. This script is
  * in my "android-swap-code" repository in the "swap-hook-generation"
  * folder.
 // marvin end
 */

#ifndef ART_RUNTIME_MIRROR_OBJECT_INL_H_
#define ART_RUNTIME_MIRROR_OBJECT_INL_H_

#include "object.h"

#include "array-inl.h"
#include "art_field.h"
#include "art_method.h"
#include "base/atomic.h"
#include "class-inl.h"
#include "class_flags.h"
#include "class_linker.h"
#include "dex_cache.h"
#include "heap_poisoning.h"
#include "lock_word-inl.h"
#include "monitor.h"
#include "obj_ptr-inl.h"
#include "object-readbarrier-inl.h"
#include "object_array-inl.h"
#include "object_reference-inl.h"
#include "read_barrier-inl.h"
#include "reference.h"
#include "runtime.h"
#include "string.h"
#include "throwable.h"
#include "write_barrier-inl.h"

// marvin start
#include "niel_swap.h"
// marvin end

namespace art {
namespace mirror {

inline uint32_t Object::ClassSize(PointerSize pointer_size) {
  uint32_t vtable_entries = kVTableLength;
  return Class::ComputeClassSize(true, vtable_entries, 0, 0, 0, 0, 0, pointer_size);
}

template<VerifyObjectFlags kVerifyFlags, ReadBarrierOption kReadBarrierOption>
inline Class* Object::GetClass() {
  return GetFieldObject<Class, kVerifyFlags, kReadBarrierOption>(ClassOffset());
}

template<VerifyObjectFlags kVerifyFlags>
inline void Object::SetClass(ObjPtr<Class> new_klass) {
  // new_klass may be null prior to class linker initialization.
  // We don't mark the card as this occurs as part of object allocation. Not all objects have
  // backing cards, such as large objects.
  // We use non transactional version since we can't undo this write. We also disable checking as
  // we may run in transaction mode here.
  SetFieldObjectWithoutWriteBarrier<false, false, RemoveThisFlags(kVerifyFlags)>(ClassOffset(),
                                                                                 new_klass);
}

template<VerifyObjectFlags kVerifyFlags>
inline void Object::SetLockWord(LockWord new_val, bool as_volatile) {
  // Force use of non-transactional mode and do not check.
  if (as_volatile) {
    SetField32Volatile<false, false, kVerifyFlags>(MonitorOffset(), new_val.GetValue());
  } else {
    SetField32<false, false, kVerifyFlags>(MonitorOffset(), new_val.GetValue());
  }
}

inline uint32_t Object::GetLockOwnerThreadId() {
  return Monitor::GetLockOwnerThreadId(this);
}

inline ObjPtr<mirror::Object> Object::MonitorEnter(Thread* self) {
  return Monitor::MonitorEnter(self, this, /*trylock=*/false);
}

inline ObjPtr<mirror::Object> Object::MonitorTryEnter(Thread* self) {
  return Monitor::MonitorEnter(self, this, /*trylock=*/true);
}

inline bool Object::MonitorExit(Thread* self) {
  return Monitor::MonitorExit(self, this);
}

inline void Object::Notify(Thread* self) {
  Monitor::Notify(self, this);
}

inline void Object::NotifyAll(Thread* self) {
  Monitor::NotifyAll(self, this);
}

inline void Object::Wait(Thread* self, int64_t ms, int32_t ns) {
  Monitor::Wait(self, this, ms, ns, true, kTimedWaiting);
}

inline uint32_t Object::GetMarkBit() {
  CHECK(kUseReadBarrier);
  return GetLockWord(false).MarkBitState();
}

inline void Object::SetReadBarrierState(uint32_t rb_state) {
  CHECK(kUseBakerReadBarrier);
  DCHECK(ReadBarrier::IsValidReadBarrierState(rb_state)) << rb_state;
  LockWord lw = GetLockWord(false);
  lw.SetReadBarrierState(rb_state);
  SetLockWord(lw, false);
}

inline void Object::AssertReadBarrierState() const {
  CHECK(kUseBakerReadBarrier);
  Object* obj = const_cast<Object*>(this);
  DCHECK_EQ(obj->GetReadBarrierState(), ReadBarrier::NonGrayState())
      << "Bad Baker pointer: obj=" << obj << " rb_state" << obj->GetReadBarrierState();
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::VerifierInstanceOf(ObjPtr<Class> klass) {
  DCHECK(klass != nullptr);
  DCHECK(GetClass<kVerifyFlags>() != nullptr);
  return klass->IsInterface() || InstanceOf(klass);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::InstanceOf(ObjPtr<Class> klass) {
  DCHECK(klass != nullptr);
  DCHECK(GetClass<kVerifyNone>() != nullptr) << "this=" << this;
  return klass->IsAssignableFrom(GetClass<kVerifyFlags>());
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsClass() {
  // OK to look at from-space copies since java.lang.Class.class is non-moveable
  // (even when running without boot image, see ClassLinker::InitWithoutImage())
  // and we're reading constant references for comparison only. See ReadBarrierOption.
  ObjPtr<Class> klass = GetClass<kVerifyFlags, kWithoutReadBarrier>();
  ObjPtr<Class> java_lang_Class = klass->GetClass<kVerifyFlags, kWithoutReadBarrier>();
  return klass == java_lang_Class;
}

template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<Class> Object::AsClass() {
  DCHECK((IsClass<kVerifyFlags>()));
  return ObjPtr<Class>::DownCast(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsObjectArray() {
  // We do not need a read barrier here as the primitive type is constant,
  // both from-space and to-space component type classes shall yield the same result.
  constexpr VerifyObjectFlags kNewFlags = RemoveThisFlags(kVerifyFlags);
  return IsArrayInstance<kVerifyFlags>() &&
      !GetClass<kNewFlags, kWithoutReadBarrier>()->
          template GetComponentType<kNewFlags, kWithoutReadBarrier>()->IsPrimitive();
}

template<class T, VerifyObjectFlags kVerifyFlags>
inline ObjPtr<ObjectArray<T>> Object::AsObjectArray() {
  DCHECK((IsObjectArray<kVerifyFlags>()));
  return ObjPtr<ObjectArray<T>>::DownCast(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsArrayInstance() {
  // We do not need a read barrier here, both from-space and to-space version of the class
  // shall return the same result from IsArrayClass().
  return GetClass<kVerifyFlags, kWithoutReadBarrier>()->template IsArrayClass<kVerifyFlags>();
}

template<VerifyObjectFlags kVerifyFlags, ReadBarrierOption kReadBarrierOption>
inline bool Object::IsReferenceInstance() {
  return GetClass<kVerifyFlags, kReadBarrierOption>()->IsTypeOfReferenceClass();
}

template<VerifyObjectFlags kVerifyFlags, ReadBarrierOption kReadBarrierOption>
inline ObjPtr<Reference> Object::AsReference() {
  DCHECK((IsReferenceInstance<kVerifyFlags, kReadBarrierOption>()));
  return ObjPtr<Reference>::DownCast(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<Array> Object::AsArray() {
  DCHECK((IsArrayInstance<kVerifyFlags>()));
  return ObjPtr<Array>::DownCast(this);
}

template<VerifyObjectFlags kVerifyFlags, Primitive::Type kType>
ALWAYS_INLINE bool Object::IsSpecificPrimitiveArray() {
  // We do not need a read barrier here as the primitive type is constant, both from-space
  // and to-space component type classes shall yield the same result. See ReadBarrierOption.
  const ObjPtr<Class> klass = GetClass<kVerifyFlags, kWithoutReadBarrier>();
  constexpr VerifyObjectFlags kNewFlags = RemoveThisFlags(kVerifyFlags);
  const ObjPtr<Class> component_type = klass->GetComponentType<kNewFlags, kWithoutReadBarrier>();
  return component_type != nullptr &&
         component_type->GetPrimitiveType<kNewFlags>() == kType;
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsBooleanArray() {
  return IsSpecificPrimitiveArray<kVerifyFlags, Primitive::kPrimBoolean>();
}

template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<BooleanArray> Object::AsBooleanArray() {
  DCHECK(IsBooleanArray<kVerifyFlags>());
  return ObjPtr<BooleanArray>::DownCast(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsByteArray() {
  return IsSpecificPrimitiveArray<kVerifyFlags, Primitive::kPrimByte>();
}

template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<ByteArray> Object::AsByteArray() {
  DCHECK(IsByteArray<kVerifyFlags>());
  return ObjPtr<ByteArray>::DownCast(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsCharArray() {
  return IsSpecificPrimitiveArray<kVerifyFlags, Primitive::kPrimChar>();
}

template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<CharArray> Object::AsCharArray() {
  DCHECK(IsCharArray<kVerifyFlags>());
  return ObjPtr<CharArray>::DownCast(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsShortArray() {
  return IsSpecificPrimitiveArray<kVerifyFlags, Primitive::kPrimShort>();
}

template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<ShortArray> Object::AsShortArray() {
  DCHECK(IsShortArray<kVerifyFlags>());
  return ObjPtr<ShortArray>::DownCast(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsIntArray() {
  return IsSpecificPrimitiveArray<kVerifyFlags, Primitive::kPrimInt>();
}

template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<IntArray> Object::AsIntArrayUnchecked() {
  return ObjPtr<IntArray>::DownCast(this);
}
template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<IntArray> Object::AsIntArray() {
  DCHECK((IsIntArray<kVerifyFlags>()));
  return AsIntArrayUnchecked<kVerifyFlags>();
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsLongArray() {
  return IsSpecificPrimitiveArray<kVerifyFlags, Primitive::kPrimLong>();
}

template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<LongArray> Object::AsLongArrayUnchecked() {
  return ObjPtr<LongArray>::DownCast(this);
}
template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<LongArray> Object::AsLongArray() {
  DCHECK((IsLongArray<kVerifyFlags>()));
  return AsLongArrayUnchecked<kVerifyFlags>();
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsFloatArray() {
  return IsSpecificPrimitiveArray<kVerifyFlags, Primitive::kPrimFloat>();
}

template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<FloatArray> Object::AsFloatArray() {
  DCHECK(IsFloatArray<kVerifyFlags>());
  return ObjPtr<FloatArray>::DownCast(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsDoubleArray() {
  return IsSpecificPrimitiveArray<kVerifyFlags, Primitive::kPrimDouble>();
}

template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<DoubleArray> Object::AsDoubleArray() {
  DCHECK(IsDoubleArray<kVerifyFlags>());
  return ObjPtr<DoubleArray>::DownCast(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsString() {
  // No read barrier is needed for reading a constant primitive field through
  // constant reference field. See ReadBarrierOption.
  return GetClass<kVerifyFlags, kWithoutReadBarrier>()->IsStringClass();
}

template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<String> Object::AsString() {
  DCHECK((IsString<kVerifyFlags>()));
  return ObjPtr<String>::DownCast(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<Throwable> Object::AsThrowable() {
  DCHECK(GetClass<kVerifyFlags>()->IsThrowableClass());
  return ObjPtr<Throwable>::DownCast(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsWeakReferenceInstance() {
  return GetClass<kVerifyFlags>()->IsWeakReferenceClass();
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsSoftReferenceInstance() {
  return GetClass<kVerifyFlags>()->IsSoftReferenceClass();
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsFinalizerReferenceInstance() {
  return GetClass<kVerifyFlags>()->IsFinalizerReferenceClass();
}

template<VerifyObjectFlags kVerifyFlags>
inline ObjPtr<FinalizerReference> Object::AsFinalizerReference() {
  DCHECK(IsFinalizerReferenceInstance<kVerifyFlags>());
  return ObjPtr<FinalizerReference>::DownCast(this);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Object::IsPhantomReferenceInstance() {
  return GetClass<kVerifyFlags>()->IsPhantomReferenceClass();
}

template<VerifyObjectFlags kVerifyFlags>
inline size_t Object::SizeOf() {
  // marvin start
  SWAP_PREAMBLE_TEMPLATE(SizeOf, Object, size_t, GATHER_TEMPLATE_ARGS(kVerifyFlags), )
  // marvin end

  // Read barrier is never required for SizeOf since objects sizes are constant. Reading from-space
  // values is OK because of that.
  size_t result;
  constexpr VerifyObjectFlags kNewFlags = RemoveThisFlags(kVerifyFlags);
  if (IsArrayInstance<kVerifyFlags>()) {
    result = AsArray<kNewFlags>()->template SizeOf<kNewFlags>();
  } else if (IsClass<kNewFlags>()) {
    result = AsClass<kNewFlags>()->template SizeOf<kNewFlags>();
  } else if (IsString<kNewFlags>()) {
    result = AsString<kNewFlags>()->template SizeOf<kNewFlags>();
  } else {
    result = GetClass<kNewFlags, kWithoutReadBarrier>()->template GetObjectSize<kNewFlags>();
  }
  DCHECK_GE(result, sizeof(Object)) << " class="
      // Note: Class::PrettyClass() is reading constant reference fields to get to constant
      // primitive fields and safely avoids read barriers, so it is safe to call on a Class
      // reference read without read barrier from a constant reference field.
      // See ReadBarrierOption. And, for correctness, we actually have to avoid the read
      // barrier here if Object::SizeOf() is called on a from-space reference.
      << GetClass<kNewFlags, kWithoutReadBarrier>()->PrettyClass();
  return result;
}

template<VerifyObjectFlags kVerifyFlags, bool kIsVolatile>
inline int8_t Object::GetFieldByte(MemberOffset field_offset) {
  // marvin start
  if (field_offset.Uint32Value() >= sizeof(Object)) {
    SWAP_PREAMBLE_TEMPLATE(GetFieldByte, Object, int8_t, GATHER_TEMPLATE_ARGS(kVerifyFlags, kIsVolatile), field_offset)
  }
  // marvin end
  Verify<kVerifyFlags>();
  return GetFieldPrimitive<int8_t, kIsVolatile>(field_offset);
}

template<VerifyObjectFlags kVerifyFlags>
inline uint8_t Object::GetFieldBooleanVolatile(MemberOffset field_offset) {
  return GetFieldBoolean<kVerifyFlags, true>(field_offset);
}

template<VerifyObjectFlags kVerifyFlags>
inline int8_t Object::GetFieldByteVolatile(MemberOffset field_offset) {
  return GetFieldByte<kVerifyFlags, true>(field_offset);
}

template<bool kTransactionActive,
         bool kCheckTransaction,
         VerifyObjectFlags kVerifyFlags,
         bool kIsVolatile>
inline void Object::SetFieldBoolean(MemberOffset field_offset, uint8_t new_value) {
  // marvin start
  if (field_offset.Uint32Value() >= sizeof(Object)) {
    SWAP_PREAMBLE_TEMPLATE_VOID(SetFieldBoolean, Object, GATHER_TEMPLATE_ARGS(kTransactionActive, kCheckTransaction, kVerifyFlags, kIsVolatile), field_offset, new_value)
  }
  // marvin end
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteFieldBoolean(
        this,
        field_offset,
        GetFieldBoolean<kVerifyFlags, kIsVolatile>(field_offset),
        kIsVolatile);
  }
  Verify<kVerifyFlags>();
  SetFieldPrimitive<uint8_t, kIsVolatile>(field_offset, new_value);
}

template<bool kTransactionActive,
         bool kCheckTransaction,
         VerifyObjectFlags kVerifyFlags,
         bool kIsVolatile>
inline void Object::SetFieldByte(MemberOffset field_offset, int8_t new_value) {
  // marvin start
  if (field_offset.Uint32Value() >= sizeof(Object)) {
    SWAP_PREAMBLE_TEMPLATE_VOID(SetFieldByte, Object, GATHER_TEMPLATE_ARGS(kTransactionActive, kCheckTransaction, kVerifyFlags, kIsVolatile), field_offset, new_value)
  }
  // marvin end
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteFieldByte(this,
                                             field_offset,
                                             GetFieldByte<kVerifyFlags, kIsVolatile>(field_offset),
                                             kIsVolatile);
  }
  Verify<kVerifyFlags>();
  SetFieldPrimitive<int8_t, kIsVolatile>(field_offset, new_value);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::SetFieldBooleanVolatile(MemberOffset field_offset, uint8_t new_value) {
  return SetFieldBoolean<kTransactionActive, kCheckTransaction, kVerifyFlags, true>(
      field_offset, new_value);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::SetFieldByteVolatile(MemberOffset field_offset, int8_t new_value) {
  return SetFieldByte<kTransactionActive, kCheckTransaction, kVerifyFlags, true>(
      field_offset, new_value);
}

template<VerifyObjectFlags kVerifyFlags, bool kIsVolatile>
inline uint16_t Object::GetFieldChar(MemberOffset field_offset) {
  // marvin start
  if (field_offset.Uint32Value() >= sizeof(Object)) {
    SWAP_PREAMBLE_TEMPLATE(GetFieldChar, Object, uint16_t, GATHER_TEMPLATE_ARGS(kVerifyFlags, kIsVolatile), field_offset)
  }
  // marvin end
  Verify<kVerifyFlags>();
  return GetFieldPrimitive<uint16_t, kIsVolatile>(field_offset);
}

template<VerifyObjectFlags kVerifyFlags, bool kIsVolatile>
inline int16_t Object::GetFieldShort(MemberOffset field_offset) {
  // marvin start
  if (field_offset.Uint32Value() >= sizeof(Object)) {
    SWAP_PREAMBLE_TEMPLATE(GetFieldShort, Object, int16_t, GATHER_TEMPLATE_ARGS(kVerifyFlags, kIsVolatile), field_offset)
  }
  // marvin end
  Verify<kVerifyFlags>();
  return GetFieldPrimitive<int16_t, kIsVolatile>(field_offset);
}

template<VerifyObjectFlags kVerifyFlags>
inline uint16_t Object::GetFieldCharVolatile(MemberOffset field_offset) {
  return GetFieldChar<kVerifyFlags, true>(field_offset);
}

template<VerifyObjectFlags kVerifyFlags>
inline int16_t Object::GetFieldShortVolatile(MemberOffset field_offset) {
  return GetFieldShort<kVerifyFlags, true>(field_offset);
}

template<bool kTransactionActive,
         bool kCheckTransaction,
         VerifyObjectFlags kVerifyFlags,
         bool kIsVolatile>
inline void Object::SetFieldChar(MemberOffset field_offset, uint16_t new_value) {
  // marvin start
  if (field_offset.Uint32Value() >= sizeof(Object)) {
    SWAP_PREAMBLE_TEMPLATE_VOID(SetFieldChar, Object, GATHER_TEMPLATE_ARGS(kTransactionActive, kCheckTransaction, kVerifyFlags, kIsVolatile), field_offset, new_value)
  }
  // marvin end
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteFieldChar(this,
                                             field_offset,
                                             GetFieldChar<kVerifyFlags, kIsVolatile>(field_offset),
                                             kIsVolatile);
  }
  Verify<kVerifyFlags>();
  SetFieldPrimitive<uint16_t, kIsVolatile>(field_offset, new_value);
}

template<bool kTransactionActive,
         bool kCheckTransaction,
         VerifyObjectFlags kVerifyFlags,
         bool kIsVolatile>
inline void Object::SetFieldShort(MemberOffset field_offset, int16_t new_value) {
  // marvin start
  if (field_offset.Uint32Value() >= sizeof(Object)) {
    SWAP_PREAMBLE_TEMPLATE_VOID(SetFieldShort, Object, GATHER_TEMPLATE_ARGS(kTransactionActive, kCheckTransaction, kVerifyFlags, kIsVolatile), field_offset, new_value)
  }
  // marvin end
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteFieldChar(this,
                                             field_offset,
                                             GetFieldShort<kVerifyFlags, kIsVolatile>(field_offset),
                                             kIsVolatile);
  }
  Verify<kVerifyFlags>();
  SetFieldPrimitive<int16_t, kIsVolatile>(field_offset, new_value);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::SetFieldCharVolatile(MemberOffset field_offset, uint16_t new_value) {
  return SetFieldChar<kTransactionActive, kCheckTransaction, kVerifyFlags, true>(
      field_offset, new_value);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::SetFieldShortVolatile(MemberOffset field_offset, int16_t new_value) {
  return SetFieldShort<kTransactionActive, kCheckTransaction, kVerifyFlags, true>(
      field_offset, new_value);
}

template<bool kTransactionActive,
         bool kCheckTransaction,
         VerifyObjectFlags kVerifyFlags,
         bool kIsVolatile>
inline void Object::SetField32(MemberOffset field_offset, int32_t new_value) {
  // marvin start
  if (field_offset.Uint32Value() >= sizeof(Object)) {
    SWAP_PREAMBLE_TEMPLATE_VOID(SetField32, Object, GATHER_TEMPLATE_ARGS(kTransactionActive, kCheckTransaction, kVerifyFlags, kIsVolatile), field_offset, new_value)
  }
  // marvin end
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteField32(this,
                                           field_offset,
                                           GetField32<kVerifyFlags, kIsVolatile>(field_offset),
                                           kIsVolatile);
  }
  Verify<kVerifyFlags>();
  SetFieldPrimitive<int32_t, kIsVolatile>(field_offset, new_value);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::SetField32Volatile(MemberOffset field_offset, int32_t new_value) {
  SetField32<kTransactionActive, kCheckTransaction, kVerifyFlags, true>(field_offset, new_value);
}

template<bool kCheckTransaction, VerifyObjectFlags kVerifyFlags, bool kIsVolatile>
inline void Object::SetField32Transaction(MemberOffset field_offset, int32_t new_value) {
  if (Runtime::Current()->IsActiveTransaction()) {
    SetField32<true, kCheckTransaction, kVerifyFlags, kIsVolatile>(field_offset, new_value);
  } else {
    SetField32<false, kCheckTransaction, kVerifyFlags, kIsVolatile>(field_offset, new_value);
  }
}

template<bool kTransactionActive,
         bool kCheckTransaction,
         VerifyObjectFlags kVerifyFlags,
         bool kIsVolatile>
inline void Object::SetField64(MemberOffset field_offset, int64_t new_value) {
  // marvin start
  if (field_offset.Uint32Value() >= sizeof(Object)) {
    SWAP_PREAMBLE_TEMPLATE_VOID(SetField64, Object, GATHER_TEMPLATE_ARGS(kTransactionActive, kCheckTransaction, kVerifyFlags, kIsVolatile), field_offset, new_value)
  }
  // marvin end
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteField64(this,
                                           field_offset,
                                           GetField64<kVerifyFlags, kIsVolatile>(field_offset),
                                           kIsVolatile);
  }
  Verify<kVerifyFlags>();
  SetFieldPrimitive<int64_t, kIsVolatile>(field_offset, new_value);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::SetField64Volatile(MemberOffset field_offset, int64_t new_value) {
  // marvin start
  if (field_offset.Uint32Value() >= sizeof(Object)) {
    SWAP_PREAMBLE_TEMPLATE_VOID(SetField64Volatile, Object, GATHER_TEMPLATE_ARGS(kTransactionActive, kCheckTransaction, kVerifyFlags), field_offset, new_value)
  }
  // marvin end
  return SetField64<kTransactionActive, kCheckTransaction, kVerifyFlags, true>(field_offset,
                                                                               new_value);
}

template<bool kCheckTransaction, VerifyObjectFlags kVerifyFlags, bool kIsVolatile>
inline void Object::SetField64Transaction(MemberOffset field_offset, int32_t new_value) {
  if (Runtime::Current()->IsActiveTransaction()) {
    SetField64<true, kCheckTransaction, kVerifyFlags, kIsVolatile>(field_offset, new_value);
  } else {
    SetField64<false, kCheckTransaction, kVerifyFlags, kIsVolatile>(field_offset, new_value);
  }
}

template<typename kSize>
inline kSize Object::GetFieldAcquire(MemberOffset field_offset) {
  const uint8_t* raw_addr = reinterpret_cast<const uint8_t*>(this) + field_offset.Int32Value();
  const kSize* addr = reinterpret_cast<const kSize*>(raw_addr);
  return reinterpret_cast<const Atomic<kSize>*>(addr)->load(std::memory_order_acquire);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline bool Object::CasFieldWeakSequentiallyConsistent64(MemberOffset field_offset,
                                                         int64_t old_value,
                                                         int64_t new_value) {
  // marvin start
  if (field_offset.Uint32Value() >= sizeof(Object)) {
    SWAP_PREAMBLE_TEMPLATE(CasFieldWeakSequentiallyConsistent64, Object, bool, GATHER_TEMPLATE_ARGS(kTransactionActive, kCheckTransaction, kVerifyFlags), field_offset, old_value, new_value)
  }
  // marvin end
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteField64(this, field_offset, old_value, true);
  }
  Verify<kVerifyFlags>();
  uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
  Atomic<int64_t>* atomic_addr = reinterpret_cast<Atomic<int64_t>*>(raw_addr);
  return atomic_addr->CompareAndSetWeakSequentiallyConsistent(old_value, new_value);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline bool Object::CasFieldStrongSequentiallyConsistent64(MemberOffset field_offset,
                                                           int64_t old_value,
                                                           int64_t new_value) {
  // marvin start
  if (field_offset.Uint32Value() >= sizeof(Object)) {
    SWAP_PREAMBLE_TEMPLATE(CasFieldStrongSequentiallyConsistent64, Object, bool, GATHER_TEMPLATE_ARGS(kTransactionActive, kCheckTransaction, kVerifyFlags), field_offset, old_value, new_value)
  }
  // marvin end
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteField64(this, field_offset, old_value, true);
  }
  Verify<kVerifyFlags>();
  uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
  Atomic<int64_t>* atomic_addr = reinterpret_cast<Atomic<int64_t>*>(raw_addr);
  return atomic_addr->CompareAndSetStrongSequentiallyConsistent(old_value, new_value);
}

/*
 * Returns a pointer to an object representing what the field points to, not an
 * object representing the field.
 */
template<class T,
         VerifyObjectFlags kVerifyFlags,
         ReadBarrierOption kReadBarrierOption,
         bool kIsVolatile>
inline T* Object::GetFieldObject(MemberOffset field_offset) {
  // marvin start
  if (field_offset.Uint32Value() >= sizeof(Object)) {
    SWAP_PREAMBLE_TEMPLATE(GetFieldObject, Object, T*, GATHER_TEMPLATE_ARGS(T, kVerifyFlags, kReadBarrierOption, kIsVolatile), field_offset)
  }
  // marvin end

  // marvin start
  if (!GetIgnoreReadFlag()) {
      SetReadBit();
  }
  // marvin end
  Verify<kVerifyFlags>();
  uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
  HeapReference<T>* objref_addr = reinterpret_cast<HeapReference<T>*>(raw_addr);
  T* result = ReadBarrier::Barrier<T, kIsVolatile, kReadBarrierOption>(
      this,
      field_offset,
      objref_addr);
  VerifyRead<kVerifyFlags>(result);
  return result;
}

template<class T, VerifyObjectFlags kVerifyFlags, ReadBarrierOption kReadBarrierOption>
inline T* Object::GetFieldObjectVolatile(MemberOffset field_offset) {
  return GetFieldObject<T, kVerifyFlags, kReadBarrierOption, true>(field_offset);
}

template<bool kTransactionActive,
         bool kCheckTransaction,
         VerifyObjectFlags kVerifyFlags,
         bool kIsVolatile>
inline void Object::SetFieldObjectWithoutWriteBarrier(MemberOffset field_offset,
                                                      ObjPtr<Object> new_value) {
  // marvin start
  if (UNLIKELY(GetStubFlag())) {
    niel::swap::Stub * stub = (niel::swap::Stub *)this;
    stub->LockTableEntry();
    if (UNLIKELY(!stub->GetTableEntry()->GetResidentBit())) {
      niel::swap::SwapInOnDemand(stub);
    }
    ((Object *)(stub->GetObjectAddress()))->SetFieldObjectWithoutWriteBarrier
        <kTransactionActive, kCheckTransaction, kVerifyFlags, kIsVolatile>
        (field_offset, new_value);
    stub->PopulateFrom(stub->GetObjectAddress());
    stub->UnlockTableEntry();
    return;
  }
  // marvin end
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    ObjPtr<Object> obj;
    if (kIsVolatile) {
      obj = GetFieldObjectVolatile<Object>(field_offset);
    } else {
      obj = GetFieldObject<Object>(field_offset);
    }
    Runtime::Current()->RecordWriteFieldReference(this, field_offset, obj, true);
  }
  Verify<kVerifyFlags>();
  VerifyWrite<kVerifyFlags>(new_value);
  uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
  HeapReference<Object>* objref_addr = reinterpret_cast<HeapReference<Object>*>(raw_addr);
  objref_addr->Assign<kIsVolatile>(new_value.Ptr());
  // marvin start
  SetWriteBit();
  SetDirtyBit();
  // marvin end
}

template<bool kTransactionActive,
         bool kCheckTransaction,
         VerifyObjectFlags kVerifyFlags,
         bool kIsVolatile>
inline void Object::SetFieldObject(MemberOffset field_offset, ObjPtr<Object> new_value) {
  SetFieldObjectWithoutWriteBarrier<kTransactionActive, kCheckTransaction, kVerifyFlags,
      kIsVolatile>(field_offset, new_value);
  if (new_value != nullptr) {
    WriteBarrier::ForFieldWrite<WriteBarrier::kWithoutNullCheck>(this, field_offset, new_value);
    // TODO: Check field assignment could theoretically cause thread suspension, TODO: fix this.
    CheckFieldAssignment(field_offset, new_value);
  }
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::SetFieldObjectVolatile(MemberOffset field_offset, ObjPtr<Object> new_value) {
  SetFieldObject<kTransactionActive, kCheckTransaction, kVerifyFlags, true>(field_offset,
                                                                            new_value);
}

template<bool kCheckTransaction, VerifyObjectFlags kVerifyFlags, bool kIsVolatile>
inline void Object::SetFieldObjectTransaction(MemberOffset field_offset, ObjPtr<Object> new_value) {
  if (Runtime::Current()->IsActiveTransaction()) {
    SetFieldObject<true, kCheckTransaction, kVerifyFlags, kIsVolatile>(field_offset, new_value);
  } else {
    SetFieldObject<false, kCheckTransaction, kVerifyFlags, kIsVolatile>(field_offset, new_value);
  }
}

template <VerifyObjectFlags kVerifyFlags>
inline HeapReference<Object>* Object::GetFieldObjectReferenceAddr(MemberOffset field_offset) {
  // marvin start
  if (field_offset.Uint32Value() >= sizeof(Object)) {
    SWAP_PREAMBLE_TEMPLATE(GetFieldObjectReferenceAddr, Object, HeapReference<Object>*, GATHER_TEMPLATE_ARGS(kVerifyFlags), field_offset)
  }
  // marvin end
  Verify<kVerifyFlags>();
  return reinterpret_cast<HeapReference<Object>*>(reinterpret_cast<uint8_t*>(this) +
      field_offset.Int32Value());
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline bool Object::CasFieldObjectWithoutWriteBarrier(MemberOffset field_offset,
                                                      ObjPtr<Object> old_value,
                                                      ObjPtr<Object> new_value,
                                                      CASMode mode,
                                                      std::memory_order memory_order) {
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  VerifyCAS<kVerifyFlags>(new_value, old_value);
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteFieldReference(this, field_offset, old_value, true);
  }
  uint32_t old_ref(PtrCompression<kPoisonHeapReferences, Object>::Compress(old_value));
  uint32_t new_ref(PtrCompression<kPoisonHeapReferences, Object>::Compress(new_value));
  uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
  Atomic<uint32_t>* atomic_addr = reinterpret_cast<Atomic<uint32_t>*>(raw_addr);
  return atomic_addr->CompareAndSet(old_ref, new_ref, mode, memory_order);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline bool Object::CasFieldObject(MemberOffset field_offset,
                                   ObjPtr<Object> old_value,
                                   ObjPtr<Object> new_value,
                                   CASMode mode,
                                   std::memory_order memory_order) {
  bool success = CasFieldObjectWithoutWriteBarrier<
      kTransactionActive, kCheckTransaction, kVerifyFlags>(field_offset,
                                                           old_value,
                                                           new_value,
                                                           mode,
                                                           memory_order);
  if (success) {
    WriteBarrier::ForFieldWrite(this, field_offset, new_value);
  }
  return success;
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline ObjPtr<Object> Object::CompareAndExchangeFieldObject(MemberOffset field_offset,
                                                            ObjPtr<Object> old_value,
                                                            ObjPtr<Object> new_value) {
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  VerifyCAS<kVerifyFlags>(new_value, old_value);
  uint32_t old_ref(PtrCompression<kPoisonHeapReferences, Object>::Compress(old_value));
  uint32_t new_ref(PtrCompression<kPoisonHeapReferences, Object>::Compress(new_value));
  uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
  Atomic<uint32_t>* atomic_addr = reinterpret_cast<Atomic<uint32_t>*>(raw_addr);
  bool success = atomic_addr->compare_exchange_strong(old_ref, new_ref, std::memory_order_seq_cst);
  ObjPtr<Object> witness_value(PtrCompression<kPoisonHeapReferences, Object>::Decompress(old_ref));
  if (kIsDebugBuild) {
    // Ensure caller has done read barrier on the reference field so it's in the to-space.
    ReadBarrier::AssertToSpaceInvariant(witness_value.Ptr());
  }
  if (success) {
    if (kTransactionActive) {
      Runtime::Current()->RecordWriteFieldReference(this, field_offset, witness_value, true);
    }
    WriteBarrier::ForFieldWrite(this, field_offset, new_value);
  }
  VerifyRead<kVerifyFlags>(witness_value);
  return witness_value;
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline ObjPtr<Object> Object::ExchangeFieldObject(MemberOffset field_offset,
                                                  ObjPtr<Object> new_value) {
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  VerifyCAS<kVerifyFlags>(new_value, /*old_value=*/ nullptr);

  uint32_t new_ref(PtrCompression<kPoisonHeapReferences, Object>::Compress(new_value));
  uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
  Atomic<uint32_t>* atomic_addr = reinterpret_cast<Atomic<uint32_t>*>(raw_addr);
  uint32_t old_ref = atomic_addr->exchange(new_ref, std::memory_order_seq_cst);
  ObjPtr<Object> old_value(PtrCompression<kPoisonHeapReferences, Object>::Decompress(old_ref));
  if (kIsDebugBuild) {
    // Ensure caller has done read barrier on the reference field so it's in the to-space.
    ReadBarrier::AssertToSpaceInvariant(old_value.Ptr());
  }
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteFieldReference(this, field_offset, old_value, true);
  }
  WriteBarrier::ForFieldWrite(this, field_offset, new_value);
  VerifyRead<kVerifyFlags>(old_value);
  return old_value;
}

template<typename T, VerifyObjectFlags kVerifyFlags>
inline void Object::GetPrimitiveFieldViaAccessor(MemberOffset field_offset, Accessor<T>* accessor) {
  Verify<kVerifyFlags>();
  uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
  T* addr = reinterpret_cast<T*>(raw_addr);
  accessor->Access(addr);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::UpdateFieldBooleanViaAccessor(MemberOffset field_offset,
                                                  Accessor<uint8_t>* accessor) {
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    static const bool kIsVolatile = true;
    uint8_t old_value = GetFieldBoolean<kVerifyFlags, kIsVolatile>(field_offset);
    Runtime::Current()->RecordWriteFieldBoolean(this, field_offset, old_value, kIsVolatile);
  }
  Verify<kVerifyFlags>();
  uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
  uint8_t* addr = raw_addr;
  accessor->Access(addr);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::UpdateFieldByteViaAccessor(MemberOffset field_offset,
                                               Accessor<int8_t>* accessor) {
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    static const bool kIsVolatile = true;
    int8_t old_value = GetFieldByte<kVerifyFlags, kIsVolatile>(field_offset);
    Runtime::Current()->RecordWriteFieldByte(this, field_offset, old_value, kIsVolatile);
  }
  Verify<kVerifyFlags>();
  uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
  int8_t* addr = reinterpret_cast<int8_t*>(raw_addr);
  accessor->Access(addr);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::UpdateFieldCharViaAccessor(MemberOffset field_offset,
                                               Accessor<uint16_t>* accessor) {
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    static const bool kIsVolatile = true;
    uint16_t old_value = GetFieldChar<kVerifyFlags, kIsVolatile>(field_offset);
    Runtime::Current()->RecordWriteFieldChar(this, field_offset, old_value, kIsVolatile);
  }
  Verify<kVerifyFlags>();
  uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
  uint16_t* addr = reinterpret_cast<uint16_t*>(raw_addr);
  accessor->Access(addr);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::UpdateFieldShortViaAccessor(MemberOffset field_offset,
                                                Accessor<int16_t>* accessor) {
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    static const bool kIsVolatile = true;
    int16_t old_value = GetFieldShort<kVerifyFlags, kIsVolatile>(field_offset);
    Runtime::Current()->RecordWriteFieldShort(this, field_offset, old_value, kIsVolatile);
  }
  Verify<kVerifyFlags>();
  uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
  int16_t* addr = reinterpret_cast<int16_t*>(raw_addr);
  accessor->Access(addr);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::UpdateField32ViaAccessor(MemberOffset field_offset,
                                             Accessor<int32_t>* accessor) {
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    static const bool kIsVolatile = true;
    int32_t old_value = GetField32<kVerifyFlags, kIsVolatile>(field_offset);
    Runtime::Current()->RecordWriteField32(this, field_offset, old_value, kIsVolatile);
  }
  Verify<kVerifyFlags>();
  uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
  int32_t* addr = reinterpret_cast<int32_t*>(raw_addr);
  accessor->Access(addr);
}

template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void Object::UpdateField64ViaAccessor(MemberOffset field_offset,
                                             Accessor<int64_t>* accessor) {
  VerifyTransaction<kTransactionActive, kCheckTransaction>();
  if (kTransactionActive) {
    static const bool kIsVolatile = true;
    int64_t old_value = GetField64<kVerifyFlags, kIsVolatile>(field_offset);
    Runtime::Current()->RecordWriteField64(this, field_offset, old_value, kIsVolatile);
  }
  Verify<kVerifyFlags>();
  uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
  int64_t* addr = reinterpret_cast<int64_t*>(raw_addr);
  accessor->Access(addr);
}

template<bool kIsStatic,
         VerifyObjectFlags kVerifyFlags,
         ReadBarrierOption kReadBarrierOption,
         typename Visitor>
inline void Object::VisitFieldsReferences(uint32_t ref_offsets, const Visitor& visitor) {
  // marvin start
  SWAP_PREAMBLE_TEMPLATE_VOID(VisitFieldsReferences, Object, GATHER_TEMPLATE_ARGS(kIsStatic, kVerifyFlags, kReadBarrierOption, Visitor), ref_offsets, visitor)
  // marvin end
  if (!kIsStatic && (ref_offsets != mirror::Class::kClassWalkSuper)) {
    // Instance fields and not the slow-path.
    uint32_t field_offset = mirror::kObjectHeaderSize;
    while (ref_offsets != 0) {
      if ((ref_offsets & 1) != 0) {
        visitor(this, MemberOffset(field_offset), kIsStatic);
      }
      ref_offsets >>= 1;
      field_offset += sizeof(mirror::HeapReference<mirror::Object>);
    }
  } else {
    // There is no reference offset bitmap. In the non-static case, walk up the class
    // inheritance hierarchy and find reference offsets the hard way. In the static case, just
    // consider this class.
    for (ObjPtr<Class> klass = kIsStatic
            ? AsClass<kVerifyFlags>()
            : GetClass<kVerifyFlags, kReadBarrierOption>();
        klass != nullptr;
        klass = kIsStatic ? nullptr : klass->GetSuperClass<kVerifyFlags, kReadBarrierOption>()) {
      const size_t num_reference_fields =
          kIsStatic ? klass->NumReferenceStaticFields() : klass->NumReferenceInstanceFields();
      if (num_reference_fields == 0u) {
        continue;
      }
      // Presumably GC can happen when we are cross compiling, it should not cause performance
      // problems to do pointer size logic.
      MemberOffset field_offset = kIsStatic
          ? klass->GetFirstReferenceStaticFieldOffset<kVerifyFlags>(
              Runtime::Current()->GetClassLinker()->GetImagePointerSize())
          : klass->GetFirstReferenceInstanceFieldOffset<kVerifyFlags, kReadBarrierOption>();
      for (size_t i = 0u; i < num_reference_fields; ++i) {
        // TODO: Do a simpler check?
        if (field_offset.Uint32Value() != ClassOffset().Uint32Value()) {
          visitor(this, field_offset, kIsStatic);
        }
        field_offset = MemberOffset(field_offset.Uint32Value() +
                                    sizeof(mirror::HeapReference<mirror::Object>));
      }
    }
  }
}

template<VerifyObjectFlags kVerifyFlags, ReadBarrierOption kReadBarrierOption, typename Visitor>
inline void Object::VisitInstanceFieldsReferences(ObjPtr<Class> klass, const Visitor& visitor) {
  VisitFieldsReferences<false, kVerifyFlags, kReadBarrierOption>(
      klass->GetReferenceInstanceOffsets<kVerifyFlags>(), visitor);
}

template<VerifyObjectFlags kVerifyFlags, ReadBarrierOption kReadBarrierOption, typename Visitor>
inline void Object::VisitStaticFieldsReferences(ObjPtr<Class> klass, const Visitor& visitor) {
  // marvin start
  SWAP_PREAMBLE_TEMPLATE_VOID(VisitStaticFieldsReferences, Object, GATHER_TEMPLATE_ARGS(kVerifyFlags, kReadBarrierOption, Visitor), klass, visitor)
  // marvin end
  DCHECK(!klass->IsTemp<kVerifyFlags>());
  klass->VisitFieldsReferences<true, kVerifyFlags, kReadBarrierOption>(0, visitor);
}

template<VerifyObjectFlags kVerifyFlags, ReadBarrierOption kReadBarrierOption>
inline bool Object::IsClassLoader() {
  return GetClass<kVerifyFlags, kReadBarrierOption>()->template IsClassLoaderClass<kVerifyFlags>();
}

template<VerifyObjectFlags kVerifyFlags, ReadBarrierOption kReadBarrierOption>
inline ObjPtr<ClassLoader> Object::AsClassLoader() {
  // marvin start
  // SWAP_PREAMBLE_TEMPLATE(AsClassLoader, Object, ObjPtr<ClassLoader>, GATHER_TEMPLATE_ARGS(kVerifyFlags, kReadBarrierOption), )
  // marvin end
  DCHECK((IsClassLoader<kVerifyFlags, kReadBarrierOption>()));
  return ObjPtr<ClassLoader>::DownCast(this);
}

template<VerifyObjectFlags kVerifyFlags, ReadBarrierOption kReadBarrierOption>
inline bool Object::IsDexCache() {
  return GetClass<kVerifyFlags, kReadBarrierOption>()->template IsDexCacheClass<kVerifyFlags>();
}

template<VerifyObjectFlags kVerifyFlags, ReadBarrierOption kReadBarrierOption>
inline ObjPtr<mirror::DexCache> Object::AsDexCache() {
  DCHECK((IsDexCache<kVerifyFlags, kReadBarrierOption>()));
  return ObjPtr<DexCache>::DownCast(this);
}

template<bool kTransactionActive, bool kCheckTransaction>
inline void Object::VerifyTransaction() {
  if (kCheckTransaction) {
    DCHECK_EQ(kTransactionActive, Runtime::Current()->IsActiveTransaction());
  }
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_OBJECT_INL_H_
