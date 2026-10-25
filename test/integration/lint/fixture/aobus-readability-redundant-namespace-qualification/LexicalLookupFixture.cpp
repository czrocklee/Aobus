// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

namespace retained::lexicalLookup
{
  inline constexpr int kValue = 1;

  template<typename T>
  struct Wrapper
  {};

  struct Outer
  {
    using Type = int;
  };
}

namespace retained::lexicalLookup::friendScopes
{
  namespace memberBefore
  {
    struct Host
    {
      static constexpr int kValue = 2;
      friend constexpr int select(Host)
      {
        // NEGATIVE - the class member replaces the namespace constant.
        return lexicalLookup::kValue;
      }
    };
    static_assert(select(Host{}) == 1);
  }

  namespace memberAfter
  {
    struct Host
    {
      friend constexpr int select(Host)
      {
        // NEGATIVE - a function body is a complete-class context.
        return lexicalLookup::kValue;
      }
      static constexpr int kValue = 2;
    };
    static_assert(select(Host{}) == 1);
  }

  namespace classTemplate
  {
    template<typename T>
    struct Host
    {
      static constexpr int kValue = 2;
      friend constexpr int select(Host)
      {
        // NEGATIVE - a class-template member participates in the same lookup.
        return lexicalLookup::kValue;
      }
    };
    static_assert(select(Host<void>{}) == 1);
  }

  namespace unrelatedMember
  {
    struct Host
    {
      static constexpr int kUnrelated = 2;
      friend constexpr int select(Host)
      {
        // POSITIVE
        return lexicalLookup::kValue;
      }
    };
    static_assert(select(Host{}) == 1);
  }

  namespace outsideDefinition
  {
    struct Host
    {
      static constexpr int kValue = 2;
      friend constexpr int select(Host);
    };

    constexpr int select(Host)
    {
      // POSITIVE - an outside definition is not in the befriending class scope.
      return lexicalLookup::kValue;
    }
    static_assert(select(Host{}) == 1);
  }

  namespace trailingReturnBefore
  {
    struct Host
    {
      // The unqualified spelling would search from the written point and find
      // the namespace constant while the complete-class search finds the later
      // member; that differing lookup would be ill-formed, no diagnostic
      // required ([class.member.lookup]/6).
      // NEGATIVE - keep the qualifier for complete-class lookup consistency.
      friend auto select(Host) -> Wrapper<decltype(lexicalLookup::kValue)>;
      static constexpr char kValue = 2;
    };
    static_assert(__is_same(decltype(select(Host{})), Wrapper<int const>));
  }

  namespace trailingReturnAfter
  {
    struct Host
    {
      static constexpr char kValue = 2;
      // NEGATIVE - the earlier member changes the trailing return type.
      friend auto select(Host) -> Wrapper<decltype(lexicalLookup::kValue)>;
    };
    static_assert(__is_same(decltype(select(Host{})), Wrapper<int const>));
  }

  namespace defaultArgument
  {
    struct Host
    {
      friend constexpr int select(Host,
                                  int selected =
                                    // NEGATIVE - a default argument sees the complete class.
                                  lexicalLookup::kValue)
      {
        return selected;
      }
      static constexpr int kValue = 2;
    };
    static_assert(select(Host{}) == 1);
  }

  namespace nestedClass
  {
    struct OuterHost
    {
      static constexpr int kValue = 2;
      struct InnerHost
      {
        friend constexpr int select(InnerHost)
        {
          // NEGATIVE - a nested-class function body also completes its enclosing class.
          return lexicalLookup::kValue;
        }
      };
    };
    static_assert(select(OuterHost::InnerHost{}) == 1);
  }

  namespace survivingValue
  {
    struct Host
    {
      static constexpr int Outer = 2;
      // POSITIVE - a value does not hide the type before the surviving ::.
      friend auto select(Host) -> lexicalLookup::Outer::Type;
    };
    static_assert(__is_same(decltype(select(Host{})), int));
  }

