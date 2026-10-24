// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

namespace retained
{
  struct Outer
  {
    struct Inner
    {
      int value;
    };
    static constexpr int staticFn() { return 11; }
    template<typename T>
    using Nested = T;
  };
}
namespace foreign
{
  struct Different
  {
    using Inner = int;
    static constexpr int staticFn() { return 22; }
  };
}
namespace retained::sourceScopes
{
  using Foreign = ::foreign::Different;
  using ExpectedOuter = ::retained::Outer;
  using ExpectedInner = ::retained::Outer::Inner;
  template<int N>
  struct Tag
  {};
}

namespace retained::sourceScopes::shadowedRequiresFunction
{
  template<typename Outer>
  // NEGATIVE - the earlier template parameter shadows the surviving name.
    requires requires(retained::Outer::Inner i) { i.value; }
  constexpr int r1()
  {
    return 11;
  }
  template<typename T>
  concept Callable = requires { r1<T>(); };
  static_assert(r1<Foreign>() == 11);
}

namespace retained::sourceScopes::unshadowedRequiresFunction
{
  template<typename UnrelatedParameter>
  // POSITIVE
    requires requires(retained::Outer::Inner i) { i.value; }
  constexpr int r1()
  {
    return 11;
  }
  template<typename T>
  concept Callable = requires { r1<T>(); };
  static_assert(r1<Foreign>() == 11);
}

namespace retained::sourceScopes::shadowedRequiresNestedConcept
{
  template<typename Outer>
  concept D9 = requires {
    // NEGATIVE - the earlier template parameter shadows the surviving name.
    requires requires(retained::Outer::Inner i) { i.value; };
  };
  static_assert(D9<Foreign>);
}

namespace retained::sourceScopes::unshadowedRequiresNestedConcept
{
  template<typename UnrelatedParameter>
  concept D9 = requires {
    // POSITIVE
    requires requires(retained::Outer::Inner i) { i.value; };
  };
  static_assert(D9<Foreign>);
}

namespace retained::sourceScopes::shadowedRequiresVariableSpecialization
{
  template<typename T>
  constexpr int kValue = 22;

  template<typename Outer>
  // NEGATIVE - the partial specialization's parameter shadows the surviving name.
    requires requires(retained::Outer::Inner i) {
      i.value;
      requires __is_same(decltype(i), ExpectedInner);
    }
  constexpr int kValue<Outer> = 11;
  static_assert(kValue<Foreign> == 11);
}

namespace retained::sourceScopes::unshadowedRequiresVariableSpecialization
{
  template<typename T>
  constexpr int kValue = 22;

  template<typename UnrelatedParameter>
  // POSITIVE
    requires requires(retained::Outer::Inner i) {
      i.value;
      requires __is_same(decltype(i), ExpectedInner);
    }
  constexpr int kValue<UnrelatedParameter> = 11;
  static_assert(kValue<Foreign> == 11);
}

namespace retained::sourceScopes::shadowedLambdaDefault
{
  template<typename Outer>
  // NEGATIVE - the variable template's parameter is visible inside the lambda default.
  constexpr auto kValue = []<typename Selected = retained::Outer::Inner>() constexpr { return Selected{}; };
  static_assert(__is_same(decltype(kValue<Foreign>()), ExpectedInner));
}

namespace retained::sourceScopes::unshadowedLambdaDefault
{
  template<typename UnrelatedParameter>
  // POSITIVE
  constexpr auto kValue = []<typename Selected = retained::Outer::Inner>() constexpr { return Selected{}; };
  static_assert(__is_same(decltype(kValue<Foreign>()), ExpectedInner));
}

namespace retained::sourceScopes::shadowedVariableTypeDefault
{
  // NEGATIVE - the earlier template parameter shadows the surviving name.
  template<typename Outer, typename U = retained::Outer::Inner>
  constexpr bool kValue = __is_same(U, ExpectedInner);
  static_assert(kValue<Foreign>);
}

namespace retained::sourceScopes::unshadowedVariableTypeDefault
{
  // POSITIVE
  template<typename UnrelatedParameter, typename U = retained::Outer::Inner>
  constexpr bool kValue = __is_same(U, ExpectedInner);
  static_assert(kValue<Foreign>);
}

