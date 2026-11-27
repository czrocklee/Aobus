// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/ExecutionPlan.h>

#include "detail/FieldResolver.h"
#include "detail/Predicate.h"
#include "detail/QueryError.h"
#include <ao/AudioCodec.h>
#include <ao/AudioCodecText.h>
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/library/RecordingDate.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/QueryCompilation.h>
#include <ao/query/detail/Bytecode.h>
#include <ao/utility/String.h>
#include <ao/utility/UnicodeText.h>
#include <ao/utility/VariantVisitor.h>

#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4267) // gperf's generated hash narrows size_t lengths
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "query/UnitDispatch.h"
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

namespace ao::query
{
  namespace
  {
    // Context-free scalars share kTextLiteralField's encoding because TagBloom is not a query variable.
    // Only OperandKind::Text makes that value a string literal when compiling a comparison.
    constexpr Field kNoFieldContext = kTextLiteralField;

    std::string queryTextOrThrow(Result<std::string> textRes, std::string_view const context)
    {
      if (!textRes)
      {
        auto error = std::move(textRes.error());

        if (error.code == Error::Code::InvalidInput || error.code == Error::Code::ValueTooLarge)
        {
          error.code = Error::Code::FormatRejected;
        }

        error.message = std::format("{}: {}", context, error.message);
        detail::throwQueryError(std::move(error));
      }

      return std::move(*textRes);
    }

    std::string normalizeQueryText(std::string_view const text, std::string_view const context)
    {
      return queryTextOrThrow(utility::normalizeUtf8Nfc(text), context);
    }

    std::string makeQueryCaselessKey(std::string_view const text, std::string_view const context)
    {
      return queryTextOrThrow(utility::makeUtf8CaselessKey(text), context);
    }

    enum class OperandKind : std::uint8_t
    {
      Boolean,
      Number,
      Text,
      RecordingDate,
      Credit,
    };

    OperandKind fieldKind(Field field)
    {
      if (isStringField(field) || isDictionaryField(field))
      {
        return OperandKind::Text;
      }

      if (isCreditField(field))
      {
        return OperandKind::Credit;
      }

      if (field == Field::RecordingDate)
      {
        return OperandKind::RecordingDate;
      }

      return field == Field::Tag ? OperandKind::Boolean : OperandKind::Number;
    }

    OperandKind constantKind(ConstantExpression const& constant)
    {
      if (std::holds_alternative<std::string>(constant))
      {
        return OperandKind::Text;
      }

      return std::holds_alternative<bool>(constant) ? OperandKind::Boolean : OperandKind::Number;
    }

    std::string_view operandKindName(OperandKind kind)
    {
      switch (kind)
      {
        case OperandKind::Boolean: return "Boolean";
        case OperandKind::Number: return "numeric";
        case OperandKind::Text: return "text";
        case OperandKind::RecordingDate: return "recording date";
        case OperandKind::Credit: return "credit member";
      }

      return "unknown";
    }

    std::string operandLabel(Expression const& expr)
    {
      if (auto const* variable = std::get_if<VariableExpression>(&expr); variable != nullptr)
      {
        return variableDisplayName(*variable);
      }

      return std::holds_alternative<ConstantExpression>(expr) ? "literal" : "predicate";
    }

    void requireCompatibleConstant(Expression const& lhs,
                                   OperandKind kind,
                                   Field field,
                                   ConstantExpression const& constant)
    {
      auto const literal = constantKind(constant);

      if (kind == OperandKind::RecordingDate || kind == literal ||
          ((kind == OperandKind::Credit || field == Field::Codec) && literal == OperandKind::Text))
      {
        return;
      }

      auto const expected = field == Field::Codec
                              ? "a codec name or an integer"
                              : operandKindName(kind == OperandKind::Credit ? OperandKind::Text : kind);
      detail::throwQueryError(
        "invalid operand for {}: expected {}, got {}", operandLabel(lhs), expected, operandKindName(literal));
    }

    class QueryCompiler final
    {
    public:
      Result<ExecutionPlan> compile(Expression const& expr);

    private:
      struct CompiledConstant final
      {
        std::uint32_t reg = 0;
        std::uint32_t dictionarySymbol = kNoDictionarySymbol;
      };

      struct CompiledOperand final
      {
        std::uint32_t reg;
        OperandKind kind;
        // Only direct fields retain a field-specific comparison or literal conversion.
        Field field = kNoFieldContext;
        std::uint32_t dictionarySymbol = kNoDictionarySymbol;
      };

      // Register-stack helpers. Each value-producing op claims the next register up and
      // each consumed (right) operand is always the current top, so the evaluator can
      // read a binary op's left operand from (operand - 1). These two helpers keep that
      // stack discipline in one place instead of open-coding _nextReg arithmetic.
      std::uint32_t pushReg();
      void popReg(std::uint32_t top);

      // Compile helper functions. Each returns the register holding its result (with
      // _nextReg left incremented by exactly one net), so callers thread registers
      // explicitly rather than re-deriving them from _nextReg. compileInSetList returns
      // nullopt when the list is not eligible for set compilation.
      std::uint32_t addStringConstant(std::string_view str, Field field, OpCode op);
      std::uint32_t addDictionarySymbol(std::string_view text);
      std::uint32_t addInSet(InSet set);
      std::uint32_t compileExpression(Expression const& expr);
      std::uint32_t compilePredicate(Expression const& expr);
      std::uint32_t compileBinary(BinaryExpression const& binary);
      std::uint32_t compileUnary(UnaryExpression const& unary);
      std::uint32_t compileExists(Expression const& operand);
      CompiledOperand compileFieldOperand(Expression const& expr);
      CompiledOperand compileVariable(VariableExpression const& var);
      std::uint32_t compileComparison(Expression const& lhs, OpCode op, Expression const& rhs);
      CompiledConstant compileConstant(ConstantExpression const& constant, Field field, OpCode op);
      CompiledConstant compileOperandConstant(ConstantExpression const& constant, CompiledOperand left, OpCode op);
      std::uint32_t compileList(ListExpression const& list);
      std::uint32_t compileRange(RangeExpression const& range);
      std::uint32_t compileIn(Expression const& lhs, Expression const& rhs);
      std::uint32_t compileInWithList(Expression const& lhs, ListExpression const& list);
      std::uint32_t compileInRange(Expression const& lhs, RangeExpression const& range);
      std::optional<std::uint32_t> compileInSetList(Expression const& lhs, ListExpression const& list);

