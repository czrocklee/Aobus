// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/Parser.h>
#include <ao/utility/VariantVisitor.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ao::query::test
{
  namespace
  {
    Expression parseOk(std::string_view text)
    {
      auto result = ::ao::query::parse(text);
      REQUIRE(result.has_value());
      return std::move(*result);
    }

    bool parseFails(std::string_view text)
    {
      return !::ao::query::parse(text).has_value();
    }

    char const* toString(Operator op)
    {
      switch (op)
      {
        case Operator::And: return "and";
        case Operator::Or: return "or";
        case Operator::Not: return "not";
        case Operator::Equal: return "eq";
        case Operator::NotEqual: return "ne";
        case Operator::Like: return "like";
        case Operator::Less: return "lt";
        case Operator::LessEqual: return "le";
        case Operator::Greater: return "gt";
        case Operator::GreaterEqual: return "ge";
        case Operator::In: return "in";
        case Operator::Add: return "add";
        case Operator::Exists: return "exists";
        default: return "unknown";
      }
    }

    struct Canonicalizer final
    {
      void operator()(std::unique_ptr<BinaryExpression> const& binary)
      {
        if (!binary)
        {
          return;
        }

        oss << "[b{" << toString(binary->optOperation->op) << "}";
        std::visit(*this, binary->operand);
        oss << ",";
        std::visit(*this, binary->optOperation->operand);
        oss << "]";
      }

      void operator()(std::unique_ptr<UnaryExpression> const& unary)
      {
        if (!unary)
        {
          return;
        }

        oss << "[u{" << toString(unary->op) << "}";
        std::visit(*this, unary->operand);
        oss << "]";
      }

      void operator()(VariableExpression const& variable)
      {
        oss << "[v{";

        switch (variable.type)
        {
          case VariableType::Metadata: oss << 'm'; break;
          case VariableType::Property: oss << 'p'; break;
          case VariableType::Tag: oss << 't'; break;
          case VariableType::Custom: oss << 'c'; break;
        }

        oss << "}" << variable.name << "]";
      }

      void operator()(ConstantExpression const& constant)
      {
        oss << "[c{";
        std::visit(utility::makeVisitor([this](std::monostate) { oss << "n}"; },
                                        [this](bool val) { oss << "b}" << (val ? "true" : "false"); },
                                        [this](std::int64_t val) { oss << "i}" << val; },
                                        [this](UnitConstantExpression const& val) { oss << "u}" << val.lexeme; },
                                        [this](std::string_view val) { oss << "s}" << val; }),
                   constant);
        oss << "]";
      }

      void operator()(ListExpression const& list)
      {
        oss << "[l";

        for (auto const& value : list.values)
        {
          std::visit(*this, Expression{value});
        }

        oss << "]";
      }

      void operator()(RangeExpression const& range)
      {
        oss << "[r";
        std::visit(*this, Expression{range.lower});
        oss << ",";
        std::visit(*this, Expression{range.upper});
        oss << "]";
      }

      std::ostringstream oss;
    };

    std::string canonicalize(Expression const& expr)
    {
      auto canonicalizer = Canonicalizer{};
      std::visit(canonicalizer, expr);
      return canonicalizer.oss.str();
    }
  } // namespace

  TEST_CASE("Parser - String Literal", "[query][unit][parser]")
  {
    CHECK("[c{s}Artist]" == canonicalize(parseOk("Artist")));
    CHECK("[c{s}Artist]" == canonicalize(parseOk("\"Artist\"")));
    CHECK("[c{s}Artist]" == canonicalize(parseOk("'Artist'")));
    CHECK("[c{s}A \"quote\"]" == canonicalize(parseOk(R"("A \"quote\"")")));
    CHECK("[c{s}A 'quote']" == canonicalize(parseOk(R"('A \'quote\'')")));
  }

  TEST_CASE("Parser - String Escape Sequences", "[query][unit][parser]")
  {
    CHECK("[c{s}quote\"and\\slash]" == canonicalize(parseOk(R"("quote\"and\\slash")")));
    CHECK("[c{s}quote\"and\\slash]" == canonicalize(parseOk(R"('quote\"and\\slash')")));
    CHECK("[c{s}line1\nline2]" == canonicalize(parseOk(R"("line1\nline2")")));
    CHECK("[c{s}line1\nline2]" == canonicalize(parseOk(R"('line1\nline2')")));
    CHECK("[c{s}col1\tcol2]" == canonicalize(parseOk(R"("col1\tcol2")")));
    CHECK("[c{s}cr\rlf]" == canonicalize(parseOk(R"("cr\rlf")")));
  }

  TEST_CASE("Parser - System Variable", "[query][unit][parser]")
  {
    CHECK("[v{m}title]" == canonicalize(parseOk("$title")));
    CHECK("[v{m}artist]" == canonicalize(parseOk("$artist")));
    CHECK("[v{m}composer]" == canonicalize(parseOk("$composer")));
    CHECK("[v{m}work]" == canonicalize(parseOk("$work")));
    CHECK("[v{m}w]" == canonicalize(parseOk("$w")));
    CHECK("[v{p}duration]" == canonicalize(parseOk("@duration")));
  }

  TEST_CASE("Parser - User Variable Name", "[query][unit][parser]")
  {
    SECTION("Bare tag and custom names allow numeric starts")
    {
      CHECK("[v{t}Tag]" == canonicalize(parseOk("#Tag")));
      CHECK("[v{t}123]" == canonicalize(parseOk("#123")));
      CHECK("[v{c}isrc]" == canonicalize(parseOk("%isrc")));
      CHECK("[v{c}123]" == canonicalize(parseOk("%123")));
      CHECK("[v{c}replaygaintrackgaindb]" == canonicalize(parseOk("%replaygaintrackgaindb")));
    }

    SECTION("Quoted names support spaces, Unicode, and escapes")
    {
      CHECK("[v{t}90s Rock]" == canonicalize(parseOk(R"(#"90s Rock")")));
      CHECK("[v{t}你说得对]" == canonicalize(parseOk(R"(#"你说得对")")));
      CHECK("[v{c}Replay Gain]" == canonicalize(parseOk(R"(%"Replay Gain")")));
      CHECK("[v{c}quote\"and\\slash]" == canonicalize(parseOk(R"(%"quote\"and\\slash")")));
      CHECK("[v{c}quote'and\nnewline]" == canonicalize(parseOk(R"(%"quote\'and\nnewline")")));
    }

    SECTION("Bracketed quoted accepted as well")
    {
      CHECK("[v{t}90s Rock]" == canonicalize(parseOk(R"(#["90s Rock"])")));
      CHECK("[v{c}Replay Gain]" == canonicalize(parseOk(R"(%["Replay Gain"])")));
    }
  }

  TEST_CASE("Parser - Invalid Variable Name", "[query][unit][parser]")
  {
    CHECK(parseFails(R"(#"")"));
    CHECK(parseFails(R"(%"")"));
    CHECK(parseFails(R"(#[""])"));
    CHECK(parseFails(R"(%[""])"));
    CHECK(parseFails("$123"));
    CHECK(parseFails("@123"));
  }

  TEST_CASE("Parser - Variable Shortcuts", "[query][unit][parser]")
  {
    CHECK("[v{m}t]" == canonicalize(parseOk("$t")));
    CHECK("[v{m}a]" == canonicalize(parseOk("$a")));
    CHECK("[v{m}al]" == canonicalize(parseOk("$al")));
    CHECK("[v{m}g]" == canonicalize(parseOk("$g")));
    CHECK("[v{m}c]" == canonicalize(parseOk("$c")));
    CHECK("[v{m}y]" == canonicalize(parseOk("$y")));
    CHECK("[v{m}tn]" == canonicalize(parseOk("$tn")));
    CHECK("[v{p}l]" == canonicalize(parseOk("@l")));
    CHECK("[v{p}br]" == canonicalize(parseOk("@br")));
    CHECK("[v{p}sr]" == canonicalize(parseOk("@sr")));
    CHECK("[v{p}bd]" == canonicalize(parseOk("@bd")));
  }

  TEST_CASE("Parser - String Cat", "[query][unit][parser]")
  {
    CHECK("[b{add}[c{s}Artist],[c{s}Album]]" == canonicalize(parseOk("Artist + Album")));
    CHECK("[b{add}[c{s}Artist],[v{m}Album]]" == canonicalize(parseOk("Artist + $Album")));
    CHECK("[b{add}[c{s}Artist],[v{m}Album]]" == canonicalize(parseOk("Artist$Album")));
  }

  TEST_CASE("Parser - Equal", "[query][unit][parser]")
  {
    CHECK("[b{eq}[v{m}artist],[c{s}Bach]]" == canonicalize(parseOk("$artist=Bach")));
    CHECK("[b{eq}[v{m}trackNumber],[c{i}12]]" == canonicalize(parseOk("$trackNumber=12")));
    CHECK("[b{eq}[v{m}year],[c{i}2020]]" == canonicalize(parseOk("$year=2020")));
  }

  TEST_CASE("Parser - NotEqual", "[query][unit][parser]")
  {
    CHECK("[b{ne}[v{m}artist],[c{s}Bach]]" == canonicalize(parseOk("$artist!=Bach")));
    CHECK("[b{ne}[v{m}year],[c{i}2020]]" == canonicalize(parseOk("$year!=2020")));
  }

  TEST_CASE("Parser - Relational", "[query][unit][parser]")
  {
    CHECK("[b{lt}[v{m}year],[c{i}2000]]" == canonicalize(parseOk("$year<2000")));
    CHECK("[b{le}[v{m}year],[c{i}2000]]" == canonicalize(parseOk("$year<=2000")));
    CHECK("[b{gt}[v{p}duration],[c{i}180000]]" == canonicalize(parseOk("@duration>180000")));
    CHECK("[b{ge}[v{p}bitrate],[c{i}320]]" == canonicalize(parseOk("@bitrate>=320")));
  }

  TEST_CASE("Parser - Like", "[query][unit][parser]")
  {
    CHECK("[b{like}[v{m}title],[c{s}Love]]" == canonicalize(parseOk("$title~Love")));
    CHECK("[b{like}[v{m}artist],[c{s}Bach]]" == canonicalize(parseOk("$artist~Bach")));
  }

  TEST_CASE("Parser - In List", "[query][unit][parser]")
  {
    CHECK("[b{in}[v{m}artist],[l[c{s}Bach][c{s}Mozart]]]" == canonicalize(parseOk(R"($artist in ["Bach", "Mozart"])")));
    CHECK("[b{in}[v{m}year],[l[c{i}1990][c{i}1991]]]" == canonicalize(parseOk("$year in [1990, 1991]")));
    CHECK("[b{in}[v{p}duration],[l[c{u}3m][c{u}4m]]]" == canonicalize(parseOk("@duration in [3m, 4m]")));
  }

  TEST_CASE("Parser - In Range", "[query][unit][parser]")
  {
    CHECK("[b{in}[v{m}year],[r[c{i}1990],[c{i}1999]]]" == canonicalize(parseOk("$year in 1990..1999")));
    CHECK("[b{in}[v{p}duration],[r[c{u}2m30s],[c{u}5m]]]" == canonicalize(parseOk("@duration in 2m30s..5m")));
    CHECK("[b{in}[v{p}sampleRate],[r[c{u}44.1k],[c{u}48k]]]" == canonicalize(parseOk("@sampleRate in 44.1k..48k")));
  }

  TEST_CASE("Parser - Logical Operators", "[query][unit][parser]")
  {
    CHECK("[b{and}[b{eq}[v{m}artist],[c{s}Bach]],[c{b}true]]" == canonicalize(parseOk("$artist=Bach && true")));
    CHECK("[b{and}[b{eq}[v{m}artist],[c{s}Bach]],[c{b}true]]" == canonicalize(parseOk("$artist=Bach and true")));
    CHECK("[b{or}[b{eq}[v{m}artist],[c{s}Bach]],[c{b}true]]" == canonicalize(parseOk("$artist=Bach || true")));
    CHECK("[b{or}[b{eq}[v{m}artist],[c{s}Bach]],[c{b}true]]" == canonicalize(parseOk("$artist=Bach or true")));
    CHECK("[u{not}[v{m}artist]]" == canonicalize(parseOk("not $artist")));
    CHECK("[u{not}[v{m}artist]]" == canonicalize(parseOk("!$artist")));
  }

  TEST_CASE("Parser - Existence Tests", "[query][unit][parser]")
  {
    CHECK("[u{exists}[v{m}year]]" == canonicalize(parseOk("$year?")));
    CHECK("[u{exists}[v{p}duration]]" == canonicalize(parseOk("@duration?")));
    CHECK("[u{exists}[v{c}rating]]" == canonicalize(parseOk("%rating?")));
    CHECK("[u{exists}[v{c}Replay Gain]]" == canonicalize(parseOk(R"(%"Replay Gain"?)")));
    CHECK("[u{exists}[v{c}Replay Gain]]" == canonicalize(parseOk(R"(%["Replay Gain"]?)")));
    CHECK("[u{exists}[v{t}favorite]]" == canonicalize(parseOk("#favorite?")));
    CHECK("[u{not}[u{exists}[v{m}year]]]" == canonicalize(parseOk("!$year?")));
    CHECK("[u{not}[u{exists}[v{m}year]]]" == canonicalize(parseOk("not $year?")));

    CHECK("[u{exists}[b{eq}[v{m}year],[c{i}1990]]]" == canonicalize(parseOk("($year = 1990)?")));
    CHECK("[u{exists}[c{i}1990]]" == canonicalize(parseOk("1990?")));
    CHECK("[u{exists}[c{s}Bach]]" == canonicalize(parseOk(R"("Bach"?)")));
  }

  TEST_CASE("Parser - Precedence And Grouping", "[query][unit][parser]")
  {
    SECTION("And Binds Tighter Than Or")
    {
      CHECK("[b{or}[b{eq}[v{m}a],[c{s}x]],[b{and}[b{eq}[v{m}b],[c{s}y]],[b{eq}[v{m}c],[c{s}z]]]]" ==
            canonicalize(parseOk("$a = x or $b = y and $c = z")));
    }

    SECTION("Parentheses Override Precedence")
    {
      CHECK("[b{and}[b{or}[b{eq}[v{m}a],[c{s}x]],[b{eq}[v{m}b],[c{s}y]]],[b{eq}[v{m}c],[c{s}z]]]" ==
            canonicalize(parseOk("($a = x or $b = y) and $c = z")));
    }

    SECTION("Add Binds Tighter Than Relational")
    {
      CHECK("[b{eq}[b{add}[v{m}trackNumber],[c{i}1]],[c{i}12]]" == canonicalize(parseOk("$trackNumber + 1 = 12")));
    }

    SECTION("Nested Parentheses Parse Correctly")
    {
      CHECK("[b{eq}[v{m}artist],[c{s}Bach]]" == canonicalize(parseOk("(($artist = Bach))")));
    }
  }

  TEST_CASE("Parser - Keyword Boundaries And Token Rules", "[query][unit][parser]")
  {
    SECTION("Bareword CanContainKeywordSubstring")
    {
      CHECK("[c{s}Bandroid]" == canonicalize(parseOk("Bandroid")));
      CHECK("[c{s}oratorio]" == canonicalize(parseOk("oratorio")));
      CHECK("[c{s}notation]" == canonicalize(parseOk("notation")));
      CHECK("[c{s}in9]" == canonicalize(parseOk("in9")));
      CHECK("[c{s}in_value]" == canonicalize(parseOk("in_value")));
    }

    SECTION("Keyword Operators Honor Bareword Boundary")
    {
      // and/or/not must only act as operators when standing as a complete token, exactly like in.
      // A trailing digit or underscore keeps them part of a bareword rather than splitting off.
      CHECK("[c{s}and9]" == canonicalize(parseOk("and9")));
      CHECK("[c{s}or9]" == canonicalize(parseOk("or9")));
      CHECK("[c{s}not9]" == canonicalize(parseOk("not9")));
      CHECK("[c{s}and_x]" == canonicalize(parseOk("and_x")));
      CHECK("[c{s}or_x]" == canonicalize(parseOk("or_x")));
      CHECK("[c{s}not_x]" == canonicalize(parseOk("not_x")));

      // In operator position, a near-keyword token must stay a single bareword (here concatenated
      // onto the preceding operand) rather than being mis-split into an operator plus a residual.
      CHECK("[b{add}[v{m}a],[c{s}and_b]]" == canonicalize(parseOk("$a and_b")));
      CHECK("[b{add}[v{m}a],[c{s}or_b]]" == canonicalize(parseOk("$a or_b")));
    }

    SECTION("Logical Keywords Are Case Insensitive")
    {
      // Any casing of and/or/not/in folds to the same operator (identical AST to the lowercase form).
      CHECK("[b{and}[b{eq}[v{m}artist],[c{s}Bach]],[c{b}true]]" == canonicalize(parseOk("$artist=Bach AND true")));
      CHECK("[b{and}[b{eq}[v{m}artist],[c{s}Bach]],[c{b}true]]" == canonicalize(parseOk("$artist=Bach And true")));
      CHECK("[u{not}[v{m}artist]]" == canonicalize(parseOk("NOT $artist")));
      CHECK("[u{not}[v{m}artist]]" == canonicalize(parseOk("Not $artist")));
      CHECK("[b{in}[v{m}year],[l[c{i}1990][c{i}1991]]]" == canonicalize(parseOk("$year IN [1990, 1991]")));

      // Boolean constants fold across casing too.
      CHECK("[c{b}true]" == canonicalize(parseOk("TRUE")));
      CHECK("[c{b}true]" == canonicalize(parseOk("True")));
      CHECK("[c{b}false]" == canonicalize(parseOk("FALSE")));

      // Reserved in any case: an unquoted bareword equal to a keyword (any casing) is rejected,
      // so it must be quoted; a longer bareword merely containing it stays a plain string.
      CHECK("[b{eq}[v{m}a],[c{s}AND]]" == canonicalize(parseOk("$a = 'AND'")));
      CHECK("[c{s}ANDROID]" == canonicalize(parseOk("ANDROID")));
    }

    SECTION("Custom Identifier Allows Underscore And Digits")
    {
      CHECK("[v{c}replaygain_track_gain_db]" == canonicalize(parseOk("%replaygain_track_gain_db")));
    }
  }

  TEST_CASE("Parser - Arithmetic", "[query][unit][parser]")
  {
    CHECK("[b{add}[c{s}hello],[v{m}artist]]" == canonicalize(parseOk("hello + $artist")));
    CHECK("[b{add}[v{m}trackNumber],[c{i}12]]" == canonicalize(parseOk("$trackNumber + 12")));
    CHECK("[b{add}[v{p}duration],[c{i}1000]]" == canonicalize(parseOk("@duration+1000")));
  }

  TEST_CASE("Parser - Boolean Constants", "[query][unit][parser]")
  {
    CHECK("[c{b}true]" == canonicalize(parseOk("true")));
    CHECK("[c{b}false]" == canonicalize(parseOk("false")));
  }

  TEST_CASE("Parser - Integer Constants", "[query][unit][parser]")
  {
    CHECK("[c{i}0]" == canonicalize(parseOk("0")));
    CHECK("[c{i}123]" == canonicalize(parseOk("123")));
    CHECK("[c{i}-456]" == canonicalize(parseOk("-456")));
  }

  TEST_CASE("Parser - Unit Constants", "[query][unit][parser]")
  {
    CHECK("[c{u}3m]" == canonicalize(parseOk("3m")));
    CHECK("[c{u}2m30s]" == canonicalize(parseOk("2m30s")));
    CHECK("[b{ge}[v{p}duration],[c{u}120s]]" == canonicalize(parseOk("@duration>=120s")));
    CHECK("[b{ge}[v{p}bitrate],[c{u}100k]]" == canonicalize(parseOk("@bitrate>=100k")));
    CHECK("[b{eq}[v{p}sampleRate],[c{u}44.1k]]" == canonicalize(parseOk("@sampleRate=44.1k")));
  }

  TEST_CASE("Parser - Empty String", "[query][unit][parser]")
  {
    CHECK("[c{s}]" == canonicalize(parseOk("''")));
    CHECK("[c{s}]" == canonicalize(parseOk("\"\"")));
  }

  TEST_CASE("Parser - Invalid Input Matrix", "[query][unit][parser]")
  {
    SECTION("Empty and Whitespace")
    {
      REQUIRE(parseFails(""));
      REQUIRE(parseFails("   "));
    }

    SECTION("Variable Token Rules")
    {
      REQUIRE(parseFails("$1bad"));
      REQUIRE(parseFails("$"));
      REQUIRE(parseFails("@"));
      REQUIRE(parseFails("#"));
      REQUIRE(parseFails("%"));
    }

    SECTION("Unterminated Quotes")
    {
      REQUIRE(parseFails("'Bach"));
      REQUIRE(parseFails("\"Bach"));
    }

    SECTION("Malformed Parentheses")
    {
      REQUIRE(parseFails("()"));
      REQUIRE(parseFails("($artist = Bach"));
    }

    SECTION("Malformed Lists")
    {
      REQUIRE(parseFails("$artist in []"));
      REQUIRE(parseFails("$artist in [Bach,]"));
      REQUIRE(parseFails("$artist in [Bach Mozart]"));
    }

    SECTION("Malformed Ranges")
    {
      REQUIRE(parseFails("$year in 1990.."));
      REQUIRE(parseFails("$year in ..1999"));
      REQUIRE(parseFails("$year in 1990...1999"));
    }

    SECTION("Invalid Escape Sequences")
    {
      REQUIRE(parseFails(R"($title = "a \x")"));
      REQUIRE(parseFails(R"($title = 'a \x')"));
      REQUIRE(parseFails(R"($title = "a \u")"));
      REQUIRE(parseFails(R"(%"bad\")"));
      REQUIRE(parseFails(R"xy(%"trailing\)xy"));
    }
  }

  TEST_CASE("Parser - Matches Expression Syntax Without Building AST", "[query][unit][parser]")
  {
    SECTION("Accepts complete expressions")
    {
      CHECK(matchesExpressionSyntax("#rock"));
      CHECK(matchesExpressionSyntax(R"($artist = "Miles")"));
      CHECK(matchesExpressionSyntax("$title"));
      CHECK(matchesExpressionSyntax("3m"));
    }

    SECTION("Rejects incomplete or invalid expressions")
    {
      CHECK_FALSE(matchesExpressionSyntax(""));
      CHECK_FALSE(matchesExpressionSyntax("$artist ="));
      CHECK_FALSE(matchesExpressionSyntax(R"($artist in ["Miles",)"));
      CHECK_FALSE(matchesExpressionSyntax(R"(#"unterminated)"));
    }
  }

  TEST_CASE("Parser - Reports Syntax Errors As Result Errors", "[query][unit][parser]")
  {
    auto const ok = ::ao::query::parse("$artist = Bach");
    CHECK(ok.has_value());

    auto const bad = ::ao::query::parse("$artist =");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == Error::Code::FormatRejected);
    CHECK_FALSE(bad.error().message.empty());
  }
} // namespace ao::query::test
