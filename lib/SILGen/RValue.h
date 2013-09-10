//===--- RValue.h - Exploded RValue Representation --------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// A storage structure for holding a destructured rvalue with an optional
// cleanup(s).
// Ownership of the rvalue can be "forwarded" to disable the associated
// cleanup(s).
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_LOWERING_RVALUE_H
#define SWIFT_LOWERING_RVALUE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "SILGen.h"

namespace swift {
  class SILValue;
  
namespace Lowering {
  class Initialization;

/// ManagedValue - represents a SIL rvalue. It consists of a SILValue and an
/// optional cleanup. Ownership of the ManagedValue can be "forwarded" to
/// disable its cleanup when the rvalue is consumed. A ManagedValue can also
/// represent an LValue used as a value, such as a [byref] function argument.
class ManagedValue {
  /// The value (or address of an address-only value) being managed, and
  /// whether it represents an lvalue.
  llvm::PointerIntPair<SILValue, 1, bool> valueAndIsLValue;
  /// A handle to the cleanup that destroys this value, or
  /// CleanupsDepth::invalid if the value has no cleanup.
  CleanupsDepth cleanup;

public:
  enum Unmanaged_t { Unmanaged };
  enum LValue_t { LValue };
  
  ManagedValue() = default;
  explicit ManagedValue(SILValue value, LValue_t)
    : valueAndIsLValue(value, true),
      cleanup(CleanupsDepth::invalid())
  {}
  explicit ManagedValue(SILValue value, Unmanaged_t)
    : valueAndIsLValue(value, false),
      cleanup(CleanupsDepth::invalid())
  {}
  ManagedValue(SILValue value, CleanupsDepth cleanup)
    : valueAndIsLValue(value, false),
      cleanup(cleanup)
  {}

  SILValue getUnmanagedValue() const {
    assert(!hasCleanup());
    return getValue();
  }
  SILValue getValue() const { return valueAndIsLValue.getPointer(); }
  
  SILType getType() const { return getValue().getType(); }
  
  bool isLValue() const { return valueAndIsLValue.getInt(); }

  CanType getSwiftType() const {
    return isLValue()
      ? getType().getSwiftType()
      : getType().getSwiftRValueType();
  }
  
  /// Emit a copy of this value with independent ownership.
  ManagedValue copy(SILGenFunction &gen, SILLocation l) {
    if (!cleanup.isValid()) {
      assert(gen.getTypeLowering(getType()).isTrivial());
      return *this;
    }
    
    auto &lowering = gen.getTypeLowering(getType());
    assert(!lowering.isTrivial() && "trivial value has cleanup?");

    if (lowering.isAddressOnly()) {
      SILValue buf = gen.emitTemporaryAllocation(l, getType());
      gen.B.createCopyAddr(l, getValue(), buf,
                           IsNotTake, IsInitialization);
      return gen.emitManagedRValueWithCleanup(buf, lowering);
    }
    lowering.emitRetain(gen.B, l, getValue());
    return gen.emitManagedRValueWithCleanup(getValue(), lowering);
  }
  
  /// Store a copy of this value with independent ownership into the given
  /// uninitialized address.
  void copyInto(SILGenFunction &gen, SILValue dest, SILLocation L) {
    auto &lowering = gen.getTypeLowering(getType());
    if (lowering.isAddressOnly()) {
      gen.B.createCopyAddr(L, getValue(), dest,
                           IsNotTake, IsInitialization);
      return;
    }
    lowering.emitRetain(gen.B, L, getValue());
    gen.B.createStore(L, getValue(), dest);
  }
  
  bool hasCleanup() const { return cleanup.isValid(); }
  CleanupsDepth getCleanup() const { return cleanup; }

  /// Disable the cleanup for this value.
  void forwardCleanup(SILGenFunction &gen) {
    assert(hasCleanup() && "value doesn't have cleanup!");
    gen.Cleanups.setCleanupState(getCleanup(), CleanupState::Dead);
  }
  
  /// Forward this value, deactivating the cleanup and returning the
  /// underlying value.
  SILValue forward(SILGenFunction &gen) {
    if (hasCleanup())
      forwardCleanup(gen);
    return getValue();
  }
  
  /// Forward this value as an argument.
  SILValue forwardArgument(SILGenFunction &gen, SILLocation loc,
                           AbstractCC cc, CanType bridgedTy) {
    // Bridge the value to the current calling convention.
    ManagedValue v = gen.emitNativeToBridgedValue(loc, *this, cc, bridgedTy);
    return v.forward(gen);
  }
  
  /// Get this value as an argument without consuming it.
  SILValue getArgumentValue(SILGenFunction &gen, SILLocation loc,
                            AbstractCC cc, CanType bridgedTy) {
    // Bridge the value to the current calling convention.
    ManagedValue v = gen.emitNativeToBridgedValue(loc, *this, cc, bridgedTy);
    return v.getValue();
  }
  
  /// Forward this value into memory by storing it to the given address.
  ///
  /// \param gen - The SILGenFunction.
  /// \param loc - the AST location to associate with emitted instructions.
  /// \param address - the address to assign to.
  void forwardInto(SILGenFunction &gen, SILLocation loc, SILValue address);
  
  /// Assign this value into memory, destroying the existing
  /// value at the destination address.
  ///
  /// \param gen - The SILGenFunction.
  /// \param loc - the AST location to associate with emitted instructions.
  /// \param address - the address to assign to.
  void assignInto(SILGenFunction &gen, SILLocation loc, SILValue address);
  