      void appendInSetValue(InSet& set, ConstantExpression const& constant, Field field);

      ExecutionPlan _plan;
      std::uint32_t _nextReg = 0;
    };

    OpCode toOpCode(Operator op)
    {
      switch (op)
      {
        case Operator::And: return OpCode::And;
        case Operator::Or: return OpCode::Or;
        case Operator::Not:
        case Operator::Exists: detail::throwQueryError("unary operator used with two operands");
        case Operator::Equal: return OpCode::Eq;
        case Operator::NotEqual: return OpCode::Ne;
        case Operator::Like: return OpCode::Like;
        case Operator::Less: return OpCode::Lt;
        case Operator::LessEqual: return OpCode::Le;
        case Operator::Greater: return OpCode::Gt;
        case Operator::GreaterEqual: return OpCode::Ge;
        case Operator::In: detail::throwQueryError("operator 'in' requires list compilation");
        case Operator::Add:
          detail::throwQueryError("string concatenation is not a query predicate; use it in a format expression");
        default: detail::throwQueryError("unsupported operator");
      }
    }

    OpCode reversedComparison(OpCode op)
    {
      switch (op)
      {
        case OpCode::Lt: return OpCode::Gt;
        case OpCode::Le: return OpCode::Ge;
        case OpCode::Gt: return OpCode::Lt;
        case OpCode::Ge: return OpCode::Le;
        default: return op;
      }
    }

    bool supportsUnicodeCaselessSubstring(Field field)
    {
      return (isStringField(field) && field != Field::Uri) || isDictionaryField(field) || isCreditField(field);
    }

    using RequiredTagNames = boost::unordered_flat_set<std::string>;

    RequiredTagNames requiredTagNames(Expression const& expr);

    RequiredTagNames requiredTagNames(BinaryExpression const& binary)
    {
      auto names = requiredTagNames(binary.operand);

      if (!binary.optOperation)
      {
        return names;
      }

      auto const rhsNames = requiredTagNames(binary.optOperation->operand);

      switch (binary.optOperation->op)
      {
        case Operator::And: names.insert(rhsNames.begin(), rhsNames.end()); return names;

        // OR can only require tags that are shared by every matching branch.
        case Operator::Or:
        {
          auto intersection = RequiredTagNames{};

          auto const& smaller = names.size() <= rhsNames.size() ? names : rhsNames;
          auto const& larger = names.size() <= rhsNames.size() ? rhsNames : names;
          intersection.reserve(smaller.size());

          for (auto const& name : smaller)
          {
            if (larger.contains(name))
            {
              intersection.insert(name);
            }
          }

          return intersection;
        }

        default: return {};
      }
    }

    RequiredTagNames requiredTagNames(UnaryExpression const& unary)
    {
      if (unary.op == Operator::Not)
      {
        return {};
      }

      return requiredTagNames(unary.operand);
    }

    RequiredTagNames requiredTagNames(Expression const& expr)
    {
      return std::visit(utility::makeVisitor(
                          [](VariableExpression const& variable)
                          {
                            if (variable.type != VariableType::Tag)
                            {
                              return RequiredTagNames{};
                            }

                            auto names = RequiredTagNames{};
                            names.emplace(variable.name);
                            return names;
                          },
                          [](ConstantExpression const&) { return RequiredTagNames{}; },
                          [](ListExpression const&) { return RequiredTagNames{}; },
                          [](RangeExpression const&) { return RequiredTagNames{}; },
                          [](std::unique_ptr<BinaryExpression> const& binaryPtr)
                          {
                            if (!binaryPtr)
                            {
                              return RequiredTagNames{};
                            }

                            return requiredTagNames(*binaryPtr);
                          },
                          [](std::unique_ptr<UnaryExpression> const& unaryPtr)
                          {
                            if (!unaryPtr)
                            {
                              return RequiredTagNames{};
                            }

                            return requiredTagNames(*unaryPtr);
                          }),
                        expr);
    }

    void collectTagNamesInSourceOrder(Expression const& expr,
                                      RequiredTagNames const& required,
                                      RequiredTagNames& seen,
                                      std::vector<std::string>& ordered);

    void collectTagNamesInSourceOrder(BinaryExpression const& binary,
                                      RequiredTagNames const& required,
                                      RequiredTagNames& seen,
                                      std::vector<std::string>& ordered)
    {
      collectTagNamesInSourceOrder(binary.operand, required, seen, ordered);

      if (binary.optOperation)
      {
        collectTagNamesInSourceOrder(binary.optOperation->operand, required, seen, ordered);
      }
    }

    void collectTagNamesInSourceOrder(UnaryExpression const& unary,
                                      RequiredTagNames const& required,
                                      RequiredTagNames& seen,
                                      std::vector<std::string>& ordered)
    {
      collectTagNamesInSourceOrder(unary.operand, required, seen, ordered);
    }

