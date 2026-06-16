// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Lexical.h"

#include <lexy/callback/noop.hpp>
#include <lexy/grammar.hpp>

namespace ao::query::detail
{
  // Tokenizing wrapper for the completion lexer: reuses a parser production's rule but discards its
  // value, so lexy::dsl::capture can extract the matched lexeme without constructing (or requiring a
  // valid value for) the production's AST node. One source of truth for the rule; the value differs.
  template<typename Production>
  struct AsToken : lexy::token_production
  {
    static constexpr auto rule = Production::rule;
    static constexpr auto value = lexy::noop;
  };

  // Operator-token groupings used only by the completion tokenizer. They are kept separate from
  // Lexical.h so that the shared lexical layer does not depend on tokenizer implementation details.
  struct RelationalOperatorToken : lexy::token_production
  {
    static constexpr auto rule = oplit::kNotEqual | oplit::kLessEqual | oplit::kGreaterEqual | oplit::kIn |
                                 oplit::kEqual | oplit::kLike | oplit::kLess | oplit::kGreater;
  };

  struct LogicalOperatorToken : lexy::token_production
  {
    static constexpr auto rule = oplit::kAndWord | oplit::kOrWord | oplit::kAndSymbol | oplit::kOrSymbol;
  };

  struct PrefixOperatorToken : lexy::token_production
  {
    static constexpr auto rule = oplit::kNotWord | oplit::kNotSymbol;
  };

  struct PostfixOperatorToken : lexy::token_production
  {
    static constexpr auto rule = oplit::kExists;
  };

  struct AddOperatorToken : lexy::token_production
  {
    static constexpr auto rule = oplit::kAdd;
  };
} // namespace ao::query::detail
