// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace header_function_definition_fixture
{
  // NEGATIVE
  inline void emptyBody()
  {
  }

  // NEGATIVE
  inline int singleReturn()
  {
    return 1;
  }

  // NEGATIVE
  inline void singleExpression(int& value)
  {
    value = 1;
  }

  // NEGATIVE
  inline void singleDeclaration()
  {
    [[maybe_unused]] int value = 1;
  }

  struct InitializerOnly
  {
    // NEGATIVE
    explicit InitializerOnly(int value)
      : value{value}
    {
    }

    int value;
  };

  struct DefaultedAndDeleted
  {
    // NEGATIVE
    DefaultedAndDeleted() = default;

    // NEGATIVE
    DefaultedAndDeleted(DefaultedAndDeleted const&) = delete;
  };

  // POSITIVE
  inline int twoStatements()
  {
    int value = 1;
    return value;
  }

  // clang-format off: physical line count must not affect this AST-only check.
  // POSITIVE
  inline int twoStatementsOnOneLine()
  {
    int value = 1; return value;
  }

  // NEGATIVE
  inline int oneExpressionAcrossLines(int value)
  {
    return value
           + 1;
  }
  // clang-format on

  // POSITIVE
  inline int controlStatement(bool condition)
  {
    if (condition)
    {
      return 1;
    }

    return 0;
  }

  // POSITIVE
  inline int loopStatement()
  {
    for (int value = 0; value < 1; ++value)
    {
    }

    return 1;
  }

  // POSITIVE
  inline int tryStatement()
  {
    try
    {
      return 1;
    }
    catch (...)
    {
      return 0;
    }
  }

  // POSITIVE
  inline int nestedLambda()
  {
    return []
    {
      int value = 1;
      return value;
    }();
  }

  // POSITIVE
  inline void nestedLocalDefinition()
  {
    struct [[maybe_unused]] Local
    {
      // POSITIVE
      int value()
      {
        int result = 1;
        return result;
      }
    };
  }

  template<typename T>
  // NEGATIVE
  T functionTemplate(T value)
  {
    T copy = value;
    return copy;
  }

  template<typename T>
  struct DependentContext
  {
    // NEGATIVE
    T member(T value)
    {
      T copy = value;
      return copy;
    }
  };

  // NEGATIVE
  constexpr int compileTimeFunction(int value)
  {
    int next = value + 1;
    return next;
  }

  // NEGATIVE
  consteval int immediateFunction(int value)
  {
    int next = value + 1;
    return next;
  }

  // NEGATIVE
  auto deducedReturn(int value)
  {
    int next = value + 1;
    return next;
  }

  // NEGATIVE
  decltype(auto) deducedDecltypeReturn(int& value)
  {
    int& alias = value;
    return alias;
  }

  template<typename T>
  T specialization(T value)
  {
    return value;
  }

  template<>
  // POSITIVE
  inline int specialization<int>(int value)
  {
    int next = value + 1;
    return next;
  }

  struct OutOfClass
  {
    int method(int value);
  };

  // POSITIVE
  inline int OutOfClass::method(int value)
  {
    int next = value + 1;
    return next;
  }

  inline auto kLambda = []
  {
    // NEGATIVE
    int value = 1;
    return value;
  };
} // namespace header_function_definition_fixture
