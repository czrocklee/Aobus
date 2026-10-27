// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

// A lambda written in a template's own signature - its default arguments,
// requires-clauses, return types, and exception specifications - is owned by
// that template's scope chain, so the template parameter can hide the
// qualified name even though the lambda body is an ordinary function body.

namespace retained::lambdaOwners
{
  inline constexpr int kValue = 1;

  struct Shadowed
  {
    static constexpr int value = 1;
  };

  struct Other
  {
    static constexpr int value = 7;
  };

  template<int N>
  struct Wrapper
  {
    static constexpr int value = N;
  };

  namespace variableTemplate
  {
    template<int kValue>
    // NEGATIVE - the variable template's parameter owns the initializer lambda.
    constexpr int selected = [] { return lambdaOwners::kValue; }();
    static_assert(selected<7> == 1);
  }

  namespace conceptOwner
  {
    template<int kValue>
    // NEGATIVE - the concept's parameter owns the initializer lambda.
    concept Selected = [] { return lambdaOwners::kValue == 1; }();
    static_assert(Selected<7>);
  }

  namespace aliasTemplate
  {
    template<int kValue>
    // NEGATIVE - the alias template's parameter owns the lambda in its defining type's argument.
    using Selected = Wrapper<[] { return lambdaOwners::kValue; }()>;
    static_assert(Selected<7>::value == 1);
  }

  namespace memberVariableTemplate
  {
    struct Host
    {
      template<int kValue>
      // NEGATIVE - the member variable template's parameter owns the initializer lambda.
      static constexpr int selected = [] { return lambdaOwners::kValue; }();
    };
    static_assert(Host::selected<7> == 1);
  }

  namespace classTemplateDefault
  {
    // NEGATIVE - the class template's parameter owns the lambda in this default argument.
    template<int kValue, int selected = [] { return lambdaOwners::kValue; }()>
    struct Host
    {
      static constexpr int value = selected;
    };
    static_assert(Host<7>::value == 1);
  }

  namespace functionTemplateValueDefault
  {
    // NEGATIVE - the function template's value parameter owns the lambda in this default.
    template<int kValue, int selected = [] { return lambdaOwners::kValue; }()>
    constexpr int select()
    {
      return selected;
    }
    static_assert(select<7>() == 1);
  }

  namespace functionTemplateTypeDefault
  {
    // NEGATIVE - the function template's value parameter owns the lambda in this type default.
    template<int kValue, typename Selected = Wrapper<[] { return lambdaOwners::kValue; }()>>
    constexpr int select()
    {
      return Selected::value;
    }
    static_assert(select<7>() == 1);
  }

  namespace functionArgumentDefault
  {
    template<int kValue>
    // NEGATIVE - the function template's parameter owns the lambda in this default argument.
    constexpr int select(int selected = [] { return lambdaOwners::kValue; }())
    {
      return selected;
    }
    static_assert(select<7>() == 1);
  }

  namespace requiresPrefix
  {
    template<int kValue>
    // NEGATIVE - the prefix requires-clause lambda is owned by the template parameter scope.
      requires([] { return lambdaOwners::kValue == 1; }())
    constexpr int select()
    {
      return 1;
    }
    static_assert(select<7>() == 1);
  }

  namespace requiresTrailing
  {
    template<int kValue>
    constexpr int select()
      // NEGATIVE - the trailing requires-clause lambda is owned by the template parameter scope.
      requires([] { return lambdaOwners::kValue == 1; }())
    {
      return 1;
    }
    static_assert(select<7>() == 1);
  }

  namespace trailingReturn
  {
    template<int kValue>
    // NEGATIVE - the trailing return type's lambda is owned by the template parameter scope.
    auto select() -> Wrapper<[] { return lambdaOwners::kValue; }()>;
    static_assert(decltype(select<7>())::value == 1);
  }

  namespace leadingReturn
  {
    template<int kValue>
    // NEGATIVE - the leading return type's lambda is owned by the template parameter scope.
    Wrapper<[] { return lambdaOwners::kValue; }()> select();
    static_assert(decltype(select<7>())::value == 1);
  }

  namespace exceptionSpecification
  {
    template<int kValue>
    // NEGATIVE - the exception specification's lambda is owned by the template parameter scope.
    void select() noexcept([] { return lambdaOwners::kValue == 1; }());
    static_assert(noexcept(select<7>()));
  }

  namespace typeParameterQualifier
  {
    template<typename Shadowed>
    // NEGATIVE - the type parameter hides the surviving type before the second qualifier.
    constexpr int selected = [] { return lambdaOwners::Shadowed::value; }();
    static_assert(selected<Other> == 1);
  }

  namespace localClassTemplateOwner
  {
    template<int kValue>
    constexpr int selected = []
    {
      struct Host
      {
        constexpr int select() const
        {
          // NEGATIVE - the ordinary method's outer lambda still needs the variable-template owner.
          return lambdaOwners::kValue;
        }
      };
      return Host{}.select();
    }();
    static_assert(selected<7> == 1);
  }