namespace retained::sourceScopes::shadowedVariableValueDefault
{
  // NEGATIVE - the earlier template parameter shadows the surviving name.
  template<typename Outer, int N = retained::Outer::staticFn()>
  constexpr int kValue = N;
  static_assert(kValue<Foreign> == 11);
}

namespace retained::sourceScopes::unshadowedVariableValueDefault
{
  // POSITIVE
  template<typename UnrelatedParameter, int N = retained::Outer::staticFn()>
  constexpr int kValue = N;
  static_assert(kValue<Foreign> == 11);
}

namespace retained::sourceScopes::shadowedVariablePointerParameter
{
  inline ExpectedInner object{};
  // NEGATIVE - the earlier template parameter shadows the surviving name.
  template<typename Outer, retained::Outer::Inner* Pointer = nullptr>
  constexpr int kValue = 11;
  template<typename T, auto* P>
  concept Accepts = requires { kValue<T, P>; };
  static_assert(kValue<Foreign, &object> == 11);
}

namespace retained::sourceScopes::unshadowedVariablePointerParameter
{
  inline ExpectedInner object{};
  // POSITIVE
  template<typename UnrelatedParameter, retained::Outer::Inner* Pointer = nullptr>
  constexpr int kValue = 11;
  template<typename T, auto* P>
  concept Accepts = requires { kValue<T, P>; };
  static_assert(kValue<Foreign, &object> == 11);
}

namespace retained::sourceScopes::shadowedAliasTypeDefault
{
  // NEGATIVE - the earlier template parameter shadows the surviving name.
  template<typename Outer, typename U = retained::Outer::Inner>
  using Selected = U;
  static_assert(__is_same(Selected<Foreign>, ExpectedInner));
}

namespace retained::sourceScopes::unshadowedAliasTypeDefault
{
  // POSITIVE
  template<typename UnrelatedParameter, typename U = retained::Outer::Inner>
  using Selected = U;
  static_assert(__is_same(Selected<Foreign>, ExpectedInner));
}

namespace retained::sourceScopes::shadowedAliasValueDefault
{
  // NEGATIVE - the earlier template parameter shadows the surviving name.
  template<typename Outer, int N = retained::Outer::staticFn()>
  using Selected = Tag<N>;
  static_assert(__is_same(Selected<Foreign>, Tag<11>));
}

namespace retained::sourceScopes::unshadowedAliasValueDefault
{
  // POSITIVE
  template<typename UnrelatedParameter, int N = retained::Outer::staticFn()>
  using Selected = Tag<N>;
  static_assert(__is_same(Selected<Foreign>, Tag<11>));
}

namespace retained::sourceScopes::shadowedAliasPointerParameter
{
  inline ExpectedInner object{};
  // NEGATIVE - the earlier template parameter shadows the surviving name.
  template<typename Outer, retained::Outer::Inner* Pointer = nullptr>
  using Selected = Tag<11>;
  template<typename T, auto* P>
  concept Accepts = requires { typename Selected<T, P>; };
  static_assert(__is_same(Selected<Foreign, &object>, Tag<11>));
}

namespace retained::sourceScopes::unshadowedAliasPointerParameter
{
  inline ExpectedInner object{};
  // POSITIVE
  template<typename UnrelatedParameter, retained::Outer::Inner* Pointer = nullptr>
  using Selected = Tag<11>;
  template<typename T, auto* P>
  concept Accepts = requires { typename Selected<T, P>; };
  static_assert(__is_same(Selected<Foreign, &object>, Tag<11>));
}

namespace retained::sourceScopes::shadowedVariableConstrainedType
{
  template<typename L, typename R>
  concept Same = __is_same(L, R);
  // NEGATIVE - the earlier template parameter shadows the surviving name.
  template<typename Outer, Same<retained::Outer> U>
  constexpr bool kValue = true;
  template<typename T, typename U>
  concept Accepts = requires { kValue<T, U>; };
  static_assert(kValue<Foreign, ExpectedOuter>);
}

namespace retained::sourceScopes::unshadowedVariableConstrainedType
{
  template<typename L, typename R>
  concept Same = __is_same(L, R);
  // POSITIVE
  template<typename UnrelatedParameter, Same<retained::Outer> U>
  constexpr bool kValue = true;
  template<typename T, typename U>
  concept Accepts = requires { kValue<T, U>; };
  static_assert(kValue<Foreign, ExpectedOuter>);
}