    void collectTagNamesInSourceOrder(Expression const& expr,
                                      RequiredTagNames const& required,
                                      RequiredTagNames& seen,
                                      std::vector<std::string>& ordered)
    {
      std::visit(utility::makeVisitor(
                   [&required, &seen, &ordered](VariableExpression const& variable)
                   {
                     if (variable.type == VariableType::Tag && required.contains(variable.name) &&
                         seen.emplace(variable.name).second)
                     {
                       ordered.emplace_back(variable.name);
                     }
                   },
                   [](ConstantExpression const&) {},
                   [](ListExpression const&) {},
                   [](RangeExpression const&) {},
                   [&required, &seen, &ordered](std::unique_ptr<BinaryExpression> const& binaryPtr)
                   {
                     if (binaryPtr)
                     {
                       collectTagNamesInSourceOrder(*binaryPtr, required, seen, ordered);
                     }
                   },
                   [&required, &seen, &ordered](std::unique_ptr<UnaryExpression> const& unaryPtr)
                   {
                     if (unaryPtr)
                     {
                       collectTagNamesInSourceOrder(*unaryPtr, required, seen, ordered);
                     }
                   }),
                 expr);
    }

    std::vector<std::string> requiredTagNamesInSourceOrder(Expression const& expr)
    {
      auto const required = requiredTagNames(expr);
      auto seen = RequiredTagNames{};
      auto ordered = std::vector<std::string>{};
      seen.reserve(required.size());
      ordered.reserve(required.size());
      collectTagNamesInSourceOrder(expr, required, seen, ordered);
      return ordered;
    }

    VariableExpression const* bareNonTagVariableInPredicatePosition(Expression const& expr);

    VariableExpression const* bareNonTagVariableInPredicatePosition(BinaryExpression const& binary)
    {
      if (!binary.optOperation)
      {
        return bareNonTagVariableInPredicatePosition(binary.operand);
      }

      if (binary.optOperation->op == Operator::And || binary.optOperation->op == Operator::Or)
      {
        if (auto const* variable = bareNonTagVariableInPredicatePosition(binary.operand); variable != nullptr)
        {
          return variable;
        }

        return bareNonTagVariableInPredicatePosition(binary.optOperation->operand);
      }

      return nullptr;
    }

    VariableExpression const* bareNonTagVariableInPredicatePosition(UnaryExpression const& unary)
    {
      if (unary.op != Operator::Not)
      {
        return nullptr;
      }

      return bareNonTagVariableInPredicatePosition(unary.operand);
    }

    VariableExpression const* bareNonTagVariableInPredicatePosition(Expression const& expr)
    {
      return std::visit(
        utility::makeVisitor([](VariableExpression const& variable) -> VariableExpression const*
                             { return variable.type == VariableType::Tag ? nullptr : &variable; },
                             [](ConstantExpression const&) -> VariableExpression const* { return nullptr; },
                             [](ListExpression const&) -> VariableExpression const* { return nullptr; },
                             [](RangeExpression const&) -> VariableExpression const* { return nullptr; },
                             [](std::unique_ptr<BinaryExpression> const& binaryPtr) -> VariableExpression const*
                             {
                               if (!binaryPtr)
                               {
                                 return nullptr;
                               }

                               return bareNonTagVariableInPredicatePosition(*binaryPtr);
                             },
                             [](std::unique_ptr<UnaryExpression> const& unaryPtr) -> VariableExpression const*
                             {
                               if (!unaryPtr)
                               {
                                 return nullptr;
                               }

                               return bareNonTagVariableInPredicatePosition(*unaryPtr);
                             }),
        expr);
    }

    Expression const& unwrappedOperand(Expression const& expr)
    {
      auto const* operand = &expr;

      while (auto const* binary = std::get_if<std::unique_ptr<BinaryExpression>>(operand))
      {
        if (!*binary || (*binary)->optOperation)
        {
          break;
        }

        operand = &(*binary)->operand;
      }

      return *operand;
    }

    bool isStringConstant(ConstantExpression const& constant)
    {
      return std::holds_alternative<std::string>(constant);
    }

    bool isStringConstantOperand(Expression const& expr)
    {
      auto const* constant = std::get_if<ConstantExpression>(&expr);
      return constant != nullptr && isStringConstant(*constant);
    }

    bool shouldReverseTextComparison(Expression const& lhs, OpCode opcode, Expression const& rhs)
    {
      auto const* variable = std::get_if<VariableExpression>(&rhs);

      if (opcode == OpCode::Like || variable == nullptr || !isStringConstantOperand(lhs))
      {
        return false;
      }

      auto const optField = detail::lookupVariableField(*variable);

      if (optField == Field::Codec)
      {
        detail::throwQueryError("put @codec on the left when comparing a codec name");
      }

      return optField && fieldKind(*optField) == OperandKind::Text;
    }

    std::optional<std::uint64_t> parseUnsigned(std::string_view value)
    {
      std::uint64_t parsedValue = 0;
      auto const [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsedValue);

      if (ec != std::errc{} || ptr != value.data() + value.size())
      {
        return std::nullopt;
      }

      return parsedValue;
    }

    std::optional<std::uint64_t> checkedMul(std::uint64_t lhs, std::uint64_t rhs)
    {
      if (lhs == 0 || rhs == 0)
      {
        return 0;
      }

      if (lhs > std::numeric_limits<std::uint64_t>::max() / rhs)
      {
        return std::nullopt;
      }

      return lhs * rhs;
    }

    std::optional<std::uint64_t> checkedAdd(std::uint64_t lhs, std::uint64_t rhs)
    {
      if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs)
      {
        return std::nullopt;
      }

      return lhs + rhs;
    }

    std::optional<std::uint64_t> pow10(std::size_t exponent)
    {
      std::uint64_t value = 1;

      for (std::size_t i = 0; i < exponent; ++i)
      {
        auto const optNext = checkedMul(value, 10);

        if (!optNext)
        {
          return std::nullopt;
        }

        value = *optNext;
      }

      return value;
    }