  namespace unshadowedLocalClassTemplateOwner
  {
    template<int unrelated>
    constexpr int selected = []
    {
      struct Host
      {
        constexpr int select() const
        {
          // POSITIVE - neither the local class nor the outer template hides the namespace constant.
          return lambdaOwners::kValue;
        }
      };
      return Host{}.select();
    }();
    static_assert(selected<7> == 1);
  }

  namespace unshadowedTemplateOwner
  {
    template<int unrelated>
    // POSITIVE - the unrelated template parameter does not hide the namespace constant.
    constexpr int selected = [] { return lambdaOwners::kValue; }();
    static_assert(selected<7> == 1);
  }
}

namespace retained::lambdaOwners::ordinarySignatures
{
  // NEGATIVE - the ordinary parameter owns the lambda in this default argument.
  constexpr int selectWithDefault(int kValue, int selected = [] { return lambdaOwners::kValue; }())
  {
    return selected;
  }
  static_assert(selectWithDefault(2) == 1);

  // NEGATIVE - the ordinary parameter owns the lambda in this trailing return type.
  auto selectTrailing(char kValue) -> decltype([] { return lambdaOwners::kValue; }());
  static_assert(__is_same(decltype(&selectTrailing), int (*)(char)));

  // NEGATIVE - the ordinary parameter owns the lambda in this exception specification.
  void selectNoexcept(char kValue) noexcept([] { return lambdaOwners::kValue == 1; }());
  static_assert(noexcept(selectNoexcept('a')));

  // POSITIVE - the unrelated parameter does not hide the namespace constant.
  constexpr int selectUnshadowed(int unrelated, int selected = [] { return lambdaOwners::kValue; }())
  {
    return selected;
  }
  static_assert(selectUnshadowed(2) == 1);
}

// Lambdas inside ordinary functions keep their own signature owners too: a
// local class member's parameters, an outer lambda's parameters, and a generic
// lambda's template parameters all hide the qualified name.
namespace retained::lambdaOwners::localScopes
{
  namespace classMemberDefault
  {
    constexpr int select()
    {
      struct Host
      {
        // NEGATIVE - the local class member's parameter owns the lambda in this default argument.
        constexpr int select(int kValue, int selected = [] { return lambdaOwners::kValue; }()) { return selected; }
      };
      return Host{}.select(2);
    }
    static_assert(select() == 1);
  }

  namespace classMemberTrailing
  {
    constexpr int select()
    {
      struct Host
      {
        // NEGATIVE - the local class member's parameter owns the lambda in this trailing return type.
        constexpr auto select(char kValue) -> decltype([] { return lambdaOwners::kValue; }()) { return 1; }
      };
      return Host{}.select('a');
    }
    static_assert(select() == 1);
    static_assert(__is_same(decltype(select()), int));
  }

  namespace classMemberNoexcept
  {
    constexpr bool select()
    {
      struct Host
      {
        // NEGATIVE - the local class member's parameter owns the lambda in this exception specification.
        constexpr void select(char kValue) noexcept([] { return lambdaOwners::kValue == 1; }()) {}
      };
      return noexcept(Host{}.select('a'));
    }
    static_assert(select());
  }

  namespace classMemberDefaultSafe
  {
    constexpr int select()
    {
      struct Host
      {
        // POSITIVE - the unrelated local class parameter does not hide the namespace constant.
        constexpr int select(int unrelated, int selected = [] { return lambdaOwners::kValue; }()) { return selected; }
      };
      return Host{}.select(2);
    }
    static_assert(select() == 1);
  }

  namespace classMemberBodyControl
  {
    constexpr int select()
    {
      struct Host
      {
        constexpr int select(int kValue)
        {
          // NEGATIVE - the local class member's body lambda resolves to the parameter without the qualifier.
          return [] { return lambdaOwners::kValue; }();
        }
      };
      return Host{}.select(7);
    }
    static_assert(select() == 1);
  }

  namespace lambdaParameterDefault
  {
    constexpr int select()
    {
      // NEGATIVE - the ordinary lambda's parameter owns the nested lambda in this default argument.
      auto invoke = [](int kValue, int selected = [] { return lambdaOwners::kValue; }()) { return selected; };
      return invoke(2);
    }
    static_assert(select() == 1);
  }

  namespace genericLambdaTemplateDefault
  {
    constexpr int select()
    {
      // NEGATIVE - the generic lambda's template parameter owns the lambda in this default argument.
      return []<int kValue, int selected = [] { return lambdaOwners::kValue; }()>()
      { return selected; }.operator()<7>();
    }
    static_assert(select() == 1);
  }
}

namespace retained::lambdaOwners::templateBodyControl
{
  template<int kValue>
  constexpr int select()
  {
    // The lambda's body is an ordinary function body, but the surrounding
    // template parameter still owns the unqualified spelling.
    // NEGATIVE - a body lambda resolves to the template parameter without the qualifier.
    return [] { return lambdaOwners::kValue; }();
  }
  static_assert(select<7>() == 1);
}