  namespace survivingType
  {
    struct Host
    {
      struct Outer
      {
        using Type = char;
      };
      // NEGATIVE - the closer type redirects the surviving qualifier.
      friend auto select(Host) -> lexicalLookup::Outer::Type;
    };
    static_assert(__is_same(decltype(select(Host{})), int));
  }

  namespace exceptionSpecification
  {
    struct Host
    {
      // NEGATIVE - an exception specification sees the complete class.
      friend void select(Host) noexcept(lexicalLookup::kValue == 1);
      static constexpr int kValue = 2;
    };
    static_assert(noexcept(select(Host{})));
  }

  namespace templateDefault
  {
    struct Host
    {
      // A default template argument is a complete-class context, so the
      // complete-class search finds the later member; the unqualified spelling
      // would differ from the written-point search and be ill-formed, no
      // diagnostic required ([class.member.lookup]/6).
      // NEGATIVE - the friend template default sees the complete class.
      template<typename T = decltype(lexicalLookup::kValue)>
      friend constexpr bool select(Host)
      {
        return __is_same(T, int const);
      }
      static constexpr char kValue = 2;
    };
    static_assert(select(Host{}));
  }

  namespace parameterShadow
  {
    struct Host
    {
      friend constexpr int select(Host, int kValue)
      {
        // NEGATIVE - the friend function parameter remains the closer value.
        return lexicalLookup::kValue;
      }
    };
    static_assert(select(Host{}, 2) == 1);
  }

  namespace captureShadow
  {
    struct Host
    {
      friend constexpr int select(Host)
      {
        return [kValue = 2]
        {
          // NEGATIVE - lookup-parent traversal must not skip lambda captures.
          return lexicalLookup::kValue;
        }();
      }
    };
    static_assert(select(Host{}) == 1);
  }
}

namespace retained::lexicalLookup::classMemberScopes
{
  namespace templateDefault
  {
    struct Host
    {
      // A member template's default argument is a complete-class context, so
      // the complete-class search finds the later member; the unqualified
      // spelling would differ from the written-point search and be ill-formed,
      // no diagnostic required ([class.member.lookup]/6).
      // NEGATIVE - the member template default sees the complete class.
      template<typename T = decltype(lexicalLookup::kValue)>
      static T select();
      static constexpr char kValue = 2;
    };
    static_assert(__is_same(decltype(&Host::select<>), int const (*)()));
  }

  namespace defaultMemberInitializer
  {
    struct Host
    {
      // NEGATIVE - a default member initializer sees the complete class.
      int selected = lexicalLookup::kValue;
      static constexpr int kValue = 2;
    };
    static_assert(Host{}.selected == 1);
  }

  namespace exceptionSpecification
  {
    struct Host
    {
      // NEGATIVE - a member exception specification sees the complete class.
      void select() noexcept(lexicalLookup::kValue == 1);
      static constexpr int kValue = 2;
    };
    static_assert(noexcept(Host{}.select()));
  }

  namespace staticOwnInitializer
  {
    struct Host
    {
      // NEGATIVE - the member is visible in its own initializer.
      static constexpr int kValue = lexicalLookup::kValue;
    };
    static_assert(Host::kValue == 1);
  }

  namespace staticMemberAfter
  {
    struct Host
    {
      // The point-use search does not necessarily change the initialized value,
      // but it differs from the complete-class search, making the shortened
      // spelling ill-formed, no diagnostic required ([class.member.lookup]/6);
      // GCC's changes-meaning diagnostic rejects it.
      // NEGATIVE - a later static member participates in complete-class lookup.
      static constexpr int selected = lexicalLookup::kValue;
      static constexpr int kValue = 2;
    };
    static_assert(Host::selected == 1);
  }

  namespace memberTypeAfter
  {
    struct Host
    {
      // The member's type would resolve to the later member alias in the
      // complete-class search; GCC's changes-meaning diagnostic rejects the
      // unqualified spelling.
      // NEGATIVE - a later member type participates in complete-class lookup.
      lexicalLookup::Outer selected;
      using Outer = char;
    };
    static_assert(__is_same(decltype(Host{}.selected), Outer));
  }

