// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/objects/lookup.h"

#include "src/deoptimizer/deoptimizer.h"
#include "src/execution/isolate-inl.h"
#include "src/init/bootstrapper.h"
#include "src/logging/counters.h"
#include "src/objects/elements.h"
#include "src/objects/field-type.h"
#include "src/objects/hash-table-inl.h"
#include "src/objects/heap-number-inl.h"
#include "src/objects/struct-inl.h"

namespace v8 {
namespace internal {

// static
LookupIterator LookupIterator::PropertyOrElement(
    Isolate* isolate, Handle<Object> receiver, Handle<Object> key,
    bool* success, Handle<JSReceiver> holder, Configuration configuration) {
  uint32_t index = 0;
  if (key->ToArrayIndex(&index)) {
    *success = true;
    return LookupIterator(isolate, receiver, index, holder, configuration);
  }

  Handle<Name> name;
  *success = Object::ToName(isolate, key).ToHandle(&name);
  if (!*success) {
    DCHECK(isolate->has_pending_exception());
    // Return an unusable dummy.
    return LookupIterator(isolate, receiver,
                          isolate->factory()->empty_string());
  }

  if (name->AsArrayIndex(&index)) {
    LookupIterator it(isolate, receiver, index, holder, configuration);
    // Here we try to avoid having to rebuild the string later
    // by storing it on the indexed LookupIterator.
    it.name_ = name;
    return it;
  }

  return LookupIterator(receiver, name, holder, configuration);
}

// static
LookupIterator LookupIterator::PropertyOrElement(Isolate* isolate,
                                                 Handle<Object> receiver,
                                                 Handle<Object> key,
                                                 bool* success,
                                                 Configuration configuration) {
  // TODO(mslekova): come up with better way to avoid duplication
  uint32_t index = 0;
  if (key->ToArrayIndex(&index)) {
    *success = true;
    return LookupIterator(isolate, receiver, index, configuration);
  }

  Handle<Name> name;
  *success = Object::ToName(isolate, key).ToHandle(&name);
  if (!*success) {
    DCHECK(isolate->has_pending_exception());
    // Return an unusable dummy.
    return LookupIterator(isolate, receiver,
                          isolate->factory()->empty_string());
  }

  if (name->AsArrayIndex(&index)) {
    LookupIterator it(isolate, receiver, index, configuration);
    // Here we try to avoid having to rebuild the string later
    // by storing it on the indexed LookupIterator.
    it.name_ = name;
    return it;
  }

  return LookupIterator(isolate, receiver, name, configuration);
}

LookupIterator::LookupIterator(Isolate* isolate, Handle<Object> receiver,
                               Handle<Name> name, Handle<Map> transition_map,
                               PropertyDetails details, bool has_property)
    : configuration_(DEFAULT),
      state_(TRANSITION),
      has_property_(has_property),
      interceptor_state_(InterceptorState::kUninitialized),
      property_details_(details),
      isolate_(isolate),
      name_(name),
      transition_(transition_map),
      receiver_(receiver),
      initial_holder_(GetRoot(isolate, receiver)),
      index_(kMaxUInt32),
      number_(static_cast<uint32_t>(DescriptorArray::kNotFound)) {
  holder_ = initial_holder_;
}

template <bool is_element>
void LookupIterator::Start() {
  DisallowHeapAllocation no_gc;

  has_property_ = false;
  state_ = NOT_FOUND;
  holder_ = initial_holder_;

  JSReceiver holder = *holder_;
  Map map = holder.map(isolate_);

  state_ = LookupInHolder<is_element>(map, holder);
  if (IsFound()) return;

  NextInternal<is_element>(map, holder);
}

template void LookupIterator::Start<true>();
template void LookupIterator::Start<false>();

void LookupIterator::Next() {
  DCHECK_NE(JSPROXY, state_);
  DCHECK_NE(TRANSITION, state_);
  DisallowHeapAllocation no_gc;
  has_property_ = false;

  JSReceiver holder = *holder_;
  Map map = holder.map(isolate_);

  if (map.IsSpecialReceiverMap()) {
    state_ = IsElement() ? LookupInSpecialHolder<true>(map, holder)
                         : LookupInSpecialHolder<false>(map, holder);
    if (IsFound()) return;
  }

  IsElement() ? NextInternal<true>(map, holder)
              : NextInternal<false>(map, holder);
}

template <bool is_element>
void LookupIterator::NextInternal(Map map, JSReceiver holder) {
  do {
    JSReceiver maybe_holder = NextHolder(map);
    if (maybe_holder.is_null()) {
      if (interceptor_state_ == InterceptorState::kSkipNonMasking) {
        RestartLookupForNonMaskingInterceptors<is_element>();
        return;
      }
      state_ = NOT_FOUND;
      if (holder != *holder_) holder_ = handle(holder, isolate_);
      return;
    }
    holder = maybe_holder;
    map = holder.map(isolate_);
    state_ = LookupInHolder<is_element>(map, holder);
  } while (!IsFound());

  holder_ = handle(holder, isolate_);
}

template <bool is_element>
void LookupIterator::RestartInternal(InterceptorState interceptor_state) {
  interceptor_state_ = interceptor_state;
  property_details_ = PropertyDetails::Empty();
  number_ = static_cast<uint32_t>(DescriptorArray::kNotFound);
  Start<is_element>();
}

template void LookupIterator::RestartInternal<true>(InterceptorState);
template void LookupIterator::RestartInternal<false>(InterceptorState);

// static
Handle<JSReceiver> LookupIterator::GetRootForNonJSReceiver(
    Isolate* isolate, Handle<Object> receiver, uint32_t index) {
  // Strings are the only objects with properties (only elements) directly on
  // the wrapper. Hence we can skip generating the wrapper for all other cases.
  if (receiver->IsString(isolate) &&
      index < static_cast<uint32_t>(String::cast(*receiver).length())) {
    // TODO(verwaest): Speed this up. Perhaps use a cached wrapper on the native
    // context, ensuring that we don't leak it into JS?
    Handle<JSFunction> constructor = isolate->string_function();
    Handle<JSObject> result = isolate->factory()->NewJSObject(constructor);
    Handle<JSPrimitiveWrapper>::cast(result)->set_value(*receiver);
    return result;
  }
  auto root = handle(
      receiver->GetPrototypeChainRootMap(isolate).prototype(isolate), isolate);
  if (root->IsNull(isolate)) {
    isolate->PushStackTraceAndDie(reinterpret_cast<void*>(receiver->ptr()));
  }
  return Handle<JSReceiver>::cast(root);
}

Handle<Map> LookupIterator::GetReceiverMap() const {
  if (receiver_->IsNumber(isolate_)) return factory()->heap_number_map();
  return handle(Handle<HeapObject>::cast(receiver_)->map(isolate_), isolate_);
}

bool LookupIterator::HasAccess() const {
  DCHECK_EQ(ACCESS_CHECK, state_);
  return isolate_->MayAccess(handle(isolate_->context(), isolate_),
                             GetHolder<JSObject>());
}

template <bool is_element>
void LookupIterator::ReloadPropertyInformation() {
  state_ = BEFORE_PROPERTY;
  interceptor_state_ = InterceptorState::kUninitialized;
  state_ = LookupInHolder<is_element>(holder_->map(isolate_), *holder_);
  DCHECK(IsFound() || !holder_->HasFastProperties(isolate_));
}

namespace {

bool IsTypedArrayFunctionInAnyContext(Isolate* isolate, JSReceiver holder) {
  static uint32_t context_slots[] = {
#define TYPED_ARRAY_CONTEXT_SLOTS(Type, type, TYPE, ctype) \
  Context::TYPE##_ARRAY_FUN_INDEX,

      TYPED_ARRAYS(TYPED_ARRAY_CONTEXT_SLOTS)
#undef TYPED_ARRAY_CONTEXT_SLOTS
  };

  if (!holder.IsJSFunction(isolate)) return false;

  return std::any_of(
      std::begin(context_slots), std::end(context_slots),
      [=](uint32_t slot) { return isolate->IsInAnyContext(holder, slot); });
}

}  // namespace

void LookupIterator::InternalUpdateProtector() {
  if (isolate_->bootstrapper()->IsActive()) return;

  Handle<NativeContext> native_context = isolate_->native_context();

  ReadOnlyRoots roots(isolate_);
  if (*name_ == roots.constructor_string()) {
    if (!isolate_->IsArraySpeciesLookupChainIntact() &&
        !isolate_->IsPromiseSpeciesLookupChainIntact() &&
        !isolate_->IsRegExpSpeciesLookupChainIntact(native_context) &&
        !isolate_->IsTypedArraySpeciesLookupChainIntact()) {
      return;
    }
    // Setting the constructor property could change an instance's @@species
    if (holder_->IsJSArray(isolate_)) {
      if (!isolate_->IsArraySpeciesLookupChainIntact()) return;
      isolate_->CountUsage(
          v8::Isolate::UseCounterFeature::kArrayInstanceConstructorModified);
      isolate_->InvalidateArraySpeciesProtector();
      return;
    } else if (holder_->IsJSPromise(isolate_)) {
      if (!isolate_->IsPromiseSpeciesLookupChainIntact()) return;
      isolate_->InvalidatePromiseSpeciesProtector();
      return;
    } else if (holder_->IsJSRegExp(isolate_)) {
      if (!isolate_->IsRegExpSpeciesLookupChainIntact(native_context)) return;
      isolate_->InvalidateRegExpSpeciesProtector(native_context);
      return;
    } else if (holder_->IsJSTypedArray(isolate_)) {
      if (!isolate_->IsTypedArraySpeciesLookupChainIntact()) return;
      isolate_->InvalidateTypedArraySpeciesProtector();
      return;
    }
    if (holder_->map(isolate_).is_prototype_map()) {
      DisallowHeapAllocation no_gc;
      // Setting the constructor of any prototype with the @@species protector
      // (of any realm) also needs to invalidate the protector.
      // For typed arrays, we check a prototype of this holder since TypedArrays
      // have different prototypes for each type, and their parent prototype is
      // pointing the same TYPED_ARRAY_PROTOTYPE.
      if (isolate_->IsInAnyContext(*holder_,
                                   Context::INITIAL_ARRAY_PROTOTYPE_INDEX)) {
        if (!isolate_->IsArraySpeciesLookupChainIntact()) return;
        isolate_->CountUsage(
            v8::Isolate::UseCounterFeature::kArrayPrototypeConstructorModified);
        isolate_->InvalidateArraySpeciesProtector();
      } else if (isolate_->IsInAnyContext(*holder_,
                                          Context::PROMISE_PROTOTYPE_INDEX)) {
        if (!isolate_->IsPromiseSpeciesLookupChainIntact()) return;
        isolate_->InvalidatePromiseSpeciesProtector();
      } else if (isolate_->IsInAnyContext(*holder_,
                                          Context::REGEXP_PROTOTYPE_INDEX)) {
        if (!isolate_->IsRegExpSpeciesLookupChainIntact(native_context)) return;
        isolate_->InvalidateRegExpSpeciesProtector(native_context);
      } else if (isolate_->IsInAnyContext(
                     holder_->map(isolate_).prototype(isolate_),
                     Context::TYPED_ARRAY_PROTOTYPE_INDEX)) {
        if (!isolate_->IsTypedArraySpeciesLookupChainIntact()) return;
        isolate_->InvalidateTypedArraySpeciesProtector();
      }
    }
  } else if (*name_ == roots.next_string()) {
    if (isolate_->IsInAnyContext(
            *holder_, Context::INITIAL_ARRAY_ITERATOR_PROTOTYPE_INDEX)) {
      // Setting the next property of %ArrayIteratorPrototype% also needs to
      // invalidate the array iterator protector.
      if (!isolate_->IsArrayIteratorLookupChainIntact()) return;
      isolate_->InvalidateArrayIteratorProtector();
    } else if (isolate_->IsInAnyContext(
                   *holder_, Context::INITIAL_MAP_ITERATOR_PROTOTYPE_INDEX)) {
      if (!isolate_->IsMapIteratorLookupChainIntact()) return;
      isolate_->InvalidateMapIteratorProtector();
    } else if (isolate_->IsInAnyContext(
                   *holder_, Context::INITIAL_SET_ITERATOR_PROTOTYPE_INDEX)) {
      if (!isolate_->IsSetIteratorLookupChainIntact()) return;
      isolate_->InvalidateSetIteratorProtector();
    } else if (isolate_->IsInAnyContext(
                   *receiver_,
                   Context::INITIAL_STRING_ITERATOR_PROTOTYPE_INDEX)) {
      // Setting the next property of %StringIteratorPrototype% invalidates the
      // string iterator protector.
      if (!isolate_->IsStringIteratorLookupChainIntact()) return;
      isolate_->InvalidateStringIteratorProtector();
    }
  } else if (*name_ == roots.species_symbol()) {
    if (!isolate_->IsArraySpeciesLookupChainIntact() &&
        !isolate_->IsPromiseSpeciesLookupChainIntact() &&
        !isolate_->IsRegExpSpeciesLookupChainIntact(native_context) &&
        !isolate_->IsTypedArraySpeciesLookupChainIntact()) {
      return;
    }
    // Setting the Symbol.species property of any Array, Promise or TypedArray
    // constructor invalidates the @@species protector
    if (isolate_->IsInAnyContext(*holder_, Context::ARRAY_FUNCTION_INDEX)) {
      if (!isolate_->IsArraySpeciesLookupChainIntact()) return;
      isolate_->CountUsage(
          v8::Isolate::UseCounterFeature::kArraySpeciesModified);
      isolate_->InvalidateArraySpeciesProtector();
    } else if (isolate_->IsInAnyContext(*holder_,
                                        Context::PROMISE_FUNCTION_INDEX)) {
      if (!isolate_->IsPromiseSpeciesLookupChainIntact()) return;
      isolate_->InvalidatePromiseSpeciesProtector();
    } else if (isolate_->IsInAnyContext(*holder_,
                                        Context::REGEXP_FUNCTION_INDEX)) {
      if (!isolate_->IsRegExpSpeciesLookupChainIntact(native_context)) return;
      isolate_->InvalidateRegExpSpeciesProtector(native_context);
    } else if (IsTypedArrayFunctionInAnyContext(isolate_, *holder_)) {
      if (!isolate_->IsTypedArraySpeciesLookupChainIntact()) return;
      isolate_->InvalidateTypedArraySpeciesProtector();
    }
  } else if (*name_ == roots.is_concat_spreadable_symbol()) {
    if (!isolate_->IsIsConcatSpreadableLookupChainIntact()) return;
    isolate_->InvalidateIsConcatSpreadableProtector();
  } else if (*name_ == roots.iterator_symbol()) {
    if (holder_->IsJSArray(isolate_)) {
      if (!isolate_->IsArrayIteratorLookupChainIntact()) return;
      isolate_->InvalidateArrayIteratorProtector();
    } else if (isolate_->IsInAnyContext(
                   *holder_, Context::INITIAL_ITERATOR_PROTOTYPE_INDEX)) {
      if (isolate_->IsMapIteratorLookupChainIntact()) {
        isolate_->InvalidateMapIteratorProtector();
      }
      if (isolate_->IsSetIteratorLookupChainIntact()) {
        isolate_->InvalidateSetIteratorProtector();
      }
    } else if (isolate_->IsInAnyContext(*holder_,
                                        Context::INITIAL_SET_PROTOTYPE_INDEX)) {
      if (!isolate_->IsSetIteratorLookupChainIntact()) return;
      isolate_->InvalidateSetIteratorProtector();
    } else if (isolate_->IsInAnyContext(
                   *receiver_, Context::INITIAL_STRING_PROTOTYPE_INDEX)) {
      // Setting the Symbol.iterator property of String.prototype invalidates
      // the string iterator protector. Symbol.iterator can also be set on a
      // String wrapper, but not on a primitive string. We only support
      // protector for primitive strings.
      if (!isolate_->IsStringIteratorLookupChainIntact()) return;
      isolate_->InvalidateStringIteratorProtector();
    }
  } else if (*name_ == roots.resolve_string()) {
    if (!isolate_->IsPromiseResolveLookupChainIntact()) return;
    // Setting the "resolve" property on any %Promise% intrinsic object
    // invalidates the Promise.resolve protector.
    if (isolate_->IsInAnyContext(*holder_, Context::PROMISE_FUNCTION_INDEX)) {
      isolate_->InvalidatePromiseResolveProtector();
    }
  } else if (*name_ == roots.then_string()) {
    if (!isolate_->IsPromiseThenLookupChainIntact()) return;
    // Setting the "then" property on any JSPromise instance or on the
    // initial %PromisePrototype% invalidates the Promise#then protector.
    // Also setting the "then" property on the initial %ObjectPrototype%
    // invalidates the Promise#then protector, since we use this protector
    // to guard the fast-path in AsyncGeneratorResolve, where we can skip
    // the ResolvePromise step and go directly to FulfillPromise if we
    // know that the Object.prototype doesn't contain a "then" method.
    if (holder_->IsJSPromise(isolate_) ||
        isolate_->IsInAnyContext(*holder_,
                                 Context::INITIAL_OBJECT_PROTOTYPE_INDEX) ||
        isolate_->IsInAnyContext(*holder_, Context::PROMISE_PROTOTYPE_INDEX)) {
      isolate_->InvalidatePromiseThenProtector();
    }
  }
}

void LookupIterator::PrepareForDataProperty(Handle<Object> value) {
  DCHECK(state_ == DATA || state_ == ACCESSOR);
  DCHECK(HolderIsReceiverOrHiddenPrototype());

  Handle<JSReceiver> holder = GetHolder<JSReceiver>();
  // JSProxy does not have fast properties so we do an early return.
  DCHECK_IMPLIES(holder->IsJSProxy(isolate_),
                 !holder->HasFastProperties(isolate_));
  DCHECK_IMPLIES(holder->IsJSProxy(isolate_), name()->IsPrivate(isolate_));
  if (holder->IsJSProxy(isolate_)) return;

  Handle<JSObject> holder_obj = Handle<JSObject>::cast(holder);

  if (IsElement()) {
    ElementsKind kind = holder_obj->GetElementsKind(isolate_);
    ElementsKind to = value->OptimalElementsKind(isolate_);
    if (IsHoleyElementsKind(kind)) to = GetHoleyElementsKind(to);
    to = GetMoreGeneralElementsKind(kind, to);

    if (kind != to) {
      JSObject::TransitionElementsKind(holder_obj, to);
    }

    // Copy the backing store if it is copy-on-write.
    if (IsSmiOrObjectElementsKind(to) || IsSealedElementsKind(to)) {
      JSObject::EnsureWritableFastElements(holder_obj);
    }
    return;
  }

  if (holder_obj->IsJSGlobalObject(isolate_)) {
    Handle<GlobalDictionary> dictionary(
        JSGlobalObject::cast(*holder_obj).global_dictionary(isolate_),
        isolate());
    Handle<PropertyCell> cell(dictionary->CellAt(isolate_, dictionary_entry()),
                              isolate());
    property_details_ = cell->property_details();
    PropertyCell::PrepareForValue(isolate(), dictionary, dictionary_entry(),
                                  value, property_details_);
    return;
  }
  if (!holder_obj->HasFastProperties(isolate_)) return;

  PropertyConstness new_constness = PropertyConstness::kConst;
  if (constness() == PropertyConstness::kConst) {
    DCHECK_EQ(kData, property_details_.kind());
    // Check that current value matches new value otherwise we should make
    // the property mutable.
    if (!IsConstFieldValueEqualTo(*value))
      new_constness = PropertyConstness::kMutable;
  }

  Handle<Map> old_map(holder_obj->map(isolate_), isolate_);
  DCHECK(!old_map->is_dictionary_map());

  Handle<Map> new_map = Map::Update(isolate_, old_map);
  if (!new_map->is_dictionary_map()) {
    new_map = Map::PrepareForDataProperty(
        isolate(), new_map, descriptor_number(), new_constness, value);

    if (old_map.is_identical_to(new_map)) {
      // Update the property details if the representation was None.
      if (constness() != new_constness || representation().IsNone()) {
        property_details_ = new_map->instance_descriptors(isolate_).GetDetails(
            descriptor_number());
      }
      return;
    }
  }
  // We should only get here if the new_map is different from the old map,
  // otherwise we would have falled through to the is_identical_to check above.
  DCHECK_NE(*old_map, *new_map);

  JSObject::MigrateToMap(isolate_, holder_obj, new_map);
  ReloadPropertyInformation<false>();
}

void LookupIterator::ReconfigureDataProperty(Handle<Object> value,
                                             PropertyAttributes attributes) {
  DCHECK(state_ == DATA || state_ == ACCESSOR);
  DCHECK(HolderIsReceiverOrHiddenPrototype());

  Handle<JSReceiver> holder = GetHolder<JSReceiver>();

  // Property details can never change for private properties.
  if (holder->IsJSProxy(isolate_)) {
    DCHECK(name()->IsPrivate(isolate_));
    return;
  }

  Handle<JSObject> holder_obj = Handle<JSObject>::cast(holder);
  if (IsElement()) {
    DCHECK(!holder_obj->HasTypedArrayElements(isolate_));
    DCHECK(attributes != NONE || !holder_obj->HasFastElements(isolate_));
    Handle<FixedArrayBase> elements(holder_obj->elements(isolate_), isolate());
    holder_obj->GetElementsAccessor(isolate_)->Reconfigure(
        holder_obj, elements, number_, value, attributes);
    ReloadPropertyInformation<true>();
  } else if (holder_obj->HasFastProperties(isolate_)) {
    Handle<Map> old_map(holder_obj->map(isolate_), isolate_);
    // Force mutable to avoid changing constant value by reconfiguring
    // kData -> kAccessor -> kData.
    Handle<Map> new_map = Map::ReconfigureExistingProperty(
        isolate_, old_map, descriptor_number(), i::kData, attributes,
        PropertyConstness::kMutable);
    if (!new_map->is_dictionary_map()) {
      // Make sure that the data property has a compatible representation.
      // TODO(leszeks): Do this as part of ReconfigureExistingProperty.
      new_map =
          Map::PrepareForDataProperty(isolate(), new_map, descriptor_number(),
                                      PropertyConstness::kMutable, value);
    }
    JSObject::MigrateToMap(isolate_, holder_obj, new_map);
    ReloadPropertyInformation<false>();
  }

  if (!IsElement() && !holder_obj->HasFastProperties(isolate_)) {
    PropertyDetails details(kData, attributes, PropertyCellType::kMutable);
    if (holder_obj->map(isolate_).is_prototype_map() &&
        (property_details_.attributes() & READ_ONLY) == 0 &&
        (attributes & READ_ONLY) != 0) {
      // Invalidate prototype validity cell when a property is reconfigured
      // from writable to read-only as this may invalidate transitioning store
      // IC handlers.
      JSObject::InvalidatePrototypeChains(holder->map(isolate_));
    }
    if (holder_obj->IsJSGlobalObject(isolate_)) {
      Handle<GlobalDictionary> dictionary(
          JSGlobalObject::cast(*holder_obj).global_dictionary(isolate_),
          isolate());

      Handle<PropertyCell> cell = PropertyCell::PrepareForValue(
          isolate(), dictionary, dictionary_entry(), value, details);
      cell->set_value(*value);
      property_details_ = cell->property_details();
    } else {
      Handle<NameDictionary> dictionary(
          holder_obj->property_dictionary(isolate_), isolate());
      PropertyDetails original_details =
          dictionary->DetailsAt(dictionary_entry());
      int enumeration_index = original_details.dictionary_index();
      DCHECK_GT(enumeration_index, 0);
      details = details.set_index(enumeration_index);
      dictionary->SetEntry(isolate(), dictionary_entry(), *name(), *value,
                           details);
      property_details_ = details;
    }
    state_ = DATA;
  }

  WriteDataValue(value, true);

#if VERIFY_HEAP
  if (FLAG_verify_heap) {
    holder->HeapObjectVerify(isolate());
  }
#endif
}

// Can only be called when the receiver is a JSObject. JSProxy has to be handled
// via a trap. Adding properties to primitive values is not observable.
void LookupIterator::PrepareTransitionToDataProperty(
    Handle<JSReceiver> receiver, Handle<Object> value,
    PropertyAttributes attributes, StoreOrigin store_origin) {
  DCHECK_IMPLIES(receiver->IsJSProxy(isolate_), name()->IsPrivate(isolate_));
  DCHECK(receiver.is_identical_to(GetStoreTarget<JSReceiver>()));
  if (state_ == TRANSITION) return;

  if (!IsElement() && name()->IsPrivate(isolate_)) {
    attributes = static_cast<PropertyAttributes>(attributes | DONT_ENUM);
  }

  DCHECK(state_ != LookupIterator::ACCESSOR ||
         (GetAccessors()->IsAccessorInfo(isolate_) &&
          AccessorInfo::cast(*GetAccessors()).is_special_data_property()));
  DCHECK_NE(INTEGER_INDEXED_EXOTIC, state_);
  DCHECK(state_ == NOT_FOUND || !HolderIsReceiverOrHiddenPrototype());

  Handle<Map> map(receiver->map(isolate_), isolate_);

  // Dictionary maps can always have additional data properties.
  if (map->is_dictionary_map()) {
    state_ = TRANSITION;
    if (map->IsJSGlobalObjectMap()) {
      // Install a property cell.
      Handle<JSGlobalObject> global = Handle<JSGlobalObject>::cast(receiver);
      int entry;
      Handle<PropertyCell> cell = JSGlobalObject::EnsureEmptyPropertyCell(
          global, name(), PropertyCellType::kUninitialized, &entry);
      Handle<GlobalDictionary> dictionary(global->global_dictionary(isolate_),
                                          isolate_);
      DCHECK(cell->value(isolate_).IsTheHole(isolate_));
      DCHECK(!value->IsTheHole(isolate_));
      transition_ = cell;
      // Assign an enumeration index to the property and update
      // SetNextEnumerationIndex.
      int index = dictionary->NextEnumerationIndex();
      dictionary->SetNextEnumerationIndex(index + 1);
      property_details_ = PropertyDetails(
          kData, attributes, PropertyCellType::kUninitialized, index);
      PropertyCellType new_type =
          PropertyCell::UpdatedType(isolate(), cell, value, property_details_);
      property_details_ = property_details_.set_cell_type(new_type);
      cell->set_property_details(property_details_);
      number_ = entry;
      has_property_ = true;
    } else {
      // Don't set enumeration index (it will be set during value store).
      property_details_ =
          PropertyDetails(kData, attributes, PropertyCellType::kNoCell);
      transition_ = map;
    }
    return;
  }

  Handle<Map> transition =
      Map::TransitionToDataProperty(isolate_, map, name_, value, attributes,
                                    PropertyConstness::kConst, store_origin);
  state_ = TRANSITION;
  transition_ = transition;

  if (transition->is_dictionary_map()) {
    // Don't set enumeration index (it will be set during value store).
    property_details_ =
        PropertyDetails(kData, attributes, PropertyCellType::kNoCell);
  } else {
    property_details_ = transition->GetLastDescriptorDetails(isolate_);
    has_property_ = true;
  }
}

void LookupIterator::ApplyTransitionToDataProperty(
    Handle<JSReceiver> receiver) {
  DCHECK_EQ(TRANSITION, state_);

  DCHECK(receiver.is_identical_to(GetStoreTarget<JSReceiver>()));
  holder_ = receiver;
  if (receiver->IsJSGlobalObject(isolate_)) {
    JSObject::InvalidatePrototypeChains(receiver->map(isolate_));
    state_ = DATA;
    return;
  }
  Handle<Map> transition = transition_map();
  bool simple_transition =
      transition->GetBackPointer(isolate_) == receiver->map(isolate_);

  if (configuration_ == DEFAULT && !transition->is_dictionary_map() &&
      !transition->IsPrototypeValidityCellValid()) {
    // Only LookupIterator instances with DEFAULT (full prototype chain)
    // configuration can produce valid transition handler maps.
    Handle<Object> validity_cell =
        Map::GetOrCreatePrototypeChainValidityCell(transition, isolate());
    transition->set_prototype_validity_cell(*validity_cell);
  }

  if (!receiver->IsJSProxy(isolate_)) {
    JSObject::MigrateToMap(isolate_, Handle<JSObject>::cast(receiver),
                           transition);
  }

  if (simple_transition) {
    int number = transition->LastAdded();
    number_ = static_cast<uint32_t>(number);
    property_details_ = transition->GetLastDescriptorDetails(isolate_);
    state_ = DATA;
  } else if (receiver->map(isolate_).is_dictionary_map()) {
    Handle<NameDictionary> dictionary(receiver->property_dictionary(isolate_),
                                      isolate_);
    int entry;
    if (receiver->map(isolate_).is_prototype_map() &&
        receiver->IsJSObject(isolate_)) {
      JSObject::InvalidatePrototypeChains(receiver->map(isolate_));
    }
    dictionary = NameDictionary::Add(isolate(), dictionary, name(),
                                     isolate_->factory()->uninitialized_value(),
                                     property_details_, &entry);
    receiver->SetProperties(*dictionary);
    // Reload details containing proper enumeration index value.
    property_details_ = dictionary->DetailsAt(entry);
    number_ = entry;
    has_property_ = true;
    state_ = DATA;

  } else {
    ReloadPropertyInformation<false>();
  }
}

void LookupIterator::Delete() {
  Handle<JSReceiver> holder = Handle<JSReceiver>::cast(holder_);
  if (IsElement()) {
    Handle<JSObject> object = Handle<JSObject>::cast(holder);
    ElementsAccessor* accessor = object->GetElementsAccessor(isolate_);
    accessor->Delete(object, number_);
  } else {
    DCHECK(!name()->IsPrivateName(isolate_));
    bool is_prototype_map = holder->map(isolate_).is_prototype_map();
    RuntimeCallTimerScope stats_scope(
        isolate_, is_prototype_map
                      ? RuntimeCallCounterId::kPrototypeObject_DeleteProperty
                      : RuntimeCallCounterId::kObject_DeleteProperty);

    PropertyNormalizationMode mode =
        is_prototype_map ? KEEP_INOBJECT_PROPERTIES : CLEAR_INOBJECT_PROPERTIES;

    if (holder->HasFastProperties(isolate_)) {
      JSObject::NormalizeProperties(isolate_, Handle<JSObject>::cast(holder),
                                    mode, 0, "DeletingProperty");
      ReloadPropertyInformation<false>();
    }
    JSReceiver::DeleteNormalizedProperty(holder, number_);
    if (holder->IsJSObject(isolate_)) {
      JSObject::ReoptimizeIfPrototype(Handle<JSObject>::cast(holder));
    }
  }
  state_ = NOT_FOUND;
}

void LookupIterator::TransitionToAccessorProperty(
    Handle<Object> getter, Handle<Object> setter,
    PropertyAttributes attributes) {
  DCHECK(!getter->IsNull(isolate_) || !setter->IsNull(isolate_));
  // Can only be called when the receiver is a JSObject. JSProxy has to be
  // handled via a trap. Adding properties to primitive values is not
  // observable.
  Handle<JSObject> receiver = GetStoreTarget<JSObject>();
  if (!IsElement() && name()->IsPrivate(isolate_)) {
    attributes = static_cast<PropertyAttributes>(attributes | DONT_ENUM);
  }

  if (!IsElement() && !receiver->map(isolate_).is_dictionary_map()) {
    Handle<Map> old_map(receiver->map(isolate_), isolate_);

    if (!holder_.is_identical_to(receiver)) {
      holder_ = receiver;
      state_ = NOT_FOUND;
    } else if (state_ == INTERCEPTOR) {
      LookupInRegularHolder<false>(*old_map, *holder_);
    }
    int descriptor =
        IsFound() ? static_cast<int>(number_) : DescriptorArray::kNotFound;

    Handle<Map> new_map = Map::TransitionToAccessorProperty(
        isolate_, old_map, name_, descriptor, getter, setter, attributes);
    bool simple_transition =
        new_map->GetBackPointer(isolate_) == receiver->map(isolate_);
    JSObject::MigrateToMap(isolate_, receiver, new_map);

    if (simple_transition) {
      int number = new_map->LastAdded();
      number_ = static_cast<uint32_t>(number);
      property_details_ = new_map->GetLastDescriptorDetails(isolate_);
      state_ = ACCESSOR;
      return;
    }

    ReloadPropertyInformation<false>();
    if (!new_map->is_dictionary_map()) return;
  }

  Handle<AccessorPair> pair;
  if (state() == ACCESSOR && GetAccessors()->IsAccessorPair(isolate_)) {
    pair = Handle<AccessorPair>::cast(GetAccessors());
    // If the component and attributes are identical, nothing has to be done.
    if (pair->Equals(*getter, *setter)) {
      if (property_details().attributes() == attributes) {
        if (!IsElement()) JSObject::ReoptimizeIfPrototype(receiver);
        return;
      }
    } else {
      pair = AccessorPair::Copy(isolate(), pair);
      pair->SetComponents(*getter, *setter);
    }
  } else {
    pair = factory()->NewAccessorPair();
    pair->SetComponents(*getter, *setter);
  }

  TransitionToAccessorPair(pair, attributes);

#if VERIFY_HEAP
  if (FLAG_verify_heap) {
    receiver->JSObjectVerify(isolate());
  }
#endif
}

void LookupIterator::TransitionToAccessorPair(Handle<Object> pair,
                                              PropertyAttributes attributes) {
  Handle<JSObject> receiver = GetStoreTarget<JSObject>();
  holder_ = receiver;

  PropertyDetails details(kAccessor, attributes, PropertyCellType::kMutable);

  if (IsElement()) {
    // TODO(verwaest): Move code into the element accessor.
    isolate_->CountUsage(v8::Isolate::kIndexAccessor);
    Handle<NumberDictionary> dictionary = JSObject::NormalizeElements(receiver);

    dictionary = NumberDictionary::Set(isolate_, dictionary, index_, pair,
                                       receiver, details);
    receiver->RequireSlowElements(*dictionary);

    if (receiver->HasSlowArgumentsElements(isolate_)) {
      FixedArray parameter_map = FixedArray::cast(receiver->elements(isolate_));
      uint32_t length = parameter_map.length() - 2;
      if (number_ < length) {
        parameter_map.set(number_ + 2,
                          ReadOnlyRoots(isolate_).the_hole_value());
      }
      FixedArray::cast(receiver->elements(isolate_)).set(1, *dictionary);
    } else {
      receiver->set_elements(*dictionary);
    }

    ReloadPropertyInformation<true>();
  } else {
    PropertyNormalizationMode mode = CLEAR_INOBJECT_PROPERTIES;
    if (receiver->map(isolate_).is_prototype_map()) {
      JSObject::InvalidatePrototypeChains(receiver->map(isolate_));
      mode = KEEP_INOBJECT_PROPERTIES;
    }

    // Normalize object to make this operation simple.
    JSObject::NormalizeProperties(isolate_, receiver, mode, 0,
                                  "TransitionToAccessorPair");

    JSObject::SetNormalizedProperty(receiver, name_, pair, details);
    JSObject::ReoptimizeIfPrototype(receiver);

    ReloadPropertyInformation<false>();
  }
}

bool LookupIterator::HolderIsReceiver() const {
  DCHECK(has_property_ || state_ == INTERCEPTOR || state_ == JSPROXY);
  // Optimization that only works if configuration_ is not mutable.
  if (!check_prototype_chain()) return true;
  return *receiver_ == *holder_;
}

bool LookupIterator::HolderIsReceiverOrHiddenPrototype() const {
  DCHECK(has_property_ || state_ == INTERCEPTOR || state_ == JSPROXY);
  // Optimization that only works if configuration_ is not mutable.
  if (!check_prototype_chain()) return true;
  if (*receiver_ == *holder_) return true;
  if (!receiver_->IsJSGlobalProxy(isolate_)) return false;
  return Handle<JSGlobalProxy>::cast(receiver_)->map(isolate_).prototype(
             isolate_) == *holder_;
}

Handle<Object> LookupIterator::FetchValue() const {
  Object result;
  if (IsElement()) {
    Handle<JSObject> holder = GetHolder<JSObject>();
    ElementsAccessor* accessor = holder->GetElementsAccessor(isolate_);
    return accessor->Get(holder, number_);
  } else if (holder_->IsJSGlobalObject(isolate_)) {
    Handle<JSGlobalObject> holder = GetHolder<JSGlobalObject>();
    result = holder->global_dictionary(isolate_).ValueAt(isolate_, number_);
  } else if (!holder_->HasFastProperties(isolate_)) {
    result = holder_->property_dictionary(isolate_).ValueAt(isolate_, number_);
  } else if (property_details_.location() == kField) {
    DCHECK_EQ(kData, property_details_.kind());
    Handle<JSObject> holder = GetHolder<JSObject>();
    FieldIndex field_index =
        FieldIndex::ForDescriptor(holder->map(isolate_), number_);
    return JSObject::FastPropertyAt(holder, property_details_.representation(),
                                    field_index);
  } else {
    result =
        holder_->map(isolate_).instance_descriptors(isolate_).GetStrongValue(
            isolate_, number_);
  }
  return handle(result, isolate_);
}

bool LookupIterator::IsConstFieldValueEqualTo(Object value) const {
  DCHECK(!IsElement());
  DCHECK(holder_->HasFastProperties(isolate_));
  DCHECK_EQ(kField, property_details_.location());
  DCHECK_EQ(PropertyConstness::kConst, property_details_.constness());
  Handle<JSObject> holder = GetHolder<JSObject>();
  FieldIndex field_index =
      FieldIndex::ForDescriptor(holder->map(isolate_), number_);
  if (property_details_.representation().IsDouble()) {
    if (!value.IsNumber(isolate_)) return false;
    uint64_t bits;
    if (holder->IsUnboxedDoubleField(isolate_, field_index)) {
      bits = holder->RawFastDoublePropertyAsBitsAt(field_index);
    } else {
      Object current_value = holder->RawFastPropertyAt(isolate_, field_index);
      DCHECK(current_value.IsMutableHeapNumber(isolate_));
      bits = MutableHeapNumber::cast(current_value).value_as_bits();
    }
    // Use bit representation of double to to check for hole double, since
    // manipulating the signaling NaN used for the hole in C++, e.g. with
    // bit_cast or value(), will change its value on ia32 (the x87 stack is
    // used to return values and stores to the stack silently clear the
    // signalling bit).
    if (bits == kHoleNanInt64) {
      // Uninitialized double field.
      return true;
    }
    return Object::SameNumberValue(bit_cast<double>(bits), value.Number());
  } else {
    Object current_value = holder->RawFastPropertyAt(isolate_, field_index);
    if (current_value.IsUninitialized(isolate()) || current_value == value) {
      return true;
    }
    return current_value.IsNumber(isolate_) && value.IsNumber(isolate_) &&
           Object::SameNumberValue(current_value.Number(), value.Number());
  }
}

int LookupIterator::GetFieldDescriptorIndex() const {
  DCHECK(has_property_);
  DCHECK(holder_->HasFastProperties());
  DCHECK_EQ(kField, property_details_.location());
  DCHECK_EQ(kData, property_details_.kind());
  return descriptor_number();
}

int LookupIterator::GetAccessorIndex() const {
  DCHECK(has_property_);
  DCHECK(holder_->HasFastProperties(isolate_));
  DCHECK_EQ(kDescriptor, property_details_.location());
  DCHECK_EQ(kAccessor, property_details_.kind());
  return descriptor_number();
}

Handle<Map> LookupIterator::GetFieldOwnerMap() const {
  DCHECK(has_property_);
  DCHECK(holder_->HasFastProperties(isolate_));
  DCHECK_EQ(kField, property_details_.location());
  DCHECK(!IsElement());
  Map holder_map = holder_->map(isolate_);
  return handle(holder_map.FindFieldOwner(isolate(), descriptor_number()),
                isolate_);
}

FieldIndex LookupIterator::GetFieldIndex() const {
  DCHECK(has_property_);
  DCHECK(holder_->HasFastProperties(isolate_));
  DCHECK_EQ(kField, property_details_.location());
  DCHECK(!IsElement());
  return FieldIndex::ForDescriptor(holder_->map(isolate_), descriptor_number());
}

Handle<FieldType> LookupIterator::GetFieldType() const {
  DCHECK(has_property_);
  DCHECK(holder_->HasFastProperties(isolate_));
  DCHECK_EQ(kField, property_details_.location());
  return handle(
      holder_->map(isolate_).instance_descriptors(isolate_).GetFieldType(
          isolate_, descriptor_number()),
      isolate_);
}

Handle<PropertyCell> LookupIterator::GetPropertyCell() const {
  DCHECK(!IsElement());
  Handle<JSGlobalObject> holder = GetHolder<JSGlobalObject>();
  return handle(
      holder->global_dictionary(isolate_).CellAt(isolate_, dictionary_entry()),
      isolate_);
}

Handle<Object> LookupIterator::GetAccessors() const {
  DCHECK_EQ(ACCESSOR, state_);
  return FetchValue();
}

Handle<Object> LookupIterator::GetDataValue() const {
  DCHECK_EQ(DATA, state_);
  Handle<Object> value = FetchValue();
  return value;
}

void LookupIterator::WriteDataValue(Handle<Object> value,
                                    bool initializing_store) {
  DCHECK_EQ(DATA, state_);
  Handle<JSReceiver> holder = GetHolder<JSReceiver>();
  if (IsElement()) {
    Handle<JSObject> object = Handle<JSObject>::cast(holder);
    ElementsAccessor* accessor = object->GetElementsAccessor(isolate_);
    accessor->Set(object, number_, *value);
  } else if (holder->HasFastProperties(isolate_)) {
    if (property_details_.location() == kField) {
      // Check that in case of VariableMode::kConst field the existing value is
      // equal to |value|.
      DCHECK_IMPLIES(!initializing_store && property_details_.constness() ==
                                                PropertyConstness::kConst,
                     IsConstFieldValueEqualTo(*value));
      JSObject::cast(*holder).WriteToField(descriptor_number(),
                                           property_details_, *value);
    } else {
      DCHECK_EQ(kDescriptor, property_details_.location());
      DCHECK_EQ(PropertyConstness::kConst, property_details_.constness());
    }
  } else if (holder->IsJSGlobalObject(isolate_)) {
    GlobalDictionary dictionary =
        JSGlobalObject::cast(*holder).global_dictionary(isolate_);
    dictionary.CellAt(isolate_, dictionary_entry()).set_value(*value);
  } else {
    DCHECK_IMPLIES(holder->IsJSProxy(isolate_), name()->IsPrivate(isolate_));
    NameDictionary dictionary = holder->property_dictionary(isolate_);
    dictionary.ValueAtPut(dictionary_entry(), *value);
  }
}

template <bool is_element>
bool LookupIterator::SkipInterceptor(JSObject holder) {
  InterceptorInfo info = GetInterceptor<is_element>(isolate_, holder);
  if (!is_element && name_->IsSymbol(isolate_) &&
      !info.can_intercept_symbols()) {
    return true;
  }
  if (info.non_masking()) {
    switch (interceptor_state_) {
      case InterceptorState::kUninitialized:
        interceptor_state_ = InterceptorState::kSkipNonMasking;
        V8_FALLTHROUGH;
      case InterceptorState::kSkipNonMasking:
        return true;
      case InterceptorState::kProcessNonMasking:
        return false;
    }
  }
  return interceptor_state_ == InterceptorState::kProcessNonMasking;
}

JSReceiver LookupIterator::NextHolder(Map map) {
  DisallowHeapAllocation no_gc;
  if (map.prototype(isolate_) == ReadOnlyRoots(isolate_).null_value()) {
    return JSReceiver();
  }
  if (!check_prototype_chain() && !map.IsJSGlobalProxyMap()) {
    return JSReceiver();
  }
  return JSReceiver::cast(map.prototype(isolate_));
}

LookupIterator::State LookupIterator::NotFound(JSReceiver const holder) const {
  DCHECK(!IsElement());
  if (!holder.IsJSTypedArray(isolate_) || !name_->IsString(isolate_))
    return NOT_FOUND;
  return IsSpecialIndex(String::cast(*name_)) ? INTEGER_INDEXED_EXOTIC
                                              : NOT_FOUND;
}

namespace {

template <bool is_element>
bool HasInterceptor(Map map) {
  return is_element ? map.has_indexed_interceptor()
                    : map.has_named_interceptor();
}

}  // namespace

template <bool is_element>
LookupIterator::State LookupIterator::LookupInSpecialHolder(
    Map const map, JSReceiver const holder) {
  STATIC_ASSERT(INTERCEPTOR == BEFORE_PROPERTY);
  switch (state_) {
    case NOT_FOUND:
      if (map.IsJSProxyMap()) {
        if (is_element || !name_->IsPrivate(isolate_)) return JSPROXY;
      }
      if (map.is_access_check_needed()) {
        if (is_element || !name_->IsPrivate(isolate_)) return ACCESS_CHECK;
      }
      V8_FALLTHROUGH;
    case ACCESS_CHECK:
      if (check_interceptor() && HasInterceptor<is_element>(map) &&
          !SkipInterceptor<is_element>(JSObject::cast(holder))) {
        if (is_element || !name_->IsPrivate(isolate_)) return INTERCEPTOR;
      }
      V8_FALLTHROUGH;
    case INTERCEPTOR:
      if (!is_element && map.IsJSGlobalObjectMap()) {
        GlobalDictionary dict =
            JSGlobalObject::cast(holder).global_dictionary(isolate_);
        int number = dict.FindEntry(isolate(), name_);
        if (number == GlobalDictionary::kNotFound) return NOT_FOUND;
        number_ = static_cast<uint32_t>(number);
        PropertyCell cell = dict.CellAt(isolate_, number_);
        if (cell.value(isolate_).IsTheHole(isolate_)) return NOT_FOUND;
        property_details_ = cell.property_details();
        has_property_ = true;
        switch (property_details_.kind()) {
          case v8::internal::kData:
            return DATA;
          case v8::internal::kAccessor:
            return ACCESSOR;
        }
      }
      return LookupInRegularHolder<is_element>(map, holder);
    case ACCESSOR:
    case DATA:
      return NOT_FOUND;
    case INTEGER_INDEXED_EXOTIC:
    case JSPROXY:
    case TRANSITION:
      UNREACHABLE();
  }
  UNREACHABLE();
}

template <bool is_element>
LookupIterator::State LookupIterator::LookupInRegularHolder(
    Map const map, JSReceiver const holder) {
  DisallowHeapAllocation no_gc;
  if (interceptor_state_ == InterceptorState::kProcessNonMasking) {
    return NOT_FOUND;
  }

  if (is_element) {
    JSObject js_object = JSObject::cast(holder);
    ElementsAccessor* accessor = js_object.GetElementsAccessor(isolate_);
    FixedArrayBase backing_store = js_object.elements(isolate_);
    number_ =
        accessor->GetEntryForIndex(isolate_, js_object, backing_store, index_);
    if (number_ == kMaxUInt32) {
      return holder.IsJSTypedArray(isolate_) ? INTEGER_INDEXED_EXOTIC
                                             : NOT_FOUND;
    }
    property_details_ = accessor->GetDetails(js_object, number_);
    if (map.has_frozen_or_sealed_elements()) {
      PropertyAttributes attrs = map.has_sealed_elements() ? SEALED : FROZEN;
      property_details_ = property_details_.CopyAddAttributes(attrs);
    }
  } else if (!map.is_dictionary_map()) {
    DescriptorArray descriptors = map.instance_descriptors(isolate_);
    int number = descriptors.SearchWithCache(isolate_, *name_, map);
    if (number == DescriptorArray::kNotFound) return NotFound(holder);
    number_ = static_cast<uint32_t>(number);
    property_details_ = descriptors.GetDetails(number_);
  } else {
    DCHECK_IMPLIES(holder.IsJSProxy(isolate_), name()->IsPrivate(isolate_));
    NameDictionary dict = holder.property_dictionary(isolate_);
    int number = dict.FindEntry(isolate(), name_);
    if (number == NameDictionary::kNotFound) return NotFound(holder);
    number_ = static_cast<uint32_t>(number);
    property_details_ = dict.DetailsAt(number_);
  }
  has_property_ = true;
  switch (property_details_.kind()) {
    case v8::internal::kData:
      return DATA;
    case v8::internal::kAccessor:
      return ACCESSOR;
  }

  UNREACHABLE();
}

Handle<InterceptorInfo> LookupIterator::GetInterceptorForFailedAccessCheck()
    const {
  DCHECK_EQ(ACCESS_CHECK, state_);
  DisallowHeapAllocation no_gc;
  AccessCheckInfo access_check_info =
      AccessCheckInfo::Get(isolate_, Handle<JSObject>::cast(holder_));
  if (!access_check_info.is_null()) {
    Object interceptor = IsElement() ? access_check_info.indexed_interceptor()
                                     : access_check_info.named_interceptor();
    if (interceptor != Object()) {
      return handle(InterceptorInfo::cast(interceptor), isolate_);
    }
  }
  return Handle<InterceptorInfo>();
}

bool LookupIterator::TryLookupCachedProperty() {
  return state() == LookupIterator::ACCESSOR &&
         GetAccessors()->IsAccessorPair(isolate_) && LookupCachedProperty();
}

bool LookupIterator::LookupCachedProperty() {
  DCHECK_EQ(state(), LookupIterator::ACCESSOR);
  DCHECK(GetAccessors()->IsAccessorPair(isolate_));

  AccessorPair accessor_pair = AccessorPair::cast(*GetAccessors());
  Handle<Object> getter(accessor_pair.getter(isolate_), isolate());
  MaybeHandle<Name> maybe_name =
      FunctionTemplateInfo::TryGetCachedPropertyName(isolate(), getter);
  if (maybe_name.is_null()) return false;

  // We have found a cached property! Modify the iterator accordingly.
  name_ = maybe_name.ToHandleChecked();
  Restart();
  CHECK_EQ(state(), LookupIterator::DATA);
  return true;
}

}  // namespace internal
}  // namespace v8
