// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/query/detail/CompletionTokenizer.h"

#include <ao/query/Parser.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ao::query::detail::test
{
  namespace
  {
    struct ExpectedToken final
    {
      CompletionTokenKind kind = CompletionTokenKind::Unknown;
      std::string_view text;
    };

    void checkTokens(std::string_view text, std::vector<ExpectedToken> const& expected)
    {
      auto const tokens = tokenizeCompletionQuery(text);

      REQUIRE(tokens.size() == expected.size());

      for (std::size_t idx = 0; idx < expected.size(); ++idx)
      {
        DYNAMIC_SECTION("Token " << idx << " in " << std::string{text})
        {
          CHECK(tokens[idx].kind == expected[idx].kind);
          CHECK(tokenText(text, tokens[idx]) == expected[idx].text);
        }
      }
    }

    bool isErrorToken(CompletionTokenKind kind)
    {
      return kind == CompletionTokenKind::Unknown || kind == CompletionTokenKind::PartialTail;
    }

    std::vector<CompletionToken> significantTokens(std::vector<CompletionToken> const& tokens)
    {
      auto result = std::vector<CompletionToken>{};

      for (auto const token : tokens)
      {
        if (token.kind != CompletionTokenKind::Whitespace)
        {
          result.push_back(token);
        }
      }

      return result;
    }

    std::string withInsertedSpace(std::string_view text, std::size_t insertOffset)
    {
      auto result = std::string{text};
      result.insert(insertOffset, 1, ' ');
      return result;
    }

    bool isTightlyBoundRangeBoundary(CompletionToken left, CompletionToken right)
    {
      return left.kind == CompletionTokenKind::RangeDelimiter || right.kind == CompletionTokenKind::RangeDelimiter;
    }

    // A corpus of parser-accepted expressions spanning every lexeme kind, sigil, operator spelling, and
    // the quoted/bracketed user-variable forms. Used as a differential oracle for the tokenizer.
    constexpr auto kParserAcceptedExpressions = std::array{
      std::string_view{R"($artist="Miles")"},
      std::string_view{R"($artist!="Miles")"},
      std::string_view{"$title~Love"},
      std::string_view{"$year<=2000"},
      std::string_view{"$year<2000"},
      std::string_view{"$year>1999"},
      std::string_view{"@duration>=2m30s"},
      std::string_view{R"($artist in ["Miles","Monk"])"},
      std::string_view{"$year in 1990..1999"},
      std::string_view{"@duration in 2m30s..5m"},
      std::string_view{"$year in 2m..5m"},
      std::string_view{"$artist?"},
      std::string_view{R"(%["Replay Gain"]?)"},
      std::string_view{"not$artist?"},
      std::string_view{"!#favorite"},
      std::string_view{R"(($artist="Miles")&&($album="Kind of Blue"))"},
      std::string_view{R"(($artist="Miles")||($album="Kind of Blue"))"},
      std::string_view{R"(#"90s Rock")"},
      std::string_view{R"(#["90s Rock"])"},
      std::string_view{R"(%"Mood"="Blue")"},
      std::string_view{R"(%["Replay Gain"]>0)"},
      std::string_view{R"("title"$artist)"},
      std::string_view{"$trackNumber + 1 = 12"},
      std::string_view{"true"},
      std::string_view{"false"},
      std::string_view{R"(($artist = "Miles") or ($album = "Kind of Blue"))"},
      std::string_view{R"(%["Replay Gain"] = 0)"},
    };
  } // namespace

  TEST_CASE("CompletionTokenizer - tokenizes complete query lexemes", "[query][unit][completion]")
  {
    checkTokens(R"($artist in ["Miles", "Monk"] and %["Replay Gain"]?)",
                {
                  {CompletionTokenKind::Variable, "$artist"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::RelationalOperator, "in"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::OpenList, "["},
                  {CompletionTokenKind::StringLiteral, R"("Miles")"},
                  {CompletionTokenKind::Comma, ","},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::StringLiteral, R"("Monk")"},
                  {CompletionTokenKind::CloseList, "]"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::LogicalOperator, "and"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::Variable, R"(%["Replay Gain"])"},
                  {CompletionTokenKind::PostfixOperator, "?"},
                });
  }

  TEST_CASE("CompletionTokenizer - keeps parser keyword boundaries", "[query][unit][completion]")
  {
    checkTokens("$artistin in9 notation",
                {
                  {CompletionTokenKind::Variable, "$artistin"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::Bareword, "in9"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::Bareword, "notation"},
                });
  }

  TEST_CASE("CompletionTokenizer - classifies incomplete cursor tails", "[query][unit][completion]")
  {
    checkTokens(R"($artist = "Mil)",
                {
                  {CompletionTokenKind::Variable, "$artist"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::RelationalOperator, "="},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::PartialTail, R"("Mil)"},
                });

    checkTokens(R"(%["Replay Gain"?)",
                {
                  {CompletionTokenKind::PartialTail, R"(%["Replay Gain"?)"},
                });
  }

  TEST_CASE("CompletionTokenizer - rejects invalid string escapes as partial tails", "[query][unit][completion]")
  {
    // An invalid escape (\x) must not let a later quote close the string.
    checkTokens(R"(%"a\x"=)",
                {
                  {CompletionTokenKind::PartialTail, R"(%"a\x"=)"},
                });

    // Valid escapes inside a quoted user-variable name still produce a complete variable.
    checkTokens(R"(%"quote\"key" = Br)",
                {
                  {CompletionTokenKind::Variable, R"(%"quote\"key")"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::RelationalOperator, "="},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::Bareword, "Br"},
                });

    // Valid escapes inside string literals, both double- and single-quoted.
    checkTokens(R"("foo\"bar")",
                {
                  {CompletionTokenKind::StringLiteral, R"("foo\"bar")"},
                });

    checkTokens(R"('foo\'bar')",
                {
                  {CompletionTokenKind::StringLiteral, R"('foo\'bar')"},
                });

    // A dangling backslash at EOF is an unterminated PartialTail.
    checkTokens(R"("foo\)",
                {
                  {CompletionTokenKind::PartialTail, R"("foo\)"},
                });
  }

  TEST_CASE("CompletionTokenizer - tokenizes value completion prefixes", "[query][unit][completion]")
  {
    checkTokens(R"($artist in ["Miles", Mo)",
                {
                  {CompletionTokenKind::Variable, "$artist"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::RelationalOperator, "in"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::OpenList, "["},
                  {CompletionTokenKind::StringLiteral, R"("Miles")"},
                  {CompletionTokenKind::Comma, ","},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::Bareword, "Mo"},
                });
  }

  TEST_CASE("CompletionTokenizer - classifies every operator spelling", "[query][unit][completion]")
  {
    checkTokens("!= <= >= ~ < > and or && || not ! + ?",
                {
                  {CompletionTokenKind::RelationalOperator, "!="}, {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::RelationalOperator, "<="}, {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::RelationalOperator, ">="}, {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::RelationalOperator, "~"},  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::RelationalOperator, "<"},  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::RelationalOperator, ">"},  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::LogicalOperator, "and"},   {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::LogicalOperator, "or"},    {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::LogicalOperator, "&&"},    {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::LogicalOperator, "||"},    {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::PrefixOperator, "not"},    {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::PrefixOperator, "!"},      {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::AddOperator, "+"},         {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::PostfixOperator, "?"},
                });
  }

  TEST_CASE("CompletionTokenizer - keeps multi-character operators as single tokens", "[query][unit][completion]")
  {
    checkTokens("! =",
                {{CompletionTokenKind::PrefixOperator, "!"},
                 {CompletionTokenKind::Whitespace, " "},
                 {CompletionTokenKind::RelationalOperator, "="}});
    checkTokens("& &",
                {{CompletionTokenKind::Unknown, "&"},
                 {CompletionTokenKind::Whitespace, " "},
                 {CompletionTokenKind::Unknown, "&"}});
    checkTokens("..", {{CompletionTokenKind::RangeDelimiter, ".."}});
    checkTokens(". .",
                {{CompletionTokenKind::Unknown, "."},
                 {CompletionTokenKind::Whitespace, " "},
                 {CompletionTokenKind::Unknown, "."}});
  }

  TEST_CASE("CompletionTokenizer - classifies literal kinds", "[query][unit][completion]")
  {
    checkTokens("true false -42 42 2m30s",
                {
                  {CompletionTokenKind::BooleanLiteral, "true"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::BooleanLiteral, "false"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::IntegerLiteral, "-42"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::IntegerLiteral, "42"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::UnitLiteral, "2m30s"},
                });
  }

  TEST_CASE("CompletionTokenizer - classifies grouping and prefix operators", "[query][unit][completion]")
  {
    checkTokens("not($a=$b)",
                {
                  {CompletionTokenKind::PrefixOperator, "not"},
                  {CompletionTokenKind::OpenGroup, "("},
                  {CompletionTokenKind::Variable, "$a"},
                  {CompletionTokenKind::RelationalOperator, "="},
                  {CompletionTokenKind::Variable, "$b"},
                  {CompletionTokenKind::CloseGroup, ")"},
                });
  }

  TEST_CASE("CompletionTokenizer - keeps unknown tokens separate from partial tails", "[query][unit][completion]")
  {
    checkTokens("$artist &|.",
                {
                  {CompletionTokenKind::Variable, "$artist"},
                  {CompletionTokenKind::Whitespace, " "},
                  {CompletionTokenKind::Unknown, "&"},
                  {CompletionTokenKind::Unknown, "|"},
                  {CompletionTokenKind::Unknown, "."},
                });
  }

  TEST_CASE("CompletionTokenizer - classifies partial tail variations", "[query][unit][completion]")
  {
    checkTokens("$", {{CompletionTokenKind::PartialTail, "$"}});
    checkTokens("@ ", {{CompletionTokenKind::PartialTail, "@ "}});
    checkTokens("# ", {{CompletionTokenKind::PartialTail, "# "}});
    checkTokens("% ", {{CompletionTokenKind::PartialTail, "% "}});
    checkTokens("$1", {{CompletionTokenKind::PartialTail, "$1"}});
    checkTokens("#\"unterminated", {{CompletionTokenKind::PartialTail, "#\"unterminated"}});
    checkTokens("#[", {{CompletionTokenKind::PartialTail, "#["}});
    checkTokens("#[x", {{CompletionTokenKind::PartialTail, "#[x"}});
    checkTokens("#[\"Rock", {{CompletionTokenKind::PartialTail, "#[\"Rock"}});
    checkTokens(R"("foo\")", {{CompletionTokenKind::PartialTail, R"("foo\")"}});
  }

  TEST_CASE("CompletionTokenizer - tokenizes complete bracketed quoted variables at end", "[query][unit][completion]")
  {
    checkTokens(R"(%["Replay Gain"])", {{CompletionTokenKind::Variable, R"(%["Replay Gain"])"}});
  }

  TEST_CASE("CompletionTokenizer - tokenizes empty and whitespace inputs", "[query][unit][completion]")
  {
    checkTokens("", {});
    checkTokens(" \t\n", {{CompletionTokenKind::Whitespace, " \t\n"}});
  }

  TEST_CASE("CompletionTokenizer - keeps token boundaries parser-acceptable", "[query][unit][completion]")
  {
    for (auto const expression : kParserAcceptedExpressions)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        REQUIRE(matchesExpressionSyntax(expression));

        auto const tokens = tokenizeCompletionQuery(expression);

        for (std::size_t idx = 0; idx < tokens.size(); ++idx)
        {
          DYNAMIC_SECTION("Token " << idx)
          {
            CHECK_FALSE(isErrorToken(tokens[idx].kind));
            CHECK(tokens[idx].begin < tokens[idx].end);
          }
        }

        auto const significant = significantTokens(tokens);
        CHECK_FALSE(significant.empty());

        for (std::size_t idx = 0; idx + 1 < significant.size(); ++idx)
        {
          auto const left = significant[idx];
          auto const right = significant[idx + 1];

          if (left.end != right.begin || isTightlyBoundRangeBoundary(left, right))
          {
            continue;
          }

          DYNAMIC_SECTION("Boundary " << idx << " at " << left.end)
          {
            CHECK(matchesExpressionSyntax(withInsertedSpace(expression, left.end)));
          }
        }
      }
    }
  }

  // Differential oracle for the hand-written partial-tail layer, which has no parser counterpart
  // (the parser cannot lex incomplete input). Every prefix of a parser-accepted expression is, by
  // construction, incomplete-but-lexically-clean up to its final byte: the tokenizer must tile it
  // contiguously and recognize every lexeme before the trailing incomplete tail. Only the last token
  // may be an error kind (Unknown/PartialTail) -- that is the in-progress tail the user is still typing.
  TEST_CASE("CompletionTokenizer - lexes prefixes of valid expressions without interior errors",
            "[query][unit][completion]")
  {
    for (auto const expression : kParserAcceptedExpressions)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        REQUIRE(matchesExpressionSyntax(expression));

        for (std::size_t len = 1; len <= expression.size(); ++len)
        {
          auto const prefix = expression.substr(0, len);
          auto const tokens = tokenizeCompletionQuery(prefix);

          DYNAMIC_SECTION("Prefix length " << len)
          {
            REQUIRE_FALSE(tokens.empty());
            CHECK(tokens.front().begin == 0);
            CHECK(tokens.back().end == len);

            for (std::size_t idx = 0; idx + 1 < tokens.size(); ++idx)
            {
              CHECK(tokens[idx].end == tokens[idx + 1].begin);
              CHECK_FALSE(isErrorToken(tokens[idx].kind));
            }
          }
        }
      }
    }
  }
} // namespace ao::query::detail::test