  namespace arrayBoundAfter
  {
    struct Host
    {
      // The point-use search does not necessarily change the bound, but it
      // differs from the complete-class search, making the shortened spelling
      // ill-formed, no diagnostic required ([class.member.lookup]/6); GCC's
      // changes-meaning diagnostic rejects it.
      // NEGATIVE - a later member bound participates in complete-class lookup.
      int selected[lexicalLookup::kValue];
      static constexpr int kValue = 2;
    };
    static_assert(sizeof(Host{}.selected) == sizeof(int));
  }

  namespace classEnumOwnInitializer
  {
    struct Host
    {
      enum
      {
        // Unlike a block-scope enumerator, this in-class initializer is subject
        // to complete-class lookup consistency; GCC's changes-meaning
        // diagnostic rejects the unqualified spelling.
        // NEGATIVE - a class enumerator joins complete-class lookup.
        kValue = lexicalLookup::kValue
      };
    };
    static_assert(Host::kValue == 1);
  }
}

namespace retained::lexicalLookup::constructorScopes
{
  namespace memberAfter
  {
    struct Host
    {
      // The mem-initializer is evaluated in the complete-class context, so the
      // later member would be found and change the initialized value.
      constexpr Host()
        // NEGATIVE - a later member participates in mem-initializer lookup.
        : selected(lexicalLookup::kValue)
      {
      }
      int selected;
      static constexpr int kValue = 2;
    };
    static_assert(Host{}.selected == 1);
  }

  namespace memberBefore
  {
    struct Host
    {
      static constexpr int kValue = 2;
      int selected;
      // The earlier member is found before the namespace constant.
      constexpr Host()
        // NEGATIVE - an earlier member is the closer lookup candidate.
        : selected(lexicalLookup::kValue)
      {
      }
    };
    static_assert(Host{}.selected == 1);
  }

  namespace outsideDefinition
  {
    struct Host
    {
      constexpr Host();
      int selected;
      static constexpr int kValue = 2;
    };

    // An out-of-line mem-initializer still resolves the class member before
    // the namespace constant.
    constexpr Host::Host()
      // NEGATIVE - an out-of-line mem-initializer still finds the class member.
      : selected(lexicalLookup::kValue)
    {
    }
    static_assert(Host{}.selected == 1);
  }

  namespace unrelatedMember
  {
    struct Host
    {
      constexpr Host()
        // POSITIVE - the unrelated member does not hide the namespace constant.
        : selected(lexicalLookup::kValue)
      {
      }
      int selected;
      static constexpr int kOther = 2;
    };
    static_assert(Host{}.selected == 1);
  }
}

namespace retained::lexicalLookup::enumScopes
{
  constexpr int unnamedEnum()
  {
    enum
    {
      kValue = 2
    };
    // NEGATIVE - an unscoped enumerator is injected into the active block.
    return lexicalLookup::kValue;
  }
  static_assert(unnamedEnum() == 1);

  constexpr int namedEnum()
  {
    enum Choice
    {
      kValue = 2
    };
    // NEGATIVE - a named unscoped enum injects its enumerators too.
    return lexicalLookup::kValue;
  }
  static_assert(namedEnum() == 1);

  constexpr int scopedEnum()
  {
    enum class Choice
    {
      kValue = 2
    };
    // POSITIVE
    return lexicalLookup::kValue;
  }
  static_assert(scopedEnum() == 1);

  constexpr int beforeDeclaration()
  {
    // POSITIVE - the later enumerator is not visible yet.
    int selected = lexicalLookup::kValue;
    enum
    {
      kValue = 2
    };
    return selected;
  }
  static_assert(beforeDeclaration() == 1);

