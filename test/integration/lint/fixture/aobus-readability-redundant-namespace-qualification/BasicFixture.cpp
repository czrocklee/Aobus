// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <type_traits>

namespace ao
{
  struct Foo
  {};
  void bar();
  namespace library
  {
    struct Track
    {};
  }
}

namespace ao
{
  void test1()
  {
    // POSITIVE
    ao::Foo const f;
    // POSITIVE
    ao::Foo const cf;
    // NEGATIVE
    library::Track const t;
    // POSITIVE
    ao::library::Track const t2;
    // POSITIVE
    ao::bar();
  }
}

namespace ao::library
{
  void test2()
  {
    // POSITIVE
    ao::library::Track const t;
    // POSITIVE
    ao::Foo const f;
  }
}

namespace other
{
  void test3()
  {
    // NEGATIVE
    ao::Foo const f;
    // NEGATIVE
    ao::Foo const cf;
    // NEGATIVE
    ao::library::Track const t;
  }
}

namespace ao::test
{
  struct TempDir
  {};
}

namespace ao::rt::test
{
  using namespace ao;

  void test4()
  {
    // NEGATIVE
    ao::test::TempDir const t;
  }
}

namespace ao::main::test
{
  void test5()
  {
    // NEGATIVE
    ao::test::TempDir const t;
  }
}

namespace ao::rt::other
{
  void test6()
  {
    // NEGATIVE
    ao::test::TempDir const t;
  }
}

namespace ao
{
  class BaseException
  {
  public:
    BaseException(int x) {}
  };
}

namespace ao::sub
{
  class DerivedException : public BaseException
  {
  public:
    // NEGATIVE
    using BaseException::BaseException;
  };
}

namespace ao::qualification
{
  struct Projected
  {};
  struct Plain
  {};

  using ProjectedAlias = Projected;

  void invoke();

  template<typename T>
  struct ProjectedTemplate
  {};
}

namespace ao::qualification::implementation
{
  struct Projected
  {};
  struct ProjectedAlias
  {};
  template<typename T>
  struct ProjectedTemplate
  {};
}

namespace ao::qualification::implementation
{
  struct Window final
  {
    // POSITIVE
    qualification::Plain unshadowed;
    // NEGATIVE - the unqualified name denotes the implementation, not the projected type.
    qualification::Projected projected;
    // NEGATIVE - a typedef's source name also participates in lookup.
    qualification::ProjectedAlias alias;
    // NEGATIVE - the class template is shadowed by a different declaration.
    qualification::ProjectedTemplate<int> specialized;
  };

  void preserveParameterShadow(void (*invoke)())
  {
    // NEGATIVE - function parameters require scope inspection, not DeclContext lookup.
    qualification::invoke();
  }

  void preserveLocalShadow()
  {
    auto invoke = [] {};
    // NEGATIVE - removing the qualifier would call the local lambda instead.
    qualification::invoke();
  }
}

namespace ao::qualification
{
  template<typename T>
  struct Deduced
  {
    T value;
  };
}

namespace ao::qualification::implementation
{
  template<typename T>
  struct Deduced
  {
    T value;
  };

  void preserveDeducedTemplateShadow()
  {
    // NEGATIVE - CTAD must still resolve the source template, not the nearer one.
    qualification::Deduced value{1};
    static_assert(std::is_same_v<decltype(value), ::ao::qualification::Deduced<int>>);
  }
}

static_assert(
  std::is_same_v<decltype(ao::qualification::implementation::Window::projected), ao::qualification::Projected>);
static_assert(
  std::is_same_v<decltype(ao::qualification::implementation::Window::alias), ao::qualification::ProjectedAlias>);
static_assert(std::is_same_v<decltype(ao::qualification::implementation::Window::specialized),
                             ao::qualification::ProjectedTemplate<int>>);

namespace ao::retained
{
  struct Outer
  {
    struct Inner
    {
      static constexpr int kIdentity = 11;
    };

    static constexpr int staticFn() { return 11; }
  };

  using OuterAlias = Outer;

  template<typename T>
  struct OuterTemplate : Outer
  {};
}

namespace ao::retained::unshadowed
{
}

// Reopening a namespace must not prevent safe record-qualified removals.
namespace ao::retained::unshadowed
{
  // POSITIVE
  using SelectedInner = retained::Outer::Inner;
  static_assert(SelectedInner::kIdentity == 11);
  // POSITIVE
  static_assert(retained::Outer::staticFn() == 11);
  // POSITIVE
  using AliasInner = retained::OuterAlias::Inner;
  // POSITIVE
  using TemplateInner = retained::OuterTemplate<int>::Inner;
}

