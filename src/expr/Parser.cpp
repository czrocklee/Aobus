// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/Exception.h>
#include <rs/expr/Parser.h>

#include <lexy/dsl.hpp>
#include <lexy/input/base.hpp>
#include <lexy/input/string_input.hpp>

#include <charconv>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace
{
  namespace dsl = lexy::dsl;
  using namespace rs::expr;

  struct ParseFailure
  {
    char const* position;
  };

  class Parser
  {
  private:
    using Input = lexy::string_input<lexy::utf8_char_encoding>;
    using Reader = lexy::input_reader<Input>;

    static bool isAsciiDigit(unsigned char ch) { return ch >= '0' && ch <= '9'; }

    static bool isAsciiAlpha(unsigned char ch)
    {
      return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    }

    static bool isIdentifierContinuation(unsigned char ch)
    {
      return isAsciiAlpha(ch) || isAsciiDigit(ch) || ch == '_';
    }

    static bool isUnquotedStringChar(unsigned char ch)
    {
      switch (ch)
      {
        case ' ':
        case '\'':
        case '"':
        case '$':
        case '@':
        case '#':
        case '%':
        case '!':
        case '(':
        case ')':
        case '&':
        case '|':
        case '=':
        case '~':
        case '<':
        case '>':
        case '+':
        case '-':
          return false;
        default:
          return true;
      }
    }

  public:
    explicit Parser(std::string_view expr) : _expr{expr}, _input{expr}, _reader{_input.reader()} {}

    Expression parse()
    {
      auto root = parseLogicalOr();

      skipWhitespace();

      if (!atEnd())
      {
        fail();
      }

      return root;
    }

    std::string remaining(char const* position) const
    {
      auto begin = _expr.data();
      auto end = _expr.data() + _expr.size();

      if (position == nullptr || position < begin || position > end)
      {
        return std::string{_expr};
      }

      return std::string{position, end};
    }

  private:
    [[noreturn]] void fail() { throw ParseFailure{_reader.position()}; }

    bool atEnd() const { return _reader.peek() == Reader::encoding::eof(); }

    unsigned char currentByte() const { return static_cast<unsigned char>(_reader.peek()); }

    void skipWhitespace()
    {
      while (lexy::try_match_token(dsl::ascii::space, _reader))
      {
      }
    }

    template<typename Token>
    bool consumeToken(Token token)
    {
      skipWhitespace();
      return lexy::try_match_token(token, _reader);
    }

    bool consumeKeyword(std::string_view keyword)
    {
      skipWhitespace();
      auto marker = _reader.current();

      for (char expected : keyword)
      {
        if (atEnd() || currentByte() != static_cast<unsigned char>(expected))
        {
          _reader.reset(marker);
          return false;
        }

        _reader.bump();
      }

      if (!atEnd() && isIdentifierContinuation(currentByte()))
      {
        _reader.reset(marker);
        return false;
      }

      return true;
    }

    bool consumeLogicalOrOperator()
    {
      return consumeKeyword("or") || consumeToken(LEXY_LIT("||"));
    }

    bool consumeLogicalAndOperator()
    {
      return consumeKeyword("and") || consumeToken(LEXY_LIT("&&"));
    }

    bool consumeLogicalNotOperator()
    {
      return consumeKeyword("not") || consumeToken(dsl::lit_c<'!'>);
    }

    std::optional<Operator> consumeRelationalOperator()
    {
      if (consumeToken(LEXY_LIT("!=")))
      {
        return Operator::NotEqual;
      }

      if (consumeToken(LEXY_LIT("<=")))
      {
        return Operator::LessEqual;
      }

      if (consumeToken(LEXY_LIT(">=")))
      {
        return Operator::GreaterEqual;
      }

      if (consumeToken(dsl::lit_c<'='>))
      {
        return Operator::Equal;
      }

      if (consumeToken(dsl::lit_c<'~'>))
      {
        return Operator::Like;
      }

      if (consumeToken(dsl::lit_c<'<'>))
      {
        return Operator::Less;
      }

      if (consumeToken(dsl::lit_c<'>'>))
      {
        return Operator::Greater;
      }

      return std::nullopt;
    }

    bool consumeAddOperator() { return consumeToken(dsl::lit_c<'+'>); }

    bool canStartPrimary()
    {
      skipWhitespace();

      if (atEnd())
      {
        return false;
      }

      auto ch = currentByte();

      // '!' can start a primary (NOT expression) but not if followed by '=' (that's != operator)
      if (ch == '!')
      {
        auto nextPos = std::next(_reader.position());
        if (nextPos != _expr.end() && *nextPos == '=')
        {
          return false;  // This is !=, not !, don't start a primary
        }
        return true;
      }

      if (ch == '$' || ch == '@' || ch == '#' || ch == '%' || ch == '(' || ch == '\'' || ch == '"')
      {
        return true;
      }

      if (ch == '+' || ch == '-')
      {
        auto marker = _reader.current();
        _reader.bump();
        auto hasDigit = !atEnd() && isAsciiDigit(currentByte());
        _reader.reset(marker);
        return hasDigit;
      }

      if (isAsciiDigit(ch) || isAsciiAlpha(ch) || ch == '_')
      {
        return true;
      }

      return isUnquotedStringChar(ch);
    }

    std::optional<std::string> parseIdentifier()
    {
      skipWhitespace();
      auto marker = _reader.current();
      auto begin = _reader.position();

      if (!lexy::try_match_token(dsl::ascii::alpha_underscore, _reader))
      {
        _reader.reset(marker);
        return std::nullopt;
      }

      while (lexy::try_match_token(dsl::ascii::alpha_digit_underscore, _reader))
      {
      }

      return std::string{begin, _reader.position()};
    }

    std::optional<VariableExpression> tryParseVariable()
    {
      skipWhitespace();
      auto marker = _reader.current();

      if (atEnd())
      {
        return std::nullopt;
      }

      VariableExpression variable{};
      switch (currentByte())
      {
        case '$':
          variable.type = VariableType::Metadata;
          break;
        case '@':
          variable.type = VariableType::Property;
          break;
        case '#':
          variable.type = VariableType::Tag;
          break;
        case '%':
          variable.type = VariableType::Custom;
          break;
        default:
          return std::nullopt;
      }

      _reader.bump();

      auto identifier = parseIdentifier();
      if (!identifier)
      {
        _reader.reset(marker);
        fail();
      }

      variable.name = std::move(*identifier);
      return variable;
    }

    std::optional<bool> tryParseBoolConstant()
    {
      if (consumeKeyword("true"))
      {
        return true;
      }

      if (consumeKeyword("false"))
      {
        return false;
      }

      return std::nullopt;
    }

    std::optional<std::int64_t> tryParseIntegerConstant()
    {
      skipWhitespace();
      auto marker = _reader.current();

      bool negative = false;
      if (!atEnd() && (currentByte() == '+' || currentByte() == '-'))
      {
        negative = currentByte() == '-';
        _reader.bump();
      }

      auto digitsBegin = _reader.position();
      while (!atEnd() && isAsciiDigit(currentByte()))
      {
        _reader.bump();
      }

      auto digitsEnd = _reader.position();
      if (digitsBegin == digitsEnd)
      {
        _reader.reset(marker);
        return std::nullopt;
      }

      std::uint64_t magnitude = 0;
      auto [ptr, ec] = std::from_chars(digitsBegin, digitsEnd, magnitude);
      if (ec != std::errc{} || ptr != digitsEnd)
      {
        _reader.reset(marker);
        return std::nullopt;
      }

      constexpr auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
      constexpr auto minMagnitude = max + 1;

      if (!negative)
      {
        if (magnitude > max)
        {
          _reader.reset(marker);
          return std::nullopt;
        }

        return static_cast<std::int64_t>(magnitude);
      }

      if (magnitude > minMagnitude)
      {
        _reader.reset(marker);
        return std::nullopt;
      }

      if (magnitude == minMagnitude)
      {
        return std::numeric_limits<std::int64_t>::min();
      }

      return -static_cast<std::int64_t>(magnitude);
    }

    std::optional<std::string> tryParseQuotedString(unsigned char quote)
    {
      if (atEnd() || currentByte() != quote)
      {
        return std::nullopt;
      }

      _reader.bump();
      auto begin = _reader.position();

      while (!atEnd() && currentByte() != quote)
      {
        _reader.bump();
      }

      if (atEnd())
      {
        fail();
      }

      auto end = _reader.position();
      _reader.bump();

      return std::string{begin, end};
    }

    std::optional<std::string> tryParseStringConstant()
    {
      skipWhitespace();
      auto marker = _reader.current();

      if (auto quoted = tryParseQuotedString('\''))
      {
        return quoted;
      }

      _reader.reset(marker);
      if (auto quoted = tryParseQuotedString('"'))
      {
        return quoted;
      }

      _reader.reset(marker);
      auto begin = _reader.position();

      while (!atEnd() && isUnquotedStringChar(currentByte()))
      {
        _reader.bump();
      }

      if (_reader.position() == begin)
      {
        _reader.reset(marker);
        return std::nullopt;
      }

      return std::string{begin, _reader.position()};
    }

    std::optional<ConstantExpression> tryParseConstant()
    {
      skipWhitespace();
      auto marker = _reader.current();

      if (auto boolean = tryParseBoolConstant())
      {
        return ConstantExpression{*boolean};
      }

      _reader.reset(marker);
      if (auto integer = tryParseIntegerConstant())
      {
        return ConstantExpression{*integer};
      }

      _reader.reset(marker);
      if (auto string = tryParseStringConstant())
      {
        return ConstantExpression{std::move(*string)};
      }

      _reader.reset(marker);
      return std::nullopt;
    }

    std::optional<std::unique_ptr<UnaryExpression>> tryParseLogicalNot()
    {
      skipWhitespace();

      // Check for '!' but NOT '!=' (that's a relational operator)
      if (currentByte() == '!' && (std::next(_reader.position()) == _expr.end() || *std::next(_reader.position()) != '='))
      {
        _reader.bump();
        auto unary = std::make_unique<UnaryExpression>();
        unary->op = Operator::Not;
        unary->operand = parseLogicalOr();
        return unary;
      }

      // Check for "not" keyword
      auto marker = _reader.current();
      if (!consumeKeyword("not"))
      {
        return std::nullopt;
      }

      auto unary = std::make_unique<UnaryExpression>();
      unary->op = Operator::Not;
      unary->operand = parseLogicalOr();
      return unary;
    }

    Expression parsePrimary()
    {
      skipWhitespace();
      auto marker = _reader.current();

      if (auto logicalNot = tryParseLogicalNot())
      {
        return Expression{std::move(*logicalNot)};
      }

      _reader.reset(marker);
      if (auto variable = tryParseVariable())
      {
        return Expression{std::move(*variable)};
      }

      _reader.reset(marker);
      if (auto constant = tryParseConstant())
      {
        return Expression{std::move(*constant)};
      }

      _reader.reset(marker);
      if (consumeToken(dsl::lit_c<'('>))
      {
        auto nested = parseLogicalOr();
        if (!consumeToken(dsl::lit_c<')'>))
        {
          fail();
        }

        return nested;
      }

      fail();
    }

    Expression parseArithmetic()
    {
      auto lhs = parsePrimary();

      if (consumeAddOperator())
      {
        auto bin = std::make_unique<BinaryExpression>();
        bin->operand = std::move(lhs);
        bin->operation = BinaryExpression::Operation{
          .op = Operator::Add,
          .operand = parseArithmetic(),
        };
        return Expression{std::move(bin)};
      }
      else if (canStartPrimary())
      {
        auto bin = std::make_unique<BinaryExpression>();
        bin->operand = std::move(lhs);
        bin->operation = BinaryExpression::Operation{
          .op = Operator::Add,
          .operand = parseArithmetic(),
        };
        return Expression{std::move(bin)};
      }

      return lhs;
    }

    Expression parseRelational()
    {
      auto lhs = parseArithmetic();

      if (auto op = consumeRelationalOperator())
      {
        auto bin = std::make_unique<BinaryExpression>();
        bin->operand = std::move(lhs);
        bin->operation = BinaryExpression::Operation{
          .op = *op,
          .operand = parseRelational(),
        };
        return Expression{std::move(bin)};
      }

      return lhs;
    }

    Expression parseLogicalAnd()
    {
      auto lhs = parseRelational();

      if (consumeLogicalAndOperator())
      {
        auto bin = std::make_unique<BinaryExpression>();
        bin->operand = std::move(lhs);
        bin->operation = BinaryExpression::Operation{
          .op = Operator::And,
          .operand = parseLogicalAnd(),
        };
        return Expression{std::move(bin)};
      }

      return lhs;
    }

    Expression parseLogicalOr()
    {
      auto lhs = parseLogicalAnd();

      if (consumeLogicalOrOperator())
      {
        auto bin = std::make_unique<BinaryExpression>();
        bin->operand = std::move(lhs);
        bin->operation = BinaryExpression::Operation{
          .op = Operator::Or,
          .operand = parseLogicalOr(),
        };
        return Expression{std::move(bin)};
      }

      return lhs;
    }

    std::string_view _expr;
    Input _input;
    Reader _reader;
  };
}

namespace rs::expr
{
  Expression parse(std::string_view expr)
  {
    try
    {
      Parser parser{expr};
      auto root = parser.parse();
      normalize(root);
      return root;
    }
    catch (ParseFailure const& failure)
    {
      Parser parser{expr};
      RS_THROW_FORMAT(rs::Exception, "parsing {} error from [{}]", expr, parser.remaining(failure.position));
    }
  }
}