  constexpr int ownInitializer()
  {
    enum
    {
      // POSITIVE - an enumerator is not visible in its own initializer.
      kValue = lexicalLookup::kValue
    };
    return kValue;
  }
  static_assert(ownInitializer() == 1);

  constexpr int earlierInitializer()
  {
    enum
    {
      kValue = 2,
      // NEGATIVE - an earlier enumerator is visible in a later initializer.
      kLaterValue = lexicalLookup::kValue
    };
    return kLaterValue;
  }
  static_assert(earlierInitializer() == 1);

  enum class ScopedOwnInitializer
  {
    // POSITIVE - scoped and unscoped enumerators have the same declaration locus.
    kValue = lexicalLookup::kValue
  };
  static_assert(static_cast<int>(ScopedOwnInitializer::kValue) == 1);

  enum class ScopedEarlierInitializer
  {
    kValue = 2,
    // NEGATIVE - earlier scoped enumerators are visible inside the enum scope.
    kLaterValue = lexicalLookup::kValue
  };
  static_assert(static_cast<int>(ScopedEarlierInitializer::kLaterValue) == 1);

  constexpr int endedBlock()
  {
    {
      enum
      {
        kValue = 2
      };
    }
    // POSITIVE - the enumerator's block has ended.
    return lexicalLookup::kValue;
  }
  static_assert(endedBlock() == 1);

  constexpr int parentBlock()
  {
    enum
    {
      kValue = 2
    };
    {
      // NEGATIVE - the enclosing block remains active.
      return lexicalLookup::kValue;
    }
  }
  static_assert(parentBlock() == 1);

  constexpr int ifInitializer()
  {
    if (enum {kValue = 2}; true)
    {
      // NEGATIVE - an if initializer is active in its substatement.
      return lexicalLookup::kValue;
    }
    return 0;
  }
  static_assert(ifInitializer() == 1);

  constexpr int afterIfInitializer()
  {
    if (enum {kValue = 2}; false)
    {
      return 0;
    }
    // POSITIVE - the if statement's initializer scope has ended.
    return lexicalLookup::kValue;
  }
  static_assert(afterIfInitializer() == 1);

  constexpr int switchInitializer()
  {
    switch (enum {kValue = 2}; 0)
    {
      default:
        // NEGATIVE - a switch initializer is active in the switch body.
        return lexicalLookup::kValue;
    }
  }
  static_assert(switchInitializer() == 1);

  constexpr int afterSwitchInitializer()
  {
    switch (enum {kValue = 2}; 0)
    {
      default: break;
    }
    // POSITIVE - the switch statement's initializer scope has ended.
    return lexicalLookup::kValue;
  }
  static_assert(afterSwitchInitializer() == 1);

  constexpr int forInitializer()
  {
    for (enum {kValue = 2}; true;)
    {
      // NEGATIVE - a for initializer is active in the loop body.
      return lexicalLookup::kValue;
    }
  }
  static_assert(forInitializer() == 1);

  constexpr int afterForInitializer()
  {
    for (enum {kValue = 2}; false;)
    {
    }
    // POSITIVE - the for statement's initializer scope has ended.
    return lexicalLookup::kValue;
  }
  static_assert(afterForInitializer() == 1);

  constexpr int rangeInitializer()
  {
    int values[]{0};
    for (enum {kValue = 2}; int selected : values)
    {
      // NEGATIVE - a range-for initializer is active in the loop body.
      return lexicalLookup::kValue + selected;
    }
    return 0;
  }
  static_assert(rangeInitializer() == 1);

  constexpr int afterRangeInitializer()
  {
    int values[]{0};
    for (enum {kValue = 2}; int selected : values)
    {
      (void)selected;
    }
    // POSITIVE - the range-for initializer scope has ended.
    return lexicalLookup::kValue;
  }
  static_assert(afterRangeInitializer() == 1);

  constexpr int siblingBranch()
  {
    if (true)
    {
      enum
      {
        kValue = 2
      };
    }
    else
    {
      return 0;
    }
    // POSITIVE - a declaration in a completed sibling block is not visible.
    return lexicalLookup::kValue;
  }
  static_assert(siblingBranch() == 1);