namespace ao::retained::recordShadow
{
  struct Outer
  {
    struct Inner
    {
      static constexpr int kIdentity = 22;
    };

    static constexpr int staticFn() { return 22; }
  };
}

namespace ao::retained::recordShadow
{
  // NEGATIVE - the first surviving qualifier is Outer, not the leaf Inner.
  using SelectedInner = retained::Outer::Inner;
  static_assert(SelectedInner::kIdentity == 11);
  // NEGATIVE - both calls compile, but the closer function returns 22.
  static_assert(retained::Outer::staticFn() == 11);
}

namespace foreignRetained
{
  struct Outer
  {
    struct Inner
    {
      static constexpr int kIdentity = 22;
    };
  };
}

namespace ao::retained::aliasShadow
{
  using Outer = ::foreignRetained::Outer;
  using OuterAlias = ::foreignRetained::Outer;
  // NEGATIVE - an alias can redirect the first surviving qualifier.
  using SelectedInner = retained::Outer::Inner;
  static_assert(SelectedInner::kIdentity == 11);
  // NEGATIVE - preserve the source alias name rather than its canonical record name.
  using AliasInner = retained::OuterAlias::Inner;
  static_assert(AliasInner::kIdentity == 11);
}

namespace ao::retained::templateShadow
{
  template<typename T>
  struct Outer : ::foreignRetained::Outer
  {};
  template<typename T>
  struct OuterTemplate : ::foreignRetained::Outer
  {};
  // NEGATIVE - a closer template cannot replace the qualified record.
  using SelectedInner = retained::Outer::Inner;
  // NEGATIVE - template qualifiers need the same source declaration proof.
  using TemplateInner = retained::OuterTemplate<int>::Inner;
  static_assert(TemplateInner::kIdentity == 11);
}

namespace ao::retained::usingShadow
{
  using ::foreignRetained::Outer;
  // NEGATIVE - a using-shadow is a different lookup result.
  using SelectedInner = retained::Outer::Inner;
  static_assert(SelectedInner::kIdentity == 11);
}

namespace ao::retained::directiveShadow
{
  using namespace ::foreignRetained;
}

namespace ao::retained::directiveShadow
{
  // NEGATIVE - direct DeclContext lookup cannot exclude a prior using-directive.
  using SelectedInner = retained::Outer::Inner;
  static_assert(SelectedInner::kIdentity == 11);
}

namespace ao::retained::inlineShadow
{
  inline namespace version
  {
    using Outer = ::foreignRetained::Outer;
  }
}

namespace ao::retained::inlineShadow
{
  // NEGATIVE - implicit inline namespace lookup also crosses namespace reopenings.
  using SelectedInner = retained::Outer::Inner;
  static_assert(SelectedInner::kIdentity == 11);
}

namespace ao::retained::inheritedShadow
{
  struct Base
  {
    using Outer = ::foreignRetained::Outer;
  };

  struct Derived : Base
  {
    // NEGATIVE - inherited qualifier lookup is not direct record lookup.
    using SelectedInner = retained::Outer::Inner;
    static_assert(SelectedInner::kIdentity == 11);
  };
}

namespace ao::retained::localShadow
{
  void preserveLocalQualifier()
  {
    struct Outer : ::foreignRetained::Outer
    {};
    // NEGATIVE - the local record is the first lookup after removing retained::.
    using SelectedInner = retained::Outer::Inner;
    static_assert(SelectedInner::kIdentity == 11);
  }

  void preserveParameterQualifier(int Outer)
  {
    // NEGATIVE - do not infer type-only lookup past a same-named parameter.
    using SelectedInner = retained::Outer::Inner;
  }

  template<typename Outer>
  void preserveTemplateParameterQualifier()
  {
    // NEGATIVE - template parameters are not ordinary DeclContext declarations.
    using SelectedInner = retained::Outer::Inner;
  }
}

namespace ao::retained::outerTemplateShadow
{
  template<typename DifferentParameterName>
  struct Host
  {
    static constexpr int run();
    static constexpr int unshadowed();
    static int const kValue;
    struct Nested;
  };