namespace retained::sourceScopes::shadowedAliasConstrainedType
{
  template<typename L, typename R>
  concept Same = __is_same(L, R);
  // NEGATIVE - the earlier template parameter shadows the surviving name.
  template<typename Outer, Same<retained::Outer> U>
  using Selected = U;
  template<typename T, typename U>
  concept Accepts = requires { typename Selected<T, U>; };
  static_assert(__is_same(Selected<Foreign, ExpectedOuter>, ExpectedOuter));
}

namespace retained::sourceScopes::unshadowedAliasConstrainedType
{
  template<typename L, typename R>
  concept Same = __is_same(L, R);
  // POSITIVE
  template<typename UnrelatedParameter, Same<retained::Outer> U>
  using Selected = U;
  template<typename T, typename U>
  concept Accepts = requires { typename Selected<T, U>; };
  static_assert(__is_same(Selected<Foreign, ExpectedOuter>, ExpectedOuter));
}

namespace retained::sourceScopes::lateParameter
{
  // POSITIVE
  template<typename Selected = retained::Outer::Inner, typename Outer = Foreign>
  using LateAlias = Selected;
  static_assert(__is_same(LateAlias<>, ExpectedInner));

  // POSITIVE
  template<typename Selected = retained::Outer::Inner, typename Outer = Foreign>
  constexpr bool kLateValue = __is_same(Selected, ExpectedInner);
  static_assert(kLateValue<>);
}

namespace retained::sourceScopes::localParameter
{
  template<typename UnrelatedParameter>
  // POSITIVE - a variable named Outer does not hide the type before ::.
  concept HasUnshadowedType = requires(int Outer, retained::Outer::Inner value) { value.value; };
  static_assert(HasUnshadowedType<Foreign>);
}

namespace retained::sourceScopes::requiresLocalValue
{
  inline constexpr int kValue = 1;

  template<typename T>
  concept PreservesValue = requires(T kValue) {
    // NEGATIVE - the local parameter would replace the constant in the body.
    requires requiresLocalValue::kValue == 1;
  };
  static_assert(PreservesValue<int>);

  template<typename T>
  concept UnshadowedValue = requires(T unrelated) {
    // POSITIVE
    requires requiresLocalValue::kValue == 1;
  };
  static_assert(UnshadowedValue<int>);

  template<int N>
  struct SizeTag
  {};
  template<>
  struct SizeTag<sizeof(int)>
  {
    using Expected = void;
  };
  static_assert(sizeof(char) != sizeof(int));

  template<typename T>
  concept PreservesSize = requires(T kValue) {
    // NEGATIVE - sizeof(char) must not replace the namespace constant's size.
    typename SizeTag<sizeof(requiresLocalValue::kValue)>::Expected;
  };
  static_assert(PreservesSize<char>);

  template<typename T>
  concept UnshadowedSize = requires(T unrelated) {
    // POSITIVE
    typename SizeTag<sizeof(requiresLocalValue::kValue)>::Expected;
  };
  static_assert(UnshadowedSize<char>);

  template<typename T>
    requires requires(T kValue) {
      // NEGATIVE - a function's requires-expression has the same local scope.
      requires requiresLocalValue::kValue == 1;
    }
  constexpr bool preservesConstraint()
  {
    return true;
  }
  static_assert(preservesConstraint<int>());

  template<typename T>
    requires requires(T unrelated) {
      // POSITIVE
      requires requiresLocalValue::kValue == 1;
    }
  constexpr bool hasUnshadowedConstraint()
  {
    return true;
  }
  static_assert(hasUnshadowedConstraint<int>());

  template<typename T>
  // POSITIVE - a later parameter is not yet visible in this parameter's type.
  concept BeforeParameter = requires(decltype(requiresLocalValue::kValue)* selected, T kValue) {
    requires __is_same(decltype(*selected), int const&);
  };
  static_assert(BeforeParameter<char>);

  template<typename T>
  // NEGATIVE - an earlier local parameter changes the following parameter's type.
  concept AfterParameter = requires(T kValue, decltype(requiresLocalValue::kValue)* selected) {
    requires __is_same(decltype(*selected), int const&);
  };
  static_assert(AfterParameter<char>);
}

