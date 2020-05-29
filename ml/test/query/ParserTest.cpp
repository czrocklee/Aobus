/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <gtest/gtest.h>
#include <rs/ml/expr/Parser.h>
#include <rs/common/utility/VariantVisitor.h>

namespace rs::ml::expr
{
  namespace
  {
    const char* toString(Operator op)
    {
      switch (op)
      {
        case Operator::And: return "and";
        case Operator::Or: return "or";
        case Operator::Not: return "neg";
        case Operator::Equal: return "eq";
        case Operator::Like: return "like";
        case Operator::Less: return "lt";
        case Operator::LessEqual: return "le";
        case Operator::Greater: return "gt";
        case Operator::GreaterEqual: return "ge";
        case Operator::Add: return "add";
        default: assert(false);
      }
    }

    struct Canonicalizer
    {
      Canonicalizer(){};

      void operator()(const BinaryExpression& binary)
      {
        assert(binary.operation);
        oss << "[b{" << toString(binary.operation->op) << "}";
        boost::apply_visitor(*this, binary.operand);
        oss << ",";
        boost::apply_visitor(*this, binary.operation->operand);
        oss << "]";
      }

      void operator()(const UnaryExpression& unary)
      {
        oss << "[u{!}";
        boost::apply_visitor(*this, unary.operand);
        oss << "]";
      }

      void operator()(const VariableExpression& variable)
      {
        oss << "[v{";

        switch (variable.type)
        {
          case VariableType::Metadata: oss << 'm'; break;
          case VariableType::Property: oss << 'p'; break;
          case VariableType::Tag: oss << 't'; break;
          case VariableType::Custom: oss << 'c'; break;
          default: break;
        }

        oss << "}" << variable.name << "]";
      }

      void operator()(const ConstantExpression& constant)
      {
        oss << "[c{";
        std::visit(
          rs::common::utility::makeVisitor(
            [this](boost::blank) { oss << "n}"; },
            [this](bool val) { oss << "b}" << (val ? "true" : "false"); },
            [this](std::int64_t val) { oss << "i}" << val; },
            [this](std::string_view val) { oss << "s}" << val; }),
          constant);
        oss << "]";
      }

      std::ostringstream oss;
    };

    std::string canonicalize(const Expression& expr)
    {
      Canonicalizer canonicalizer{};
      boost::apply_visitor(canonicalizer, expr);
      return canonicalizer.oss.str();
    }
  }

  TEST(ParserTest, StringLiteral)
  {
    EXPECT_EQ("[c{s}Artist]", canonicalize(parse("Artist")));
    EXPECT_EQ("[c{s}Artist]", canonicalize(parse("\"Artist\"")));
    EXPECT_EQ("[c{s}Artist]", canonicalize(parse("'Artist'")));
  }

  TEST(ParserTest, Variable)
  {
    EXPECT_EQ("[v{m}Artist]", canonicalize(parse("$Artist")));
    EXPECT_EQ("[v{p}BitRate]", canonicalize(parse("@BitRate")));
    EXPECT_EQ("[v{t}Tag]", canonicalize(parse("#Tag")));
    EXPECT_EQ("[v{c}Custom]", canonicalize(parse("%Custom")));
  }

  TEST(ParserTest, StringCat)
  {
    EXPECT_EQ("[b{add}[c{s}Artist],[c{s}Album]]", canonicalize(parse("Artist + Album")));
    EXPECT_EQ("[b{add}[c{s}Artist],[v{m}Album]]", canonicalize(parse("Artist + $Album")));
    EXPECT_EQ("[b{add}[c{s}Artist],[v{m}Album]]", canonicalize(parse("Artist$Album")));
    EXPECT_EQ(
      "[b{add}[b{add}[b{add}[b{add}[b{add}[b{add}[v{m}AlbumArtist],[c{s}/]],[v{m}Album]],[c{s}/"
      "]],[v{m}TrackNumber]],[c{s}-]],[v{m}Title]]",
      canonicalize(parse("$AlbumArtist/$Album/$TrackNumber-$Title")));
  }

  TEST(ParserTest, Equal)
  {
    EXPECT_EQ("[b{eq}[v{m}Artist],[c{s}Bach]]", canonicalize(parse("$Artist=Bach")));
    EXPECT_EQ("[b{eq}[v{m}TrackNumber],[c{i}12]]", canonicalize(parse("$TrackNumber=12")));
  }

  TEST(ParserTest, Add)
  {
    EXPECT_EQ("[b{add}[c{s}hello],[v{m}Artist]]", canonicalize(parse("hello + $Artist")));
    EXPECT_EQ("[b{add}[v{m}TrackNumber],[c{i}12]]", canonicalize(parse("$TrackNumber + 12")));
  }
}