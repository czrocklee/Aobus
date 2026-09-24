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
      std::size_t begin = 0;
      std::size_t end = 0;
      std::string_view text;
    };

    void checkTokens(std::string_view text, std::vector<ExpectedToken> const& expected)
    {
      auto const tokens = tokenizeCompletionQuery(text);

      REQUIRE(tokens.size() == expected.size());

      for (std::size_t index = 0; index < expected.size(); ++index)
      {
        DYNAMIC_SECTION("Token " << index << " in " << std::string{text})
        {
          CHECK(tokens[index].kind == expected[index].kind);
          CHECK(tokens[index].begin == expected[index].begin);
          CHECK(tokens[index].end == expected[index].end);
          CHECK(tokenText(text, tokens[index]) == expected[index].text);
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
                  {CompletionTokenKind::Variable, 0, 7, "$artist"},
                  {CompletionTokenKind::Whitespace, 7, 8, " "},
                  {CompletionTokenKind::RelationalOperator, 8, 10, "in"},
                  {CompletionTokenKind::Whitespace, 10, 11, " "},
                  {CompletionTokenKind::OpenList, 11, 12, "["},
                  {CompletionTokenKind::StringLiteral, 12, 19, R"("Miles")"},
                  {CompletionTokenKind::Comma, 19, 20, ","},
                  {CompletionTokenKind::Whitespace, 20, 21, " "},
                  {CompletionTokenKind::StringLiteral, 21, 27, R"("Monk")"},
                  {CompletionTokenKind::CloseList, 27, 28, "]"},
                  {CompletionTokenKind::Whitespace, 28, 29, " "},
                  {CompletionTokenKind::LogicalOperator, 29, 32, "and"},
                  {CompletionTokenKind::Whitespace, 32, 33, " "},
                  {CompletionTokenKind::Variable, 33, 49, R"(%["Replay Gain"])"},
                  {CompletionTokenKind::PostfixOperator, 49, 50, "?"},
                });
  }

  TEST_CASE("CompletionTokenizer - reports byte offsets for complete Unicode text", "[query][unit][completion]")
  {
    auto const text = std::string{R"($artist = "Björk")"};
    REQUIRE(text.size() == 18);
    REQUIRE(matchesExpressionSyntax(text));

    checkTokens(text,
                {
                  {CompletionTokenKind::Variable, 0, 7, "$artist"},
                  {CompletionTokenKind::Whitespace, 7, 8, " "},
                  {CompletionTokenKind::RelationalOperator, 8, 9, "="},
                  {CompletionTokenKind::Whitespace, 9, 10, " "},
                  {CompletionTokenKind::StringLiteral, 10, 18, R"("Björk")"},
                });
  }

  TEST_CASE("CompletionTokenizer - keeps parser keyword boundaries", "[query][unit][completion]")
  {
    checkTokens("$artistin in9 notation",
                {
                  {CompletionTokenKind::Variable, 0, 9, "$artistin"},
                  {CompletionTokenKind::Whitespace, 9, 10, " "},
                  {CompletionTokenKind::Bareword, 10, 13, "in9"},
                  {CompletionTokenKind::Whitespace, 13, 14, " "},
                  {CompletionTokenKind::Bareword, 14, 22, "notation"},
                });

    checkTokens("inside and_x or9 notation",
                {
                  {CompletionTokenKind::Bareword, 0, 6, "inside"},
                  {CompletionTokenKind::Whitespace, 6, 7, " "},
                  {CompletionTokenKind::Bareword, 7, 12, "and_x"},
                  {CompletionTokenKind::Whitespace, 12, 13, " "},
                  {CompletionTokenKind::Bareword, 13, 16, "or9"},
                  {CompletionTokenKind::Whitespace, 16, 17, " "},
                  {CompletionTokenKind::Bareword, 17, 25, "notation"},
                });
  }

  TEST_CASE("CompletionTokenizer - matches parser keyword casing", "[query][unit][completion]")
  {
    auto const text = std::string{R"(NoT ($artist IN ["Miles"]) AND ($year = TRUE) Or ($year = FaLsE))"};
    REQUIRE(matchesExpressionSyntax(text));

    checkTokens(text,
                {
                  {CompletionTokenKind::PrefixOperator, 0, 3, "NoT"},
                  {CompletionTokenKind::Whitespace, 3, 4, " "},
                  {CompletionTokenKind::OpenGroup, 4, 5, "("},
                  {CompletionTokenKind::Variable, 5, 12, "$artist"},
                  {CompletionTokenKind::Whitespace, 12, 13, " "},
                  {CompletionTokenKind::RelationalOperator, 13, 15, "IN"},
                  {CompletionTokenKind::Whitespace, 15, 16, " "},
                  {CompletionTokenKind::OpenList, 16, 17, "["},
                  {CompletionTokenKind::StringLiteral, 17, 24, R"("Miles")"},
                  {CompletionTokenKind::CloseList, 24, 25, "]"},
                  {CompletionTokenKind::CloseGroup, 25, 26, ")"},
                  {CompletionTokenKind::Whitespace, 26, 27, " "},
                  {CompletionTokenKind::LogicalOperator, 27, 30, "AND"},
                  {CompletionTokenKind::Whitespace, 30, 31, " "},
                  {CompletionTokenKind::OpenGroup, 31, 32, "("},
                  {CompletionTokenKind::Variable, 32, 37, "$year"},
                  {CompletionTokenKind::Whitespace, 37, 38, " "},
                  {CompletionTokenKind::RelationalOperator, 38, 39, "="},
                  {CompletionTokenKind::Whitespace, 39, 40, " "},
                  {CompletionTokenKind::BooleanLiteral, 40, 44, "TRUE"},
                  {CompletionTokenKind::CloseGroup, 44, 45, ")"},
                  {CompletionTokenKind::Whitespace, 45, 46, " "},
                  {CompletionTokenKind::LogicalOperator, 46, 48, "Or"},
                  {CompletionTokenKind::Whitespace, 48, 49, " "},
                  {CompletionTokenKind::OpenGroup, 49, 50, "("},
                  {CompletionTokenKind::Variable, 50, 55, "$year"},
                  {CompletionTokenKind::Whitespace, 55, 56, " "},
                  {CompletionTokenKind::RelationalOperator, 56, 57, "="},
                  {CompletionTokenKind::Whitespace, 57, 58, " "},
                  {CompletionTokenKind::BooleanLiteral, 58, 63, "FaLsE"},
                  {CompletionTokenKind::CloseGroup, 63, 64, ")"},
                });
  }

  TEST_CASE("CompletionTokenizer - classifies incomplete cursor tails", "[query][unit][completion]")
  {
    checkTokens(R"($artist = "Mil)",
                {
                  {CompletionTokenKind::Variable, 0, 7, "$artist"},
                  {CompletionTokenKind::Whitespace, 7, 8, " "},
                  {CompletionTokenKind::RelationalOperator, 8, 9, "="},
                  {CompletionTokenKind::Whitespace, 9, 10, " "},
                  {CompletionTokenKind::PartialTail, 10, 14, R"("Mil)"},
                });

    checkTokens(R"(%["Replay Gain"?)",
                {
                  {CompletionTokenKind::PartialTail, 0, 16, R"(%["Replay Gain"?)"},
                });
  }

  TEST_CASE("CompletionTokenizer - rejects invalid string escapes as partial tails", "[query][unit][completion]")
  {
    // An invalid escape (\x) must not let a later quote close the string.
    checkTokens(R"(%"a\x"=)",
                {
                  {CompletionTokenKind::PartialTail, 0, 7, R"(%"a\x"=)"},
                });

    // Valid escapes inside a quoted user-variable name still produce a complete variable.
    checkTokens(R"(%"quote\"key" = Br)",
                {
                  {CompletionTokenKind::Variable, 0, 13, R"(%"quote\"key")"},
                  {CompletionTokenKind::Whitespace, 13, 14, " "},
                  {CompletionTokenKind::RelationalOperator, 14, 15, "="},
                  {CompletionTokenKind::Whitespace, 15, 16, " "},
                  {CompletionTokenKind::Bareword, 16, 18, "Br"},
                });

    // Valid escapes inside string literals, both double- and single-quoted.
    checkTokens(R"("foo\"bar")",
                {
                  {CompletionTokenKind::StringLiteral, 0, 10, R"("foo\"bar")"},
                });

    checkTokens(R"('foo\'bar')",
                {
                  {CompletionTokenKind::StringLiteral, 0, 10, R"('foo\'bar')"},
                });

    // A dangling backslash at EOF is an unterminated PartialTail.
    checkTokens(R"("foo\)",
                {
                  {CompletionTokenKind::PartialTail, 0, 5, R"("foo\)"},
                });
  }

  TEST_CASE("CompletionTokenizer - accepts every shared quoted escape", "[query][unit][completion]")
  {
    constexpr auto kDoubleQuoted = std::array{
      std::string_view{R"("a\"b")"},
      std::string_view{R"("a\\b")"},
      std::string_view{R"("a\'b")"},
      std::string_view{R"("a\nb")"},
      std::string_view{R"("a\tb")"},
      std::string_view{R"("a\rb")"},
    };
    constexpr auto kSingleQuoted = std::array{
      std::string_view{R"('a\"b')"},
      std::string_view{R"('a\\b')"},
      std::string_view{R"('a\'b')"},
      std::string_view{R"('a\nb')"},
      std::string_view{R"('a\tb')"},
      std::string_view{R"('a\rb')"},
    };
    constexpr auto kQuotedVariables = std::array{
      std::string_view{R"(%"a\"b")"},
      std::string_view{R"(%"a\\b")"},
      std::string_view{R"(%"a\'b")"},
      std::string_view{R"(%"a\nb")"},
      std::string_view{R"(%"a\tb")"},
      std::string_view{R"(%"a\rb")"},
    };

    for (auto const text : kDoubleQuoted)
    {
      DYNAMIC_SECTION("Double-quoted: " << text)
      {
        REQUIRE(text.size() == 6);
        REQUIRE(matchesExpressionSyntax(text));
        checkTokens(text, {{CompletionTokenKind::StringLiteral, 0, 6, text}});
      }
    }

    for (auto const text : kSingleQuoted)
    {
      DYNAMIC_SECTION("Single-quoted: " << text)
      {
        REQUIRE(text.size() == 6);
        REQUIRE(matchesExpressionSyntax(text));
        checkTokens(text, {{CompletionTokenKind::StringLiteral, 0, 6, text}});
      }
    }

    for (auto const text : kQuotedVariables)
    {
      DYNAMIC_SECTION("Quoted variable: " << text)
      {
        REQUIRE(text.size() == 7);
        REQUIRE(matchesExpressionSyntax(text));
        checkTokens(text, {{CompletionTokenKind::Variable, 0, 7, text}});
      }
    }
  }

  TEST_CASE("CompletionTokenizer - tokenizes value completion prefixes", "[query][unit][completion]")
  {
    checkTokens(R"($artist in ["Miles", Mo)",
                {
                  {CompletionTokenKind::Variable, 0, 7, "$artist"},
                  {CompletionTokenKind::Whitespace, 7, 8, " "},
                  {CompletionTokenKind::RelationalOperator, 8, 10, "in"},
                  {CompletionTokenKind::Whitespace, 10, 11, " "},
                  {CompletionTokenKind::OpenList, 11, 12, "["},
                  {CompletionTokenKind::StringLiteral, 12, 19, R"("Miles")"},
                  {CompletionTokenKind::Comma, 19, 20, ","},
                  {CompletionTokenKind::Whitespace, 20, 21, " "},
                  {CompletionTokenKind::Bareword, 21, 23, "Mo"},
                });
  }

  TEST_CASE("CompletionTokenizer - classifies every operator spelling", "[query][unit][completion]")
  {
    checkTokens(
      "!= <= >= ~ < > and or && || not ! + ?",
      {
        {CompletionTokenKind::RelationalOperator, 0, 2, "!="},  {CompletionTokenKind::Whitespace, 2, 3, " "},
        {CompletionTokenKind::RelationalOperator, 3, 5, "<="},  {CompletionTokenKind::Whitespace, 5, 6, " "},
        {CompletionTokenKind::RelationalOperator, 6, 8, ">="},  {CompletionTokenKind::Whitespace, 8, 9, " "},
        {CompletionTokenKind::RelationalOperator, 9, 10, "~"},  {CompletionTokenKind::Whitespace, 10, 11, " "},
        {CompletionTokenKind::RelationalOperator, 11, 12, "<"}, {CompletionTokenKind::Whitespace, 12, 13, " "},
        {CompletionTokenKind::RelationalOperator, 13, 14, ">"}, {CompletionTokenKind::Whitespace, 14, 15, " "},
        {CompletionTokenKind::LogicalOperator, 15, 18, "and"},  {CompletionTokenKind::Whitespace, 18, 19, " "},
        {CompletionTokenKind::LogicalOperator, 19, 21, "or"},   {CompletionTokenKind::Whitespace, 21, 22, " "},
        {CompletionTokenKind::LogicalOperator, 22, 24, "&&"},   {CompletionTokenKind::Whitespace, 24, 25, " "},
        {CompletionTokenKind::LogicalOperator, 25, 27, "||"},   {CompletionTokenKind::Whitespace, 27, 28, " "},
        {CompletionTokenKind::PrefixOperator, 28, 31, "not"},   {CompletionTokenKind::Whitespace, 31, 32, " "},
        {CompletionTokenKind::PrefixOperator, 32, 33, "!"},     {CompletionTokenKind::Whitespace, 33, 34, " "},
        {CompletionTokenKind::AddOperator, 34, 35, "+"},        {CompletionTokenKind::Whitespace, 35, 36, " "},
        {CompletionTokenKind::PostfixOperator, 36, 37, "?"},
      });
  }

  TEST_CASE("CompletionTokenizer - keeps multi-character operators as single tokens", "[query][unit][completion]")
  {
    checkTokens("! =",
                {{CompletionTokenKind::PrefixOperator, 0, 1, "!"},
                 {CompletionTokenKind::Whitespace, 1, 2, " "},
                 {CompletionTokenKind::RelationalOperator, 2, 3, "="}});
    checkTokens("& &",
                {{CompletionTokenKind::Unknown, 0, 1, "&"},
                 {CompletionTokenKind::Whitespace, 1, 2, " "},
                 {CompletionTokenKind::Unknown, 2, 3, "&"}});
    checkTokens("..", {{CompletionTokenKind::RangeDelimiter, 0, 2, ".."}});
    checkTokens(". .",
                {{CompletionTokenKind::Unknown, 0, 1, "."},
                 {CompletionTokenKind::Whitespace, 1, 2, " "},
                 {CompletionTokenKind::Unknown, 2, 3, "."}});
  }

  TEST_CASE("CompletionTokenizer - classifies literal kinds", "[query][unit][completion]")
  {
    checkTokens("true false -42 42 2m30s",
                {
                  {CompletionTokenKind::BooleanLiteral, 0, 4, "true"},
                  {CompletionTokenKind::Whitespace, 4, 5, " "},
                  {CompletionTokenKind::BooleanLiteral, 5, 10, "false"},
                  {CompletionTokenKind::Whitespace, 10, 11, " "},
                  {CompletionTokenKind::IntegerLiteral, 11, 14, "-42"},
                  {CompletionTokenKind::Whitespace, 14, 15, " "},
                  {CompletionTokenKind::IntegerLiteral, 15, 17, "42"},
                  {CompletionTokenKind::Whitespace, 17, 18, " "},
                  {CompletionTokenKind::UnitLiteral, 18, 23, "2m30s"},
                });
  }

  TEST_CASE("CompletionTokenizer - classifies grouping and prefix operators", "[query][unit][completion]")
  {
    checkTokens("not($a=$b)",
                {
                  {CompletionTokenKind::PrefixOperator, 0, 3, "not"},
                  {CompletionTokenKind::OpenGroup, 3, 4, "("},
                  {CompletionTokenKind::Variable, 4, 6, "$a"},
                  {CompletionTokenKind::RelationalOperator, 6, 7, "="},
                  {CompletionTokenKind::Variable, 7, 9, "$b"},
                  {CompletionTokenKind::CloseGroup, 9, 10, ")"},
                });
  }

  TEST_CASE("CompletionTokenizer - keeps unknown tokens separate from partial tails", "[query][unit][completion]")
  {
    checkTokens("$artist &|.",
                {
                  {CompletionTokenKind::Variable, 0, 7, "$artist"},
                  {CompletionTokenKind::Whitespace, 7, 8, " "},
                  {CompletionTokenKind::Unknown, 8, 9, "&"},
                  {CompletionTokenKind::Unknown, 9, 10, "|"},
                  {CompletionTokenKind::Unknown, 10, 11, "."},
                });
  }

  TEST_CASE("CompletionTokenizer - classifies partial tail variations", "[query][unit][completion]")
  {
    checkTokens("$", {{CompletionTokenKind::PartialTail, 0, 1, "$"}});
    checkTokens("@ ", {{CompletionTokenKind::PartialTail, 0, 2, "@ "}});
    checkTokens("# ", {{CompletionTokenKind::PartialTail, 0, 2, "# "}});
    checkTokens("% ", {{CompletionTokenKind::PartialTail, 0, 2, "% "}});
    checkTokens("$1", {{CompletionTokenKind::PartialTail, 0, 2, "$1"}});
    checkTokens("#\"unterminated", {{CompletionTokenKind::PartialTail, 0, 14, "#\"unterminated"}});
    checkTokens("#[", {{CompletionTokenKind::PartialTail, 0, 2, "#["}});
    checkTokens("#[x", {{CompletionTokenKind::PartialTail, 0, 3, "#[x"}});
    checkTokens("#[\"Rock", {{CompletionTokenKind::PartialTail, 0, 7, "#[\"Rock"}});
    checkTokens(R"("foo\")", {{CompletionTokenKind::PartialTail, 0, 6, R"("foo\")"}});
  }

  TEST_CASE("CompletionTokenizer - tokenizes complete bracketed quoted variables at end", "[query][unit][completion]")
  {
    checkTokens(R"(%["Replay Gain"])", {{CompletionTokenKind::Variable, 0, 16, R"(%["Replay Gain"])"}});
  }

  TEST_CASE("CompletionTokenizer - tokenizes empty and whitespace inputs", "[query][unit][completion]")
  {
    checkTokens("", {});
    checkTokens(" \t\n", {{CompletionTokenKind::Whitespace, 0, 3, " \t\n"}});
  }

  TEST_CASE("CompletionTokenizer - keeps token boundaries parser-acceptable", "[query][unit][completion]")
  {
    for (auto const expression : kParserAcceptedExpressions)
    {
      DYNAMIC_SECTION("Expression: " << expression)
      {
        REQUIRE(matchesExpressionSyntax(expression));

        auto const tokens = tokenizeCompletionQuery(expression);

        for (std::size_t index = 0; index < tokens.size(); ++index)
        {
          DYNAMIC_SECTION("Token " << index)
          {
            CHECK_FALSE(isErrorToken(tokens[index].kind));
            CHECK(tokens[index].begin < tokens[index].end);
          }
        }

        auto const significant = significantTokens(tokens);
        CHECK_FALSE(significant.empty());

        for (std::size_t index = 0; index + 1 < significant.size(); ++index)
        {
          auto const left = significant[index];
          auto const right = significant[index + 1];

          if (left.end != right.begin || isTightlyBoundRangeBoundary(left, right))
          {
            continue;
          }

          DYNAMIC_SECTION("Boundary " << index << " at " << left.end)
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

            for (std::size_t index = 0; index + 1 < tokens.size(); ++index)
            {
              CHECK(tokens[index].end == tokens[index + 1].begin);
              CHECK_FALSE(isErrorToken(tokens[index].kind));
            }
          }
        }
      }
    }
  }
} // namespace ao::query::detail::test