namespace retained::sourceScopes::classControl
{
  // NEGATIVE - the existing class-context template-parameter guard must remain effective.
  template<typename Outer, typename Selected = retained::Outer::Inner>
  struct Host
  {
    using Type = Selected;
  };
  static_assert(__is_same(typename Host<Foreign>::Type, ExpectedInner));
}

namespace retained::sourceScopes::attributeControl
{
  // The attribute is a source-bearing parent, not an unknown lookup scope.
  // POSITIVE
  struct alignas(alignof(retained::Outer::Inner)) Aligned
  {};
  static_assert(alignof(Aligned) == alignof(ExpectedInner));
}

namespace retained::sourceScopes::selfDefaults
{
  // POSITIVE
  template<typename Outer = retained::Outer::Inner>
  using Selected = Outer;
  static_assert(__is_same(Selected<>, ExpectedInner));

  // POSITIVE
  template<typename Outer = retained::Outer::Inner>
  constexpr bool kSelected = __is_same(Outer, ExpectedInner);
  static_assert(kSelected<>);

  // POSITIVE
  template<int Outer = sizeof(retained::Outer::Inner)>
  constexpr int kSize = Outer;
  static_assert(kSize<> == sizeof(ExpectedInner));

  // POSITIVE
  template<template<typename> typename Outer = retained::Outer::Nested>
  using SelectedTemplate = Outer<ExpectedInner>;
  static_assert(__is_same(SelectedTemplate<>, ExpectedInner));
}

namespace retained::sourceScopes::classAndFunctionDefaults
{
  // POSITIVE
  template<typename Outer = retained::Outer::Inner>
  struct Host
  {
    using Type = Outer;
  };
  static_assert(__is_same(Host<>::Type, ExpectedInner));

  // POSITIVE
  template<typename Outer = retained::Outer::Inner>
  constexpr bool hasExpectedType()
  {
    return __is_same(Outer, ExpectedInner);
  }
  static_assert(hasExpectedType());

  // POSITIVE
  template<typename Selected = retained::Outer::Inner, typename Outer = Foreign>
  struct LateHost
  {
    using Type = Selected;
  };
  static_assert(__is_same(LateHost<>::Type, ExpectedInner));

  // POSITIVE
  template<typename Selected = retained::Outer::Inner, typename Outer = Foreign>
  constexpr bool hasLateExpectedType()
  {
    return __is_same(Selected, ExpectedInner);
  }
  static_assert(hasLateExpectedType());

  // POSITIVE
  template<int Outer = sizeof(retained::Outer::Inner)>
  struct SizedHost
  {
    static constexpr int kSize = Outer;
  };
  static_assert(SizedHost<>::kSize == sizeof(ExpectedInner));

  // POSITIVE
  template<template<typename> typename Outer = retained::Outer::Nested>
  constexpr bool hasExpectedTemplate()
  {
    return __is_same(Outer<ExpectedInner>, ExpectedInner);
  }
  static_assert(hasExpectedTemplate());
}

namespace retained::sourceScopes::outOfLineMemberTemplate
{
  template<typename T>
  struct Host
  {
    template<typename U>
    // POSITIVE
      requires requires(retained::Outer::Inner i) { i.value; }
    static constexpr int member();
  };

  template<typename Outer>
  template<typename U>
  // NEGATIVE - the definition's renamed outer list is not on FunctionTemplateDecl.
    requires requires(retained::Outer::Inner i) { i.value; }
  constexpr int Host<Outer>::member()
  {
    return 11;
  }
  static_assert(Host<Foreign>::member<void>() == 11);
}

namespace retained::sourceScopes::lambdaOwnDefault
{
  template<typename UnrelatedParameter>
  // POSITIVE
  constexpr auto kValue = []<typename Outer = retained::Outer::Inner>() constexpr { return Outer{}; };
  static_assert(__is_same(decltype(kValue<Foreign>()), ExpectedInner));
}

namespace retained::sourceScopes::templateTemplateShadow
{
  template<typename T>
  struct ForeignTemplate
  {};

  // NEGATIVE - an earlier template-template parameter still shadows the surviving name.
  template<template<typename> typename Outer, typename Selected = retained::Outer::Inner>
  using SelectedType = Selected;
  static_assert(__is_same(SelectedType<ForeignTemplate>, ExpectedInner));
}