    std::uint64_t unitMultiplier(Field field, std::string_view unit)
    {
      auto const normalized = utility::toLower(unit);

      switch (field)
      {
        case Field::Duration:
        {
          if (auto const* entry = unit_dispatch::Table::lookupUnit(normalized.data(), normalized.size());
              entry != nullptr && entry->durationMultiplier != 0)
          {
            return entry->durationMultiplier;
          }

          break;
        }
        case Field::Bitrate:
        case Field::SampleRate:
        {
          if (auto const* entry = unit_dispatch::Table::lookupUnit(normalized.data(), normalized.size());
              entry != nullptr && entry->scaledMultiplier != 0)
          {
            return entry->scaledMultiplier;
          }

          break;
        }
        default: break;
      }

      detail::throwQueryError("unit '{}' is not supported for {} constants", normalized, fieldDisplayName(field));
    }

    std::uint64_t scaleUnitSegment(std::string_view numberPart,
                                   std::string_view suffixPart,
                                   Field field,
                                   UnitConstantExpression const& constant)
    {
      if (numberPart.empty() || suffixPart.empty())
      {
        detail::throwQueryError("invalid unit literal '{}'", constant.lexeme);
      }

      auto const dotOffset = numberPart.find('.');

      if (dotOffset != std::string_view::npos && numberPart.find('.', dotOffset + 1) != std::string_view::npos)
      {
        detail::throwQueryError("invalid unit literal '{}'", constant.lexeme);
      }

      auto const wholePart = numberPart.substr(0, dotOffset);
      auto const fractionPart =
        dotOffset == std::string_view::npos ? std::string_view{} : numberPart.substr(dotOffset + 1);

      if (wholePart.empty() || (dotOffset != std::string_view::npos && fractionPart.empty()))
      {
        detail::throwQueryError("invalid unit literal '{}'", constant.lexeme);
      }

      auto const optWhole = parseUnsigned(wholePart);
      auto const optFraction =
        fractionPart.empty() ? std::optional<std::uint64_t>{std::uint64_t{0}} : parseUnsigned(fractionPart);
      auto const optDenominator = pow10(fractionPart.size());

      if (!optWhole || !optFraction || !optDenominator)
      {
        detail::throwQueryError("invalid unit literal '{}'", constant.lexeme);
      }

      auto const optScaledWhole = checkedMul(*optWhole, *optDenominator);
      auto const optNumerator = optScaledWhole ? checkedAdd(*optScaledWhole, *optFraction) : std::nullopt;
      auto const multiplier = unitMultiplier(field, suffixPart);

      auto const optScaledNumerator = optNumerator ? checkedMul(*optNumerator, multiplier) : std::nullopt;

      if (!optScaledNumerator)
      {
        detail::throwQueryError("unit literal '{}' is out of range", constant.lexeme);
      }

      if (*optScaledNumerator % *optDenominator != 0)
      {
        detail::throwQueryError(
          "unit literal '{}' does not resolve to an integer {} value", constant.lexeme, fieldDisplayName(field));
      }

      return *optScaledNumerator / *optDenominator;
    }

