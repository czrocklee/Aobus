// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <ao/query/Parser.h>
#include <ao/utility/VariantVisitor.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <sstream>
#include <string>

using namespace ao::query;

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
    case Operator::Add: return "add";
    default: return "unknown";
  }
}

struct Canonicalizer
{
  void operator()(std::unique_ptr<BinaryExpression> const& binary)
  {
    if (!binary) return;
    oss << "[b{" << toString(binary->operation->op) << "}";
    std::visit(*this, binary->operand);
    oss << ",";
    std::visit(*this, binary->operation->operand);
    oss << "]";
  }

  void operator()(std::unique_ptr<UnaryExpression> const& unary)
  {
    if (!unary) return;
    oss << "[u{!}";
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
    std::visit(ao::utility::makeVisitor([this](std::monostate) { oss << "n}"; },
                                        [this](bool val) { oss << "b}" << (val ? "true" : "false"); },
                                        [this](std::int64_t val) { oss << "i}" << val; },
                                        [this](UnitConstantExpression const& val) { oss << "u}" << val.lexeme; },
                                        [this](std::string_view val) { oss << "s}" << val; }),
               constant);
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

TEST_CASE("Parser - String Literal")
{
  CHECK("[c{s}Artist]" == canonicalize(parse("Artist")));
  CHECK("[c{s}Artist]" == canonicalize(parse("\"Artist\"")));
  CHECK("[c{s}Artist]" == canonicalize(parse("'Artist'")));
}

TEST_CASE("Parser - Variable")
{
  CHECK("[v{m}title]" == canonicalize(parse("$title")));
  CHECK("[v{m}artist]" == canonicalize(parse("$artist")));
  CHECK("[v{m}composer]" == canonicalize(parse("$composer")));
  CHECK("[v{m}work]" == canonicalize(parse("$work")));
  CHECK("[v{m}w]" == canonicalize(parse("$w")));
  CHECK("[v{p}duration]" == canonicalize(parse("@duration")));
  CHECK("[v{t}Tag]" == canonicalize(parse("#Tag")));
  CHECK("[v{c}isrc]" == canonicalize(parse("%isrc")));
  CHECK("[v{c}replaygaintrackgaindb]" == canonicalize(parse("%replaygaintrackgaindb")));
}

TEST_CASE("Parser - Variable Shortcuts")
{
  CHECK("[v{m}t]" == canonicalize(parse("$t")));
  CHECK("[v{m}a]" == canonicalize(parse("$a")));
  CHECK("[v{m}al]" == canonicalize(parse("$al")));
  CHECK("[v{m}g]" == canonicalize(parse("$g")));
  CHECK("[v{m}c]" == canonicalize(parse("$c")));
  CHECK("[v{m}y]" == canonicalize(parse("$y")));
  CHECK("[v{m}tn]" == canonicalize(parse("$tn")));
  CHECK("[v{p}l]" == canonicalize(parse("@l")));
  CHECK("[v{p}br]" == canonicalize(parse("@br")));
  CHECK("[v{p}sr]" == canonicalize(parse("@sr")));
  CHECK("[v{p}bd]" == canonicalize(parse("@bd")));
}

TEST_CASE("Parser - String Cat")
{
  CHECK("[b{add}[c{s}Artist],[c{s}Album]]" == canonicalize(parse("Artist + Album")));
  CHECK("[b{add}[c{s}Artist],[v{m}Album]]" == canonicalize(parse("Artist + $Album")));
  CHECK("[b{add}[c{s}Artist],[v{m}Album]]" == canonicalize(parse("Artist$Album")));
}

TEST_CASE("Parser - Equal")
{
  CHECK("[b{eq}[v{m}artist],[c{s}Bach]]" == canonicalize(parse("$artist=Bach")));
  CHECK("[b{eq}[v{m}trackNumber],[c{i}12]]" == canonicalize(parse("$trackNumber=12")));
  CHECK("[b{eq}[v{m}year],[c{i}2020]]" == canonicalize(parse("$year=2020")));
}

TEST_CASE("Parser - NotEqual")
{
  CHECK("[b{ne}[v{m}artist],[c{s}Bach]]" == canonicalize(parse("$artist!=Bach")));
  CHECK("[b{ne}[v{m}year],[c{i}2020]]" == canonicalize(parse("$year!=2020")));
}

TEST_CASE("Parser - Relational")
{
  CHECK("[b{lt}[v{m}year],[c{i}2000]]" == canonicalize(parse("$year<2000")));
  CHECK("[b{le}[v{m}year],[c{i}2000]]" == canonicalize(parse("$year<=2000")));
  CHECK("[b{gt}[v{p}duration],[c{i}180000]]" == canonicalize(parse("@duration>180000")));
  CHECK("[b{ge}[v{p}bitrate],[c{i}320]]" == canonicalize(parse("@bitrate>=320")));
}

TEST_CASE("Parser - Like")
{
  CHECK("[b{like}[v{m}title],[c{s}Love]]" == canonicalize(parse("$title~Love")));
  CHECK("[b{like}[v{m}artist],[c{s}Bach]]" == canonicalize(parse("$artist~Bach")));
}

TEST_CASE("Parser - Logical Operators")
{
  CHECK("[b{and}[b{eq}[v{m}artist],[c{s}Bach]],[c{b}true]]" == canonicalize(parse("$artist=Bach && true")));
  CHECK("[b{and}[b{eq}[v{m}artist],[c{s}Bach]],[c{b}true]]" == canonicalize(parse("$artist=Bach and true")));
  CHECK("[b{or}[b{eq}[v{m}artist],[c{s}Bach]],[c{b}true]]" == canonicalize(parse("$artist=Bach || true")));
  CHECK("[b{or}[b{eq}[v{m}artist],[c{s}Bach]],[c{b}true]]" == canonicalize(parse("$artist=Bach or true")));
  CHECK("[u{!}[v{m}artist]]" == canonicalize(parse("not $artist")));
  CHECK("[u{!}[v{m}artist]]" == canonicalize(parse("!$artist")));
}

TEST_CASE("Parser - Precedence And Grouping")
{
  SECTION("And Binds Tighter Than Or")
  {
    CHECK("[b{or}[b{eq}[v{m}a],[c{s}x]],[b{and}[b{eq}[v{m}b],[c{s}y]],[b{eq}[v{m}c],[c{s}z]]]]" ==
          canonicalize(parse("$a = x or $b = y and $c = z")));
  }

  SECTION("Parentheses Override Precedence")
  {
    CHECK("[b{and}[b{or}[b{eq}[v{m}a],[c{s}x]],[b{eq}[v{m}b],[c{s}y]]],[b{eq}[v{m}c],[c{s}z]]]" ==
          canonicalize(parse("($a = x or $b = y) and $c = z")));
  }

  SECTION("Add Binds Tighter Than Relational")
  {
    CHECK("[b{eq}[b{add}[v{m}trackNumber],[c{i}1]],[c{i}12]]" == canonicalize(parse("$trackNumber + 1 = 12")));
  }

  SECTION("Nested Parentheses Parse Correctly")
  {
    CHECK("[b{eq}[v{m}artist],[c{s}Bach]]" == canonicalize(parse("(($artist = Bach))")));
  }
}

TEST_CASE("Parser - Keyword Boundaries And Token Rules")
{
  SECTION("Bareword CanContainKeywordSubstring")
  {
    CHECK("[c{s}Bandroid]" == canonicalize(parse("Bandroid")));
    CHECK("[c{s}oratorio]" == canonicalize(parse("oratorio")));
    CHECK("[c{s}notation]" == canonicalize(parse("notation")));
  }

  SECTION("Custom Identifier Allows Underscore And Digits")
  {
    CHECK("[v{c}replaygain_track_gain_db]" == canonicalize(parse("%replaygain_track_gain_db")));
  }
}

TEST_CASE("Parser - Arithmetic")
{
  CHECK("[b{add}[c{s}hello],[v{m}artist]]" == canonicalize(parse("hello + $artist")));
  CHECK("[b{add}[v{m}trackNumber],[c{i}12]]" == canonicalize(parse("$trackNumber + 12")));
  CHECK("[b{add}[v{p}duration],[c{i}1000]]" == canonicalize(parse("@duration+1000")));
}

TEST_CASE("Parser - Boolean Constants")
{
  CHECK("[c{b}true]" == canonicalize(parse("true")));
  CHECK("[c{b}false]" == canonicalize(parse("false")));
}

TEST_CASE("Parser - Integer Constants")
{
  CHECK("[c{i}0]" == canonicalize(parse("0")));
  CHECK("[c{i}123]" == canonicalize(parse("123")));
  CHECK("[c{i}-456]" == canonicalize(parse("-456")));
}

TEST_CASE("Parser - Unit Constants")
{
  CHECK("[c{u}3m]" == canonicalize(parse("3m")));
  CHECK("[b{ge}[v{p}duration],[c{u}120s]]" == canonicalize(parse("@duration>=120s")));
  CHECK("[b{ge}[v{p}bitrate],[c{u}100k]]" == canonicalize(parse("@bitrate>=100k")));
  CHECK("[b{eq}[v{p}sampleRate],[c{u}44.1k]]" == canonicalize(parse("@sampleRate=44.1k")));
}

TEST_CASE("Parser - Empty String")
{
  CHECK("[c{s}]" == canonicalize(parse("''")));
  CHECK("[c{s}]" == canonicalize(parse("\"\"")));
}

TEST_CASE("Parser - Invalid Input Matrix")
{
  SECTION("Empty and Whitespace")
  {
    REQUIRE_THROWS(parse(""));
    REQUIRE_THROWS(parse("   "));
  }

  SECTION("Variable Token Rules")
  {
    REQUIRE_THROWS(parse("$1bad"));
    REQUIRE_THROWS(parse("$"));
    REQUIRE_THROWS(parse("@"));
    REQUIRE_THROWS(parse("#"));
    REQUIRE_THROWS(parse("%"));
  }

  SECTION("Unterminated Quotes")
  {
    REQUIRE_THROWS(parse("'Bach"));
    REQUIRE_THROWS(parse("\"Bach"));
  }

  SECTION("Malformed Parentheses")
  {
    REQUIRE_THROWS(parse("()"));
    REQUIRE_THROWS(parse("($artist = Bach"));
  }
}