  explicit operator bool() const {
    return bool(getValue());
  }
};
  
/// An "exploded" SIL rvalue, in which tuple values are recursively
/// destructured. (In SILGen we don't try to explode structs, because doing so
/// would require considering resilience, a job we want to delegate to IRGen).
class RValue {
  std::vector<ManagedValue> values;
  CanType type;
  unsigned elementsToBeAdded;
  
  /// Flag value used to mark an rvalue as invalid, because it was
  /// consumed or it was default-initialized.
  enum : unsigned { Used = ~0U };
  
  // Don't copy.
  RValue(const RValue &) = delete;
  RValue &operator=(const RValue &) = delete;
  
  void makeUsed() {
    elementsToBeAdded = Used;
    values = {};
  }
  
  /// Private constructor used by copy().
  RValue(const RValue &copied, SILGenFunction &gen, SILLocation l);
  
public:
  /// Creates an invalid RValue object, in a "used" state.
  RValue() : elementsToBeAdded(Used) {}
  
  RValue(RValue &&rv)
    : values(std::move(rv.values)),
      type(rv.type),
      elementsToBeAdded(rv.elementsToBeAdded)
  {
    assert((rv.isComplete() || rv.isUsed())
           && "moving rvalue that wasn't complete?!");
    rv.elementsToBeAdded = Used;
  }
  RValue &operator=(RValue &&rv) {
    assert(isUsed() && "reassigning an unused rvalue?!");
    
    assert((rv.isComplete() || rv.isUsed())
           && "moving rvalue that wasn't complete?!");
    values = std::move(rv.values);
    type = rv.type;
    elementsToBeAdded = rv.elementsToBeAdded;
    rv.elementsToBeAdded = Used;
    return *this;
  }
  
  /// Create a RValue from a single value. If the value is of tuple type, it
  /// will be exploded.
  RValue(SILGenFunction &gen, ManagedValue v, SILLocation l);

  /// Construct an RValue from a pre-exploded set of
  /// ManagedValues. Used to implement the extractElement* methods.
  RValue(ArrayRef<ManagedValue> values, CanType type);
  
  /// Create an RValue to which values will be subsequently added using
  /// addElement(). The RValue will not be complete until all the elements have
  /// been added.
  explicit RValue(CanType type);
  
  /// Create an RValue by emitting destructured arguments into a basic block.
  static RValue emitBBArguments(CanType type,
                                SILGenFunction &gen,
                                SILBasicBlock *parent,
                                SILLocation L);
  
  /// True if the rvalue has been completely initialized by adding all its
  /// elements.
  bool isComplete() const & { return elementsToBeAdded == 0; }
  
  /// True if this rvalue has been used.
  bool isUsed() const & { return elementsToBeAdded == Used; }
  explicit operator bool() const & { return !isUsed(); }
  
  /// True if this represents an lvalue.
  bool isLValue() const & { return isa<LValueType>(type); }
  
  /// Add an element to the rvalue. The rvalue must not yet be complete.
  void addElement(RValue &&element) &;
  
  /// Add a ManagedValue element to the rvalue, exploding tuples if necessary.
  /// The rvalue must not yet be complete.
  void addElement(SILGenFunction &gen, ManagedValue element, SILLocation l) &;
  
  /// Forward an rvalue into a single value, imploding tuples if necessary.
  SILValue forwardAsSingleValue(SILGenFunction &gen, SILLocation l) &&;

  /// Get the rvalue as a single value, imploding tuples if necessary.
  ManagedValue getAsSingleValue(SILGenFunction &gen, SILLocation l) &&;
  
  /// Get the rvalue as a single unmanaged value, imploding tuples if necessary.
  /// The values must not require any cleanups.
  SILValue getUnmanagedSingleValue(SILGenFunction &gen, SILLocation l) const &;
  
  /// Peek at the single scalar value backing this rvalue without consuming it.
  /// The rvalue must not be of a tuple type.
  SILValue peekScalarValue() const & {
    assert(!isa<TupleType>(type) && "peekScalarValue of a tuple rvalue");
    assert(values.size() == 1 && "exploded scalar value?!");
    return values[0].getValue();
  }

  /// Use this rvalue to initialize an Initialization.
  void forwardInto(SILGenFunction &gen, Initialization *I, SILLocation Loc) &&;
  
  /// Forward the exploded SILValues into a SmallVector.
  void forwardAll(SILGenFunction &gen,
                  SmallVectorImpl<SILValue> &values) &&;

  
  /// Take the ManagedValues from this RValue into a SmallVector.
  void getAll(SmallVectorImpl<ManagedValue> &values) &&;
  
  /// Store the unmanaged SILValues into a SmallVector. The values must not
  /// require any cleanups.
  void getAllUnmanaged(SmallVectorImpl<SILValue> &values) const &;
  
  /// Extract a single tuple element from the rvalue.
  RValue extractElement(unsigned element) &&;
  
  /// Extract the tuple elements from the rvalue.
  void extractElements(SmallVectorImpl<RValue> &elements) &&;
  
  CanType getType() const & { return type; }
  
  /// Emit an equivalent value with independent ownership.
  RValue copy(SILGenFunction &gen, SILLocation l) const & {
    return RValue(*this, gen, l);
  }
};

} // end namespace Lowering
} // end namespace swift

#endif