    std::int64_t scaleUnitConstant(UnitConstantExpression const& constant, Field field)
    {
      if (field == kNoFieldContext)
      {
        detail::throwQueryError(
          "unit literal '{}' requires @duration, @bitrate or @sampleRate on the left", constant.lexeme);
      }

      auto lexeme = std::string_view{constant.lexeme};

      auto const negative = !lexeme.empty() && lexeme.front() == '-';

      if (negative)
      {
        lexeme.remove_prefix(1);
      }

      if (lexeme.empty())
      {
        detail::throwQueryError("invalid unit literal '{}'", constant.lexeme);
      }

      std::uint64_t total = 0;
      std::uint32_t segmentCount = 0;

      while (!lexeme.empty())
      {
        // NOLINTNEXTLINE(readability-qualified-auto) -- string_view iterator representation is library-specific.
        auto const suffixStart = std::ranges::find_if(lexeme, utility::isAsciiAlpha);

        if (suffixStart == lexeme.end())
        {
          detail::throwQueryError("invalid unit literal '{}'", constant.lexeme);
        }

        auto const suffixOffset = static_cast<std::size_t>(std::distance(lexeme.begin(), suffixStart));
        auto const numberPart = lexeme.substr(0, suffixOffset);
        auto const suffixAndRest = lexeme.substr(suffixOffset);
        // NOLINTNEXTLINE(readability-qualified-auto) -- string_view iterator representation is library-specific.
        auto const nextNumber = std::ranges::find_if(suffixAndRest, utility::isAsciiDigit);
        auto const suffixSize = static_cast<std::size_t>(std::distance(suffixAndRest.begin(), nextNumber));
        auto const suffixPart = suffixAndRest.substr(0, suffixSize);
        auto const segmentValue = scaleUnitSegment(numberPart, suffixPart, field, constant);
        auto const optTotal = checkedAdd(total, segmentValue);

        if (!optTotal)
        {
          detail::throwQueryError("unit literal '{}' is out of range", constant.lexeme);
        }

        total = *optTotal;
        segmentCount++;
        lexeme = suffixAndRest.substr(suffixSize);
      }

      if (segmentCount > 1 && field != Field::Duration)
      {
        detail::throwQueryError("compound unit literal '{}' is only supported for duration constants", constant.lexeme);
      }

      if (!negative)
      {
        if (total > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
          detail::throwQueryError("unit literal '{}' is out of range", constant.lexeme);
        }

        return static_cast<std::int64_t>(total);
      }

      auto const negativeLimit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;

      if (total > negativeLimit)
      {
        detail::throwQueryError("unit literal '{}' is out of range", constant.lexeme);
      }

      if (total == negativeLimit)
      {
        return std::numeric_limits<std::int64_t>::min();
      }

      return -static_cast<std::int64_t>(total);
    }
  } // namespace

  std::uint32_t QueryCompiler::addStringConstant(std::string_view str, Field field, OpCode op)
  {
    auto prepared = std::string{};

    if (op == OpCode::Like && field != Field::Uri)
    {
      prepared = makeQueryCaselessKey(str, "Unicode caseless query literal");
    }
    else
    {
      // URI values preserve the byte identity of the filesystem namespace. They
      // must never pass through user-visible text normalization.
      prepared = field == Field::Uri ? std::string{str} : normalizeQueryText(str, "Query string literal");
    }

    if (auto const it = std::ranges::find(_plan.stringConstants, prepared); it != _plan.stringConstants.end())
    {
      return static_cast<std::uint32_t>(std::distance(_plan.stringConstants.begin(), it));
    }

    _plan.stringConstants.emplace_back(std::move(prepared));

    return static_cast<std::uint32_t>(_plan.stringConstants.size() - 1);
  }

  std::uint32_t QueryCompiler::addDictionarySymbol(std::string_view text)
  {
    auto normalized = normalizeQueryText(text, "Query dictionary literal");

    if (auto const it = std::ranges::find(_plan.dictionarySymbols, normalized); it != _plan.dictionarySymbols.end())
    {
      return static_cast<std::uint32_t>(std::distance(_plan.dictionarySymbols.begin(), it));
    }

    _plan.dictionarySymbols.emplace_back(std::move(normalized));
    return static_cast<std::uint32_t>(_plan.dictionarySymbols.size() - 1);
  }

  std::uint32_t QueryCompiler::addInSet(InSet set)
  {
    if (set.valueKind == InSetValueKind::String)
    {
      std::ranges::sort(set.strings);
      auto const last = std::ranges::unique(set.strings).begin();
      set.strings.erase(last, set.strings.end());
    }

    _plan.inSets.emplace_back(std::move(set));
    return static_cast<std::uint32_t>(_plan.inSets.size() - 1);
  }

  std::uint32_t QueryCompiler::pushReg()
  {
    return _nextReg++;
  }

  void QueryCompiler::popReg(std::uint32_t top)
  {
    // Only the current top register may be freed; this enforces the stack discipline the
    // evaluator relies on (a binary op's left operand sits immediately below its right).
    AO_INVARIANT(top + 1 == _nextReg);
    --_nextReg;
  }

  std::uint32_t QueryCompiler::compileExpression(Expression const& expr)
  {
    return std::visit(utility::makeVisitor(
                        [this](std::unique_ptr<BinaryExpression> const& binaryPtr) -> std::uint32_t
                        {
                          if (!binaryPtr)
                          {
                            detail::throwQueryError("missing binary expression");
                          }

                          return compileBinary(*binaryPtr);
                        },
                        [this](std::unique_ptr<UnaryExpression> const& unaryPtr) -> std::uint32_t
                        {
                          if (!unaryPtr)
                          {
                            detail::throwQueryError("missing unary expression");
                          }

                          return compileUnary(*unaryPtr);
                        },
                        [this](VariableExpression const& var) -> std::uint32_t { return compileVariable(var).reg; },
                        [this](ConstantExpression const& constant) -> std::uint32_t
                        { return compileConstant(constant, kNoFieldContext, OpCode::LoadConstant).reg; },
                        [this](ListExpression const& list) -> std::uint32_t { return compileList(list); },
                        [this](RangeExpression const& range) -> std::uint32_t { return compileRange(range); }),
                      expr);
  }

  std::uint32_t QueryCompiler::compilePredicate(Expression const& expr)
  {
    if (auto const* var = bareNonTagVariableInPredicatePosition(expr); var != nullptr)
    {
      auto const name = variableDisplayName(*var);
      detail::throwQueryError(
        "bare field '{}' is not a predicate; use '{}?' for existence, '!{}?' for missing, or compare it "
        "explicitly",
        name,
        name,
        name);
    }

    if (!detail::isPredicateExpression(expr))
    {
      detail::throwQueryError("query expression is not a predicate");
    }

    return compileExpression(expr);
  }

  std::uint32_t QueryCompiler::compileBinary(BinaryExpression const& binary)
  {
    if (!binary.optOperation)
    {
      return compilePredicate(binary.operand);
    }

    if (binary.optOperation->op == Operator::In)
    {
      return compileIn(unwrappedOperand(binary.operand), unwrappedOperand(binary.optOperation->operand));
    }

    if (binary.optOperation->op == Operator::And || binary.optOperation->op == Operator::Or)
    {
      auto const leftReg = compilePredicate(binary.operand);
      auto const rightReg = compilePredicate(binary.optOperation->operand);
      auto const opcode = toOpCode(binary.optOperation->op);

      // And/Or consume the top two results (left immediately below right) and write the
      // result back into the left register.
      AO_INVARIANT(rightReg == leftReg + 1);
      _plan.instructions.push_back(Instruction{
        .op = opcode,
        .field = 0,
        .operand = static_cast<std::int32_t>(rightReg),
      });
      popReg(rightReg);
      return leftReg;
    }

    return compileComparison(unwrappedOperand(binary.operand),
                             toOpCode(binary.optOperation->op),
                             unwrappedOperand(binary.optOperation->operand));
  }

  std::uint32_t QueryCompiler::compileComparison(Expression const& lhs, OpCode opcode, Expression const& rhs)
  {
    // For equality and ordering, bind and normalize a string literal in the direct text field's context.
    if (shouldReverseTextComparison(lhs, opcode, rhs))
    {
      return compileComparison(rhs, reversedComparison(opcode), lhs);
    }

    auto const left = compileFieldOperand(lhs);
    auto const leftField = left.field;
    auto const* literal = std::get_if<ConstantExpression>(&rhs);

    if ((leftField == Field::RecordingDate || isCreditField(leftField)) && literal == nullptr)
    {
      detail::throwQueryError("field '{}' requires a literal operand", fieldDisplayName(leftField));
    }

    if (leftField == Field::RecordingDate && opcode == OpCode::Like)
    {
      detail::throwQueryError("recordingDate does not support substring matching");
    }

    if (isCreditField(leftField) && opcode != OpCode::Eq && opcode != OpCode::Ne && opcode != OpCode::Like)
    {
      detail::throwQueryError("credit members only support equality, inequality, substring, lists, and existence");
    }

    if (opcode == OpCode::Like && !supportsUnicodeCaselessSubstring(leftField) && leftField != Field::Uri)
    {
      detail::throwQueryError("operator '~' requires a text field or a credit member field on the left");
    }

    if (opcode == OpCode::Like && !isStringConstantOperand(rhs))
    {
      detail::throwQueryError("operator '~' requires a string operand");
    }

    auto comparison = Instruction{
      .op = opcode, .field = static_cast<std::uint8_t>(leftField), .dictionarySymbol = left.dictionarySymbol};
    std::uint32_t rightReg = 0;

    if (literal != nullptr)
    {
      requireCompatibleConstant(lhs, left.kind, leftField, *literal);
      auto const compiled = compileOperandConstant(*literal, left, opcode);
      rightReg = compiled.reg;

      if (isDictionaryField(leftField) || isCreditField(leftField))
      {
        comparison.dictionarySymbol = compiled.dictionarySymbol;
      }

      if (left.kind == OperandKind::Text && leftField == kTextLiteralField)
      {
        comparison.op = OpCode::CompareText;
        comparison.textRelation = opcode;
      }
    }
    else
    {
      auto const right = compileFieldOperand(rhs);
      rightReg = right.reg;

      if (right.kind != left.kind)
      {
        detail::throwQueryError("incompatible predicate operands: {} ({}) and {} ({})",
                                operandLabel(lhs),
                                operandKindName(left.kind),
                                operandLabel(rhs),
                                operandKindName(right.kind));
      }

      // Compare shared dictionary IDs directly for Eq/Ne: two missing IDs are equal.
      // Mixed text equality instead requires two present values.
      if (left.kind == OperandKind::Text &&
          (isOrderedComparison(opcode) || !isDictionaryField(leftField) || !isDictionaryField(right.field)))
      {
        comparison.op = OpCode::CompareText;
        comparison.textRelation = opcode;
        comparison.rightTextField = right.field;
        comparison.rightDictionarySymbol = right.dictionarySymbol;
      }
    }

    // Carry the left field and relevant dictionary symbol directly on the comparison so the
    // evaluator resolves the operand's type without scanning back for the LoadField. The
    // comparison consumes the top two results and writes the result into the left register.
    AO_INVARIANT(rightReg == left.reg + 1);
    comparison.operand = static_cast<std::int32_t>(rightReg);
    _plan.instructions.push_back(comparison);
    popReg(rightReg);
    return left.reg;
  }

  std::uint32_t QueryCompiler::compileUnary(UnaryExpression const& unary)
  {
    if (unary.op == Operator::Exists)
    {
      return compileExists(unary.operand);
    }

    if (unary.op == Operator::Not)
    {
      auto const reg = compilePredicate(unary.operand);

      // Not rewrites its operand in place; the result stays in the same register.
      _plan.instructions.push_back(Instruction{
        .op = OpCode::Not,
        .operand = static_cast<std::int32_t>(reg),
      });
      return reg;
    }

    detail::throwQueryError("unsupported unary operator");
  }

  std::uint32_t QueryCompiler::compileExists(Expression const& operand)
  {
    auto const* var = std::get_if<VariableExpression>(&operand);

    if (var == nullptr)
    {
      detail::throwQueryError("operator '?' requires a field operand");
    }

    auto const fieldRes = detail::resolveVariableField(*var);

    if (!fieldRes)
    {
      detail::throwQueryError(fieldRes.error());
    }

    auto const field = *fieldRes;

    _plan.accessProfile = combineAccessProfiles(_plan.accessProfile, accessProfile(field));

    auto dictionarySymbol = kNoDictionarySymbol;

    if (var->type == VariableType::Custom || var->type == VariableType::Tag)
    {
      dictionarySymbol = addDictionarySymbol(var->name);
      _plan.requiresDictionary = true;
    }

    auto const reg = pushReg();
    _plan.instructions.push_back(Instruction{
      .op = OpCode::Exists,
      .field = static_cast<std::uint8_t>(field),
      .operand = static_cast<std::int32_t>(reg),
      .dictionarySymbol = dictionarySymbol,
    });

    return reg;
  }

  QueryCompiler::CompiledOperand QueryCompiler::compileFieldOperand(Expression const& expr)
  {
    if (auto const* variable = std::get_if<VariableExpression>(&expr); variable != nullptr)
    {
      return compileVariable(*variable);
    }

    if (auto const* literal = std::get_if<ConstantExpression>(&expr); literal != nullptr)
    {
      auto const kind = constantKind(*literal);
      auto const field = kind == OperandKind::Text ? kTextLiteralField : kNoFieldContext;
      return {.reg = compileConstant(*literal, field, OpCode::LoadConstant).reg, .kind = kind, .field = field};
    }

    // Only predicates produce nested values. Let the owning compiler reject
    // lists, ranges and concatenation with their operand-specific diagnostics.
    return {.reg = compileExpression(expr), .kind = OperandKind::Boolean};
  }

  QueryCompiler::CompiledOperand QueryCompiler::compileVariable(VariableExpression const& var)
  {
    if (var.type == VariableType::Tag)
    {
      _plan.accessProfile = combineAccessProfiles(_plan.accessProfile, accessProfile(Field::Tag));

      auto const dictionarySymbol = addDictionarySymbol(var.name);
      _plan.requiresDictionary = true;

      // Generate implicit tag comparison: track.tags().has(bound tag id).
      auto const fieldReg = pushReg();
      _plan.instructions.push_back(Instruction{
        .op = OpCode::LoadField,
        .field = static_cast<std::uint8_t>(Field::Tag),
        .operand = static_cast<std::int32_t>(fieldReg),
      });

      auto const constReg = pushReg();
      _plan.instructions.push_back(Instruction{
        .op = OpCode::LoadConstant,
        .field = 0,
        .operand = static_cast<std::int32_t>(constReg),
      });

      _plan.instructions.push_back(Instruction{
        .op = OpCode::Eq,
        .field = static_cast<std::uint8_t>(Field::Tag),
        .operand = static_cast<std::int32_t>(constReg),
        .dictionarySymbol = dictionarySymbol,
      });

      popReg(constReg);
      return {.reg = fieldReg, .kind = OperandKind::Boolean};
    }

    auto const fieldRes = detail::resolveVariableField(var);

    if (!fieldRes)
    {
      detail::throwQueryError(fieldRes.error());
    }

    auto const field = *fieldRes;

    _plan.accessProfile = combineAccessProfiles(_plan.accessProfile, accessProfile(field));

    auto dictionarySymbol = kNoDictionarySymbol;

    if (var.type == VariableType::Custom)
    {
      dictionarySymbol = addDictionarySymbol(var.name);
      _plan.requiresDictionary = true;
    }
    else if (isDictionaryField(field) || isCreditField(field))
    {
      _plan.requiresDictionary = true;
    }

    auto const reg = pushReg();
    _plan.instructions.push_back(Instruction{
      .op = OpCode::LoadField,
      .field = static_cast<std::uint8_t>(field),
      .operand = static_cast<std::int32_t>(reg),
      .dictionarySymbol = dictionarySymbol,
    });

    return {.reg = reg, .kind = fieldKind(field), .field = field, .dictionarySymbol = dictionarySymbol};
  }

  QueryCompiler::CompiledConstant QueryCompiler::compileConstant(ConstantExpression const& constant,
                                                                 Field field,
                                                                 OpCode op)
  {
    auto dictionarySymbol = kNoDictionarySymbol;

    auto const value =
      std::visit(utility::makeVisitor(
                   [](bool val) -> std::int64_t { return val ? 1 : 0; },
                   [](std::int64_t val) { return val; },
                   [field](UnitConstantExpression const& val) { return scaleUnitConstant(val, field); },
                   [this, field, op, &dictionarySymbol](std::string const& val) -> std::int64_t
                   {
                     if (field == Field::Codec)
                     {
                       if (auto const optCodec = parseAudioCodecName(val); optCodec)
                       {
                         return audioCodecStorageValue(*optCodec);
                       }

                       detail::throwQueryError("unknown audio codec '{}'", val);
                     }

                     if ((isDictionaryField(field) || isCreditField(field)) && (op == OpCode::Eq || op == OpCode::Ne))
                     {
                       dictionarySymbol = addDictionarySymbol(val);
                       return 0;
                     }

                     return addStringConstant(val, field, op);
                   }),
                 constant);
    auto const reg = pushReg();
    _plan.instructions.push_back(
      Instruction{.op = OpCode::LoadConstant, .operand = static_cast<std::int32_t>(reg), .constValue = value});
    return CompiledConstant{.reg = reg, .dictionarySymbol = dictionarySymbol};
  }

  QueryCompiler::CompiledConstant QueryCompiler::compileOperandConstant(ConstantExpression const& constant,
                                                                        CompiledOperand left,
                                                                        OpCode op)
  {
    if (left.kind != OperandKind::RecordingDate)
    {
      return compileConstant(constant, left.field, op);
    }

    constexpr std::int64_t kMaxRecordingYear = 9999;
    auto date = library::RecordingDate{};

    if (auto const* text = std::get_if<std::string>(&constant); text != nullptr)
    {
      auto const dateRes = library::parseRecordingDate(*text);

      if (!dateRes)
      {
        detail::throwQueryError("invalid recordingDate literal: {}", dateRes.error().message);
      }

      date = *dateRes;
    }
    else if (auto const* year = std::get_if<std::int64_t>(&constant);
             year != nullptr && *year >= 1 && *year <= kMaxRecordingYear)
    {
      date.year = static_cast<std::uint16_t>(*year);
    }
    else
    {
      detail::throwQueryError("recordingDate requires a canonical quoted date or integer year 1..9999");
    }

    auto const index = _plan.recordingDateConstants.size();
    _plan.recordingDateConstants.push_back(date);
    auto const reg = pushReg();
    _plan.instructions.push_back(Instruction{
      .op = OpCode::LoadConstant,
      .operand = static_cast<std::int32_t>(reg),
      .constValue = static_cast<std::int64_t>(index),
    });
    return CompiledConstant{.reg = reg};
  }

  std::uint32_t QueryCompiler::compileList(ListExpression const& /*list*/)
  {
    detail::throwQueryError("list expressions are only supported as the right operand of 'in'");
  }

  std::uint32_t QueryCompiler::compileRange(RangeExpression const& /*range*/)
  {
    detail::throwQueryError("range expressions are only supported as the right operand of 'in'");
  }

  std::uint32_t QueryCompiler::compileIn(Expression const& lhs, Expression const& rhs)
  {
    if (auto const* list = std::get_if<ListExpression>(&rhs); list != nullptr)
    {
      return compileInWithList(lhs, *list);
    }

    auto const* range = std::get_if<RangeExpression>(&rhs);

    if (range == nullptr)
    {
      detail::throwQueryError("operator 'in' expects a list or range right operand");
    }

    return compileInRange(lhs, *range);
  }

  std::uint32_t QueryCompiler::compileInWithList(Expression const& lhs, ListExpression const& list)
  {
    if (list.values.empty())
    {
      detail::throwQueryError("operator 'in' expects a non-empty list");
    }

    // Use a membership set for eligible fields even for a single item.
    if (auto const optCompiled = compileInSetList(lhs, list); optCompiled)
    {
      return *optCompiled;
    }

    // Not eligible for set compilation: expand into a chain of OR-ed equalities,
    // accumulating the running result in the register of the first comparison.
    auto optAccumReg = std::optional<std::uint32_t>{};

    for (auto const& value : list.values)
    {
      auto const leftReg = compileComparison(lhs, OpCode::Eq, value);

      if (!optAccumReg)
      {
        optAccumReg = leftReg;
        continue;
      }

      // OR the new comparison (the top register) into the accumulator just below it.
      AO_INVARIANT(leftReg == *optAccumReg + 1);
      _plan.instructions.push_back(Instruction{
        .op = OpCode::Or,
        .field = 0,
        .operand = static_cast<std::int32_t>(leftReg),
      });
      popReg(leftReg); // Or result is now in *optAccumReg
    }

    AO_INVARIANT(optAccumReg); // non-empty list guarantees at least one comparison
    return *optAccumReg;
  }

  std::uint32_t QueryCompiler::compileInRange(Expression const& lhs, RangeExpression const& range)
  {
    auto const geReg = compileComparison(lhs, OpCode::Ge, range.lower);
    auto const leReg = compileComparison(lhs, OpCode::Le, range.upper);

    // AND the two bounds together; the result lands in the Ge register.
    AO_INVARIANT(leReg == geReg + 1);
    _plan.instructions.push_back(Instruction{
      .op = OpCode::And,
      .field = 0,
      .operand = static_cast<std::int32_t>(leReg),
    });
    popReg(leReg); // And result is now in geReg

    return geReg;
  }

  std::optional<std::uint32_t> QueryCompiler::compileInSetList(Expression const& lhs, ListExpression const& list)
  {
    auto const* variable = std::get_if<VariableExpression>(&lhs);

    if (variable == nullptr)
    {
      return std::nullopt;
    }

    auto const fieldRes = detail::resolveVariableField(*variable);

    if (!fieldRes)
    {
      detail::throwQueryError(fieldRes.error());
    }

    auto const field = *fieldRes;

    if (isTagField(field) || field == Field::RecordingDate)
    {
      return std::nullopt;
    }

    auto set = InSet{};

    if (isDictionaryField(field) || isCreditField(field))
    {
      set.valueKind = InSetValueKind::Dictionary;
    }
    else if (isStringField(field))
    {
      set.valueKind = InSetValueKind::String;
    }

    for (auto const& value : list.values)
    {
      requireCompatibleConstant(lhs, fieldKind(field), field, value);
      appendInSetValue(set, value, field);
    }

    auto const left = compileVariable(*variable);
    auto const leftReg = left.reg;
    auto const setIndex = addInSet(std::move(set));
    // InSet rewrites its operand in place, so the result stays in the loaded register.
    // It spends constValue on the set index; the Custom key is a plan-owned symbol.
    _plan.instructions.push_back(Instruction{
      .op = OpCode::InSet,
      .field = static_cast<std::uint8_t>(field),
      .operand = static_cast<std::int32_t>(leftReg),
      .constValue = static_cast<std::int64_t>(setIndex),
      .dictionarySymbol = left.dictionarySymbol,
    });

    return std::optional<std::uint32_t>{leftReg};
  }

  void QueryCompiler::appendInSetValue(InSet& set, ConstantExpression const& constant, Field field)
  {
    std::visit(utility::makeVisitor([&set](bool value) { set.numericValues.insert(value ? 1 : 0); },
                                    [&set](std::int64_t value) { set.numericValues.insert(value); },
                                    [&set, field](UnitConstantExpression const& value)
                                    { set.numericValues.insert(scaleUnitConstant(value, field)); },
                                    [this, &set, field](std::string const& value)
                                    {
                                      if (set.valueKind == InSetValueKind::String)
                                      {
                                        set.strings.push_back(field == Field::Uri
                                                                ? value
                                                                : normalizeQueryText(value, "Query string literal"));
                                        return;
                                      }

                                      if (field == Field::Codec)
                                      {
                                        if (auto const optCodec = parseAudioCodecName(value); optCodec)
                                        {
                                          set.numericValues.insert(audioCodecStorageValue(*optCodec));
                                          return;
                                        }

                                        detail::throwQueryError("unknown audio codec '{}'", value);
                                      }

                                      AO_INVARIANT(set.valueKind == InSetValueKind::Dictionary);
                                      set.dictionarySymbols.push_back(addDictionarySymbol(value));
                                    }),
               constant);
  }

  Result<ExecutionPlan> QueryCompiler::compile(Expression const& expr)

  try
  {
    _plan.accessProfile = AccessProfile::NoTrackData;

    for (auto const& tagName : requiredTagNamesInSourceOrder(expr))
    {
      _plan.requiredTagSymbols.push_back(addDictionarySymbol(tagName));
    }

    // Check if the expression is a constant "true"
    if (auto const* constant = std::get_if<ConstantExpression>(&expr); constant != nullptr)
    {
      if (bool const* val = std::get_if<bool>(constant); val != nullptr)
      {
        if (*val)
        {
          _plan.matchesAll = true;
        }
      }
    }

    compilePredicate(expr);

    return std::move(_plan);
  }
  catch (detail::QueryException const& ex)
  {
    return std::unexpected{ex.error()};
  }

  AccessProfile accessProfile(ExecutionPlan const& plan) noexcept
  {
    return plan.accessProfile;
  }

  Result<ExecutionPlan> compileQuery(Expression const& expr)
  {
    return QueryCompiler{}.compile(expr);
  }
} // namespace ao::query