  template<typename Outer>
  constexpr int Host<Outer>::run()
  {
    // NEGATIVE - this member's outer template list renames the class parameter.
    using SelectedInner = retained::Outer::Inner;
    static_assert(SelectedInner::kIdentity == 11);
    // NEGATIVE - the definition's template parameter would redirect the static call.
    return retained::Outer::staticFn();
  }

  template<typename Outer>
  struct Host<Outer>::Nested
  {
    // NEGATIVE - TagDecl also carries an outer template parameter list.
    using SelectedInner = retained::Outer::Inner;
    static_assert(SelectedInner::kIdentity == 11);
  };

  template<typename IrrelevantParameter>
  constexpr int Host<IrrelevantParameter>::unshadowed()
  {
    // POSITIVE
    using SelectedInner = retained::Outer::Inner;
    static_assert(SelectedInner::kIdentity == 11);
    // POSITIVE
    return retained::Outer::staticFn();
  }

  template<typename Outer>
  // NEGATIVE - a static member's VarDecl carries the outer list, not its context.
  int const Host<Outer>::kValue = retained::Outer::staticFn();

  static_assert(Host<::ao::retained::recordShadow::Outer>::kValue == 11);
  static_assert(Host<::ao::retained::recordShadow::Outer>::run() == 11);
  static_assert(Host<::ao::retained::recordShadow::Outer>::Nested::SelectedInner::kIdentity == 11);
  static_assert(Host<::ao::retained::recordShadow::Outer>::unshadowed() == 11);
}

namespace ao::retained::exported
{
  using ::ao::retained::Outer;
}

namespace ao::retained
{
  namespace path = exported;
}

namespace ao::retained::namespaceAliasUnshadowed
{
  // POSITIVE
  using SelectedInner = retained::path::Outer::Inner;
  static_assert(SelectedInner::kIdentity == 11);
}

namespace ao::retained::namespaceAliasShadow
{
  namespace path = ::foreignRetained;
  // NEGATIVE - the retained namespace alias uses its source name, not exported.
  using SelectedInner = retained::path::Outer::Inner;
  static_assert(SelectedInner::kIdentity == 11);
}

namespace foreignRetained::empty
{
}

namespace foreignRetained::irrelevant
{
  struct Unrelated
  {};
}

// These safe-to-shorten forms document conservative AST-only lookup limits.
// They remain unsupported until a separately bounded lookup proof is available.
namespace ao::retained::benignDirectives
{
  namespace empty
  {
    using namespace ::foreignRetained::empty;
    // NEGATIVE - even an empty using-directive currently makes lookup uncertain.
    using SelectedInner = retained::Outer::Inner;
  }

  namespace irrelevant
  {
    using namespace ::foreignRetained::irrelevant;
    // NEGATIVE - unrelated nominated declarations are not recursively searched.
    using SelectedInner = retained::Outer::Inner;
  }
}

namespace ao::retained::benignInlineNamespaces
{
  namespace empty
  {
    inline namespace version
    {
    }
    // NEGATIVE - empty inline namespaces share the conservative lookup boundary.
    using SelectedInner = retained::Outer::Inner;
  }

  namespace irrelevant
  {
    inline namespace version
    {
      struct Unrelated
      {};
    }
    // NEGATIVE - unrelated inline namespace declarations are not searched.
    using SelectedInner = retained::Outer::Inner;
  }
}

namespace ao::retained::benignBases
{
  struct EmptyBase
  {};
  struct IrrelevantBase
  {
    struct Unrelated
    {};
  };

  struct EmptyDerived : EmptyBase
  {
    // NEGATIVE - even an empty base currently makes inherited lookup uncertain.
    using SelectedInner = retained::Outer::Inner;
  };

  struct IrrelevantDerived : IrrelevantBase
  {
    // NEGATIVE - unrelated base declarations are not recursively searched.
    using SelectedInner = retained::Outer::Inner;
  };
}

namespace ao::importProvider
{
  struct Credit
  {
    static constexpr int kIdentity = 11;
  };
  using CreditAlias = Credit;
  template<typename T>
  struct Box
  {
    using value_type = T;
  };
  template<typename T>
  using DependentAlias = typename T::value_type;
  enum class Rating
  {
    Value
  };
}

namespace ao::imported
{
  using importProvider::Box;
  using importProvider::Credit;
  using importProvider::CreditAlias;
  using importProvider::DependentAlias;
  using importProvider::Rating;
}