  constexpr int enclosingLambda()
  {
    enum
    {
      kValue = 2
    };
    return []
    {
      // NEGATIVE - the enclosing block's enumerator remains visible in a lambda.
      return lexicalLookup::kValue;
    }();
  }
  static_assert(enclosingLambda() == 1);

  constexpr int enclosingLocalClass()
  {
    enum
    {
      kValue = 2
    };
    struct Host
    {
      constexpr int selected() const
      {
        // NEGATIVE - the local class method sees the enclosing function's active enumerator.
        return lexicalLookup::kValue;
      }
    };
    return Host{}.selected();
  }
  static_assert(enclosingLocalClass() == 1);

  constexpr int endedEnumLocalClass()
  {
    {
      enum
      {
        kValue = 2
      };
    }
    struct Host
    {
      constexpr int selected() const
      {
        // POSITIVE - an ended block's enumerator is not visible in the local class method.
        return lexicalLookup::kValue;
      }
    };
    return Host{}.selected();
  }
  static_assert(endedEnumLocalClass() == 1);

  struct ClassEnumerator
  {
    constexpr int selected() const
    {
      // NEGATIVE - a member function body sees later class enumerators.
      return lexicalLookup::kValue;
    }
    enum
    {
      kValue = 2
    };
  };
  static_assert(ClassEnumerator{}.selected() == 1);

  struct ClassBeforeEnumerator
  {
    // The unqualified spelling would search from the written point and find
    // the namespace constant while the complete-class search finds the later
    // enumerator; that differing lookup would be ill-formed, no diagnostic
    // required ([class.member.lookup]/6), and GCC's changes-meaning
    // diagnostic rejects the spelling outright.
    // NEGATIVE - keep the qualifier for complete-class lookup consistency.
    using Selected = decltype(lexicalLookup::kValue);
    enum
    {
      kValue = 2
    };
  };
  static_assert(__is_same(ClassBeforeEnumerator::Selected, int const));

  struct ClassAfterEnumerator
  {
    enum
    {
      kValue = 2
    };
    // NEGATIVE - the earlier class enumerator changes the selected type.
    using Selected = decltype(lexicalLookup::kValue);
  };
  static_assert(__is_same(ClassAfterEnumerator::Selected, int const));

  enum class ImportedChoice
  {
    kValue = 2
  };

  constexpr int importedEnumerator()
  {
    using enum ImportedChoice;
    // NEGATIVE - a using-enum declaration introduces the scoped enumerator.
    return lexicalLookup::kValue;
  }
  static_assert(importedEnumerator() == 1);
}

namespace retained::lexicalLookup::parameterScopes
{
  // NEGATIVE - a parameter is visible in its own default argument.
  void ownDefault(int kValue = lexicalLookup::kValue);

  // NEGATIVE - an earlier parameter is visible in a later default argument.
  void earlierDefault(int kValue, int selected = lexicalLookup::kValue);

  // NEGATIVE - parameters are visible in a trailing return type without a body.
  auto trailingReturn(char kValue) -> Wrapper<decltype(lexicalLookup::kValue)>;
  static_assert(__is_same(decltype(trailingReturn('a')), Wrapper<int const>));

  // NEGATIVE - an earlier parameter is visible in a later parameter type.
  void laterParameterType(char kValue, decltype(lexicalLookup::kValue)* selected);
  static_assert(__is_same(decltype(&laterParameterType), void (*)(char, int const*)));

  // POSITIVE - a later parameter is not visible in an earlier parameter type.
  void beforeParameter(decltype(lexicalLookup::kValue)* selected, char kValue);
  static_assert(__is_same(decltype(&beforeParameter), void (*)(int const*, char)));

  // POSITIVE - a parameter is not visible inside its own complete declarator.
  void ownArrayDeclarator(int (&kValue)[sizeof(lexicalLookup::kValue)]);
  static_assert(__is_same(decltype(&ownArrayDeclarator), void (*)(int (&)[sizeof(int)])));

