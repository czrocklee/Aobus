// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

namespace associated
{
  struct Arg
  {
    int value;
  };

  struct Convertible
  {
    constexpr operator int() const { return 0; }
  };

  enum class Choice
  {
    Value
  };

  struct Hidden
  {
    friend constexpr int hiddenPick(Hidden, int) { return 22; }
  };

  constexpr int pick(Arg, int)
  {
    return 22;
  }
  constexpr int pick(Convertible, int)
  {
    return 22;
  }
  constexpr int pointer(Arg const*, int)
  {
    return 22;
  }
  constexpr int reference(Arg const&, int)
  {
    return 22;
  }
  constexpr int array(Arg const*, int)
  {
    return 22;
  }
  constexpr int enumeration(Choice, int)
  {
    return 22;
  }
  constexpr int callback(void (*)(Arg), int)
  {
    return 22;
  }
  constexpr int member(int Arg::*, int)
  {
    return 22;
  }
  constexpr int overload(void (*)(int), int)
  {
    return 22;
  }

  void argumentFunction(Arg);
  void overloaded(int);
  void overloaded(Arg);

  inline constexpr Arg kArgs[1]{};
}

namespace ao::adl
{
  constexpr int pick(associated::Arg, long)
  {
    return 11;
  }
  constexpr int pick(int, int)
  {
    return 11;
  }
  constexpr int pointer(associated::Arg const*, long)
  {
    return 11;
  }
  constexpr int reference(associated::Arg const&, long)
  {
    return 11;
  }
  constexpr int array(associated::Arg const*, long)
  {
    return 11;
  }
  constexpr int enumeration(associated::Choice, long)
  {
    return 11;
  }
  constexpr int hiddenPick(associated::Hidden, long)
  {
    return 11;
  }
  constexpr int callback(void (*)(associated::Arg), long)
  {
    return 11;
  }
  constexpr int member(int associated::Arg::*, long)
  {
    return 11;
  }
  constexpr int overload(void (*)(int), long)
  {
    return 11;
  }

  constexpr int zero()
  {
    return 11;
  }
  constexpr int basic(int, double, decltype(nullptr))
  {
    return 11;
  }
  constexpr int defaulted(int = 1, associated::Arg = {})
  {
    return 11;
  }

  template<typename T>
  constexpr int explicitPick(int, int)
  {
    return 11;
  }

  namespace detail
  {
    constexpr int pick(associated::Arg, long)
    {
      return 11;
    }
  }
}

namespace ao::adl::implementation
{
  // NEGATIVE - complete removal activates a better overload from ADL.
  constexpr int kDirect = adl::pick(associated::Arg{}, 1);
  static_assert(kDirect == 11);
  // NEGATIVE - the source class contributes to ADL before conversion to int.
  constexpr int kConverted = adl::pick(associated::Convertible{}, 1);
  static_assert(kConverted == 11);
  // NEGATIVE - a pointer contributes its pointee's associated namespace.
  constexpr int kPointer = adl::pointer(static_cast<associated::Arg const*>(nullptr), 1);
  static_assert(kPointer == 11);
  // NEGATIVE - reference binding does not erase the argument's class association.
  constexpr int kReference = adl::reference(associated::kArgs[0], 1);
  static_assert(kReference == 11);
  // NEGATIVE - inspect the array before its implicit pointer decay.
  constexpr int kArray = adl::array(associated::kArgs, 1);
  static_assert(kArray == 11);
  // NEGATIVE - enumeration types also contribute associated namespaces.
  constexpr int kEnumeration = adl::enumeration(associated::Choice::Value, 1);
  static_assert(kEnumeration == 11);
  // NEGATIVE - ADL sees a hidden friend absent from ordinary namespace lookup.
  constexpr int kHidden = adl::hiddenPick(associated::Hidden{}, 1);
  static_assert(kHidden == 11);
  // NEGATIVE - function parameter types contribute even before function decay.
  constexpr int kFunction = adl::callback(associated::argumentFunction, 1);
  static_assert(kFunction == 11);
  // NEGATIVE - a function pointer can contribute associated argument types.
  constexpr int kFunctionPointer = adl::callback(&associated::argumentFunction, 1);
  static_assert(kFunctionPointer == 11);
  // NEGATIVE - member pointers contribute the declaring class.
  constexpr int kMember = adl::member(&associated::Arg::value, 1);
  static_assert(kMember == 11);
  // NEGATIVE - the source overload set includes the overload taking Arg.
  constexpr int kOverload = adl::overload(associated::overloaded, 1);
  static_assert(kOverload == 11);

  template<typename T>
  constexpr int dependent()
  {
    // NEGATIVE - instantiation must not prove the dependent source type builtin.
    return adl::pick(T{}, 1);
  }

  static_assert(dependent<int>() == 11);
  static_assert(dependent<associated::Convertible>() == 11);

  // NEGATIVE - explicit template arguments are outside the bounded ADL proof.
  constexpr int kExplicit = adl::explicitPick<associated::Arg>(1, 1);
  static_assert(kExplicit == 11);

  template<typename T>
  constexpr int dependentTemplateArgument()
  {
    // NEGATIVE - the explicit source template argument is dependent.
    return adl::explicitPick<T>(1, 1);
  }

  static_assert(dependentTemplateArgument<associated::Arg>() == 11);

  // POSITIVE: FIX-TO: static_assert(zero() == 11);
  static_assert(adl::zero() == 11);
  // POSITIVE: FIX-TO: static_assert(basic(1, 2.0, nullptr) == 11);
  static_assert(adl::basic(1, 2.0, nullptr) == 11);
  // POSITIVE: FIX-TO: static_assert(pick(1, 1) == 11);
  static_assert(adl::pick(1, 1) == 11);
  // POSITIVE: FIX-TO: static_assert(pointer(nullptr, 1) == 11);
  static_assert(adl::pointer(nullptr, 1) == 11);
  // POSITIVE: FIX-TO: static_assert(pick(static_cast<int>(associated::Convertible{}), 1) == 11);
  static_assert(adl::pick(static_cast<int>(associated::Convertible{}), 1) == 11);
  // POSITIVE: FIX-TO: static_assert(defaulted() == 11);
  static_assert(adl::defaulted() == 11);
  // POSITIVE: FIX-TO: static_assert(defaulted(1) == 11);
  static_assert(adl::defaulted(1) == 11);
  // NEGATIVE - an explicitly supplied class argument is not a default argument.
  static_assert(adl::defaulted(1, associated::Arg{}) == 11);
  // POSITIVE: FIX-TO: static_assert(detail::pick(associated::Arg{}, 1) == 11);
  static_assert(adl::detail::pick(associated::Arg{}, 1) == 11);
  // POSITIVE: FIX-TO: static_assert((pick)(associated::Arg{}, 1) == 11);
  static_assert((adl::pick)(associated::Arg{}, 1) == 11);
  // POSITIVE: FIX-TO: static_assert((&reference)(associated::kArgs[0], 1) == 11);
  static_assert((&adl::reference)(associated::kArgs[0], 1) == 11);

  // NEGATIVE - an unrecognized non-call parent is conservatively unsupported.
  constexpr auto kFunctionAddress = adl::zero;
  static_assert(kFunctionAddress() == 11);
}
