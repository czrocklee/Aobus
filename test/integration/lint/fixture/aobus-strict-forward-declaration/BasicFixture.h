// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <memory>
#include <string>

// Mock Glib::RefPtr
namespace Glib
{
  template<typename T>
  class RefPtr
  {
  public:
    T* get() const { return nullptr; }
  };
}

// WeakOnlyTypes.h contains types used ONLY via pointer — safe to forward-declare.
// StrongTypes.h contains types used by-value or as base classes.
#include "StrongTypes.h"
#include "WeakOnlyTypes.h"

// --- NEGATIVE CASES: Must be fully defined or already forward declared ---

class ExistingFwdDecl;
class TestExistingFwdDecl
{
  // NEGATIVE
  ExistingFwdDecl* _ptr;
};

class TargetForTypedef
{};
typedef TargetForTypedef TargetTypedef;
class TestTypedefRef
{
  // NEGATIVE
  TargetTypedef* _ptr;
};

class TestSharedHeader : public SharedHeaderBase
{
  // NEGATIVE - No warning because SharedHeaderUsed shares the header StrongTypes.h with SharedHeaderBase
  SharedHeaderUsed* _ptr;
};

// --- POSITIVE CASES: Can be forward declared ---

class BadUniquePtr
{
  // POSITIVE
  std::unique_ptr<TargetBadUniquePtr> _ptr;
  ~BadUniquePtr();
};

class BadSharedPtr
{
  // POSITIVE
  std::shared_ptr<TargetBadSharedPtr> _ptr;
};

class BadRefPtr
{
  // POSITIVE
  Glib::RefPtr<TargetBadRefPtr> _ptr;
};

class BadRawPtr
{
  // POSITIVE
  TargetBadRawPtr* _ptr;
};

class BadReference
{
  // POSITIVE
  void setTarget(TargetBadRef& target);
};

// --- NEGATIVE CASES: Must be fully defined ---

class GoodInheritance : public TargetGoodInheritance
{
  // NEGATIVE
  int x;
};

class GoodValueType
{
  // NEGATIVE
  TargetGoodValueType _value;
};

class GoodInlineCall
{
  std::unique_ptr<TargetGoodInlineCall> _ptr;

public:
  // NEGATIVE
  void inlineCall() { _ptr->doSomething(); }
};

class GoodByValueParam
{
  // NEGATIVE
  void passByValue(TargetGoodByValParam val);
};

class GoodByValueReturn
{
  // NEGATIVE
  TargetGoodByValRet returnByValue();
};

class GoodSystemHeaderParam
{
  // NEGATIVE
  void passSystemString(std::string const& str);
};

class GoodSystemHeaderPtr
{
  // NEGATIVE
  std::string* _strPtr;
};