  // NEGATIVE - a parameter is visible in the function exception specification.
  void exceptionSpecification(char kValue) noexcept(sizeof(lexicalLookup::kValue) == sizeof(int));
  static_assert(noexcept(exceptionSpecification('a')));

  // NEGATIVE - the nested function-type parameter is visible in its own scope.
  using Callback = void(char kValue, decltype(lexicalLookup::kValue)* selected);
  static_assert(__is_same(Callback, void(char, int const*)));

  // NEGATIVE - a callback parameter has its own nested parameter scope.
  void nestedCallback(void (*callback)(char kValue, decltype(lexicalLookup::kValue)* selected));
  static_assert(__is_same(decltype(&nestedCallback), void (*)(void (*)(char, int const*))));

  // POSITIVE - the callback's parameter scope does not leak into the outer parameter list.
  void endedNestedCallback(void (*callback)(char kValue), decltype(lexicalLookup::kValue)* selected);
  static_assert(__is_same(decltype(&endedNestedCallback), void (*)(void (*)(char), int const*)));

  void renamedDeclaration(char kValue, int selected);
  // POSITIVE - parameter names from another declaration do not participate in lookup.
  void renamedDeclaration(char unrelated, int selected = lexicalLookup::kValue);

  // NEGATIVE - unevaluated use can still change the default argument's value.
  constexpr int ownUnevaluatedDefault(char kValue = sizeof(lexicalLookup::kValue))
  {
    return kValue;
  }
  static_assert(ownUnevaluatedDefault() == sizeof(int));

  // POSITIVE - a leading return type precedes the parameter scope.
  Wrapper<decltype(lexicalLookup::kValue)> leadingReturn(char kValue);
  static_assert(__is_same(decltype(leadingReturn('a')), Wrapper<int const>));

  template<typename T>
    requires true
  void trailingRequirement(char kValue)
    // NEGATIVE - parameters are visible in a trailing requires-clause.
    requires(sizeof(lexicalLookup::kValue) == sizeof(int));
  static_assert(requires { trailingRequirement<void>('a'); });

  void survivingQualifier(int Outer)
  {
    // POSITIVE - a value parameter does not hide the type before ::.
    using Selected = lexicalLookup::Outer::Type;
    static_assert(__is_same(Selected, int));
  }

  // NEGATIVE - adding a body does not change default-argument lookup.
  constexpr int definitionDefault(int kValue, int selected = lexicalLookup::kValue)
  {
    return kValue + selected;
  }
  static_assert(definitionDefault(1) == 2);

  // NEGATIVE - adding a body does not change trailing-return lookup.
  auto definitionTrailing(char kValue) -> Wrapper<decltype(lexicalLookup::kValue)>
  {
    return {};
  }
  static_assert(__is_same(decltype(definitionTrailing('a')), Wrapper<int const>));

  // NEGATIVE - adding a body does not change later-parameter lookup.
  constexpr int definitionParameter(char kValue, decltype(lexicalLookup::kValue)* selected)
  {
    return kValue + *selected;
  }
  static_assert(__is_same(decltype(&definitionParameter), int (*)(char, int const*)));
}

namespace retained::lexicalLookup::requiresScopes
{
  constexpr bool shadowedParameter()
  {
    return requires(char kValue) {
      // The parameter's type differs from the namespace constant, so the
      // unqualified spelling would fail this requirement.
      // NEGATIVE - the requires-expression parameter is the closer lookup candidate.
      requires sizeof(lexicalLookup::kValue) == sizeof(int);
    };
  }
  static_assert(shadowedParameter());

  constexpr bool unshadowedParameter()
  {
    return requires(char kOther) {
      // POSITIVE - the unrelated parameter does not hide the namespace constant.
      requires sizeof(lexicalLookup::kValue) == sizeof(int);
    };
  }
  static_assert(unshadowedParameter());
}