#define IMPORTED_TYPE imported::Credit
#define IMPORTED_NAMESPACE imported

namespace ao::imported::unshadowed
{
  // POSITIVE
  imported::Credit record;
  // POSITIVE
  imported::CreditAlias alias;
  // POSITIVE
  imported::Rating enumeration;
  // POSITIVE
  imported::Box<int> specialized;
  // POSITIVE
  imported::DependentAlias<Box<int>> dependentAlias;

  template<typename T>
  struct Generic
  {
    // POSITIVE
    imported::DependentAlias<T> dependent;
    // POSITIVE
    typename imported::Box<T>::value_type dependentMember;
  };

  // NEGATIVE - a macro owns the entire source qualifier.
  IMPORTED_TYPE macroWhole;
  // NEGATIVE - the namespace token belongs to a macro expansion.
  IMPORTED_NAMESPACE::Credit macroPrefix;
}

namespace ao::imported::shadowed
{
  struct Credit
  {};
  struct CreditAlias
  {};
  enum class Rating
  {
    Value
  };
  // NEGATIVE - an imported record still uses its source using-shadow name.
  imported::Credit record;
  // NEGATIVE - desugaring CreditAlias would incorrectly check Credit instead.
  imported::CreditAlias alias;
  // NEGATIVE - imported enum names participate in the same lookup.
  imported::Rating enumeration;
}

namespace ao::imported::aliasOnlyShadow
{
  struct CreditAlias
  {};
  // NEGATIVE - no closer Credit exists; only the source alias is shadowed.
  imported::CreditAlias alias;
}

namespace ao::imported::outerTemplateShadow
{
  template<typename DifferentParameterName>
  struct Host
  {
    static constexpr int run();
  };

  template<typename Credit>
  constexpr int Host<Credit>::run()
  {
    // NEGATIVE - UsingType lookup must also inspect the outer template list.
    imported::Credit value;
    return decltype(value)::kIdentity;
  }

  struct Foreign
  {
    static constexpr int kIdentity = 22;
  };

  static_assert(Host<Foreign>::run() == 11);
}

namespace ao::retained::constraints
{
  struct Foreign
  {
    using Inner = int;
    static constexpr int staticFn() { return 22; }
  };

  template<typename Outer>
  concept PreservesConcept = requires {
    // NEGATIVE - the concept's TemplateDecl owns the shadowing parameter.
    typename retained::Outer::Inner;
    // NEGATIVE - a nested requirement must retain the projected type identity.
    requires std::is_class_v<typename retained::Outer::Inner>;
  };

  template<typename Outer>
  // NEGATIVE - a leading constraint belongs to FunctionTemplateDecl, not FunctionDecl.
    requires(retained::Outer::staticFn() == 11)
  constexpr int leading()
  {
    return 11;
  }

  template<typename Outer>
  constexpr int trailing()
    // NEGATIVE - trailing constraints also retain their template parameter scope.
    requires(retained::Outer::staticFn() == 11)
  {
    return 11;
  }

  template<typename Outer>
  concept PreservesLocalRequirement = requires(Outer value) {
    // NEGATIVE - a requires-expression body must retain the owning concept's scope.
    requires(retained::Outer::staticFn() == 11);
  };

  static_assert(PreservesConcept<Foreign>);
  static_assert(PreservesLocalRequirement<Foreign>);
  static_assert(leading<Foreign>() == 11);
  static_assert(trailing<Foreign>() == 11);

  template<typename UnrelatedParameter>
  concept UnshadowedConcept = requires {
    // POSITIVE
    typename retained::Outer::Inner;
    // POSITIVE
    requires std::is_class_v<typename retained::Outer::Inner>;
  };

  template<typename UnrelatedParameter>
  // POSITIVE
    requires(retained::Outer::staticFn() == 11)
  constexpr int unshadowedLeading()
  {
    return 11;
  }

  template<typename UnrelatedParameter>
  constexpr int unshadowedTrailing()
    // POSITIVE
    requires(retained::Outer::staticFn() == 11)
  {
    return 11;
  }

  static_assert(UnshadowedConcept<Foreign>);
  static_assert(unshadowedLeading<Foreign>() == 11);
  static_assert(unshadowedTrailing<Foreign>() == 11);
}

namespace ao::tui
{
  void complete(int value);

  namespace test
  {
    namespace
    {
      void complete(double value)
      {
        // NEGATIVE
        ao::tui::complete(1);
      }
    }
  }
}
