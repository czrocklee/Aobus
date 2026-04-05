// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>
#include <rs/expr/Parser.h>
#include <rs/utility/VariantVisitor.h>

#include <sstream>
#include <string>

using namespace rs::expr;

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
    std::visit(rs::utility::makeVisitor([this](std::monostate) { oss << "n}"; },
                                        [this](bool val) { oss << "b}" << (val ? "true" : "false"); },
                                        [this](std::int64_t val) { oss << "i}" << val; },
                                        [this](std::string_view val) { oss << "s}" << val; }),
               constant);
    oss << "]";
  }

  std::ostringstream oss;
};

std::string canonicalize(Expression const& expr)
{
  Canonicalizer canonicalizer{};
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

TEST_CASE("Parser - Empty String")
{
  CHECK("[c{s}]" == canonicalize(parse("''")));
  CHECK("[c{s}]" == canonicalize(parse("\"\"")));
}

TEST_CASE("Parser - Invalid Input")
{
  REQUIRE_THROWS(parse(""));
  REQUIRE_THROWS(parse("   "));
}
