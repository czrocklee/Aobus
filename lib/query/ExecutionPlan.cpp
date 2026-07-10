// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/QueryCompiler.h>
#include <ao/query/detail/Bytecode.h>
#include <ao/query/detail/FieldResolver.h>
#include <ao/query/detail/Predicate.h>
#include <ao/query/detail/QueryError.h>
#include <ao/utility/String.h>
#include <ao/utility/VariantVisitor.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
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
    // Bloom filter uses 5 bits per tag (bit mask 31 = 0x1F)
    constexpr std::uint32_t kBloomBitMask = 31;
    // The Phase 0 Query IN threshold sweep shows the membership plan is faster
    // than repeated field-load/equality expansion even for a one-item list.
    constexpr std::size_t kInSetCompilationThreshold = 1;

    OpCode toOpCode(Operator op)
    {
      switch (op)
      {
        case Operator::And: return OpCode::And;
        case Operator::Or: return OpCode::Or;
        case Operator::Not: return OpCode::Not;
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
        case Operator::Exists: return OpCode::Exists;
        default: detail::throwQueryError("unsupported operator");
      }
    }

    bool isUnsupportedLikeField(Field field)
    {
      return field == Field::CoverArtId || field == Field::Tag;
    }

    std::uint32_t tagBloomBit(library::DictionaryStore* dictionary, std::string_view tagName)
    {
      if (dictionary == nullptr)
      {
        return 0;
      }

      auto const tagId = dictionary->getOrIntern(tagName);
      return std::uint32_t{1} << (tagId.raw() & kBloomBitMask);
    }

    std::uint32_t computeRequiredTagBloomMask(Expression const& expr, library::DictionaryStore* dictionary);

    std::uint32_t computeRequiredTagBloomMask(BinaryExpression const& binary, library::DictionaryStore* dictionary)
    {
      auto const lhsMask = computeRequiredTagBloomMask(binary.operand, dictionary);

      if (!binary.optOperation)
      {
        return lhsMask;
      }

      auto const rhsMask = computeRequiredTagBloomMask(binary.optOperation->operand, dictionary);

      switch (binary.optOperation->op)
      {
        case Operator::And: return lhsMask | rhsMask;

        // OR can only require tags that are shared by every matching branch.
        case Operator::Or: return lhsMask & rhsMask;

        default: return 0;
      }
    }

    std::uint32_t computeRequiredTagBloomMask(UnaryExpression const& unary, library::DictionaryStore* dictionary)
    {
      if (unary.op == Operator::Not)
      {
        return 0;
      }

      return computeRequiredTagBloomMask(unary.operand, dictionary);
    }

    std::uint32_t computeRequiredTagBloomMask(Expression const& expr, library::DictionaryStore* dictionary)
    {
      return std::visit(utility::makeVisitor(
                          [dictionary](VariableExpression const& variable)
                          {
                            if (variable.type != VariableType::Tag)
                            {
                              return std::uint32_t{0};
                            }

                            return tagBloomBit(dictionary, variable.name);
                          },
                          [](ConstantExpression const&) { return std::uint32_t{0}; },
                          [](ListExpression const&) { return std::uint32_t{0}; },
                          [](RangeExpression const&) { return std::uint32_t{0}; },
                          [dictionary](std::unique_ptr<BinaryExpression> const& binaryPtr)
                          {
                            if (!binaryPtr)
                            {
                              return std::uint32_t{0};
                            }

                            return computeRequiredTagBloomMask(*binaryPtr, dictionary);
                          },
                          [dictionary](std::unique_ptr<UnaryExpression> const& unaryPtr)
                          {
                            if (!unaryPtr)
                            {
                              return std::uint32_t{0};
                            }

                            return computeRequiredTagBloomMask(*unaryPtr, dictionary);
                          }),
                        expr);
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

    bool isStringConstant(ConstantExpression const& constant)
    {
      return std::holds_alternative<std::string>(constant);
    }

    bool isStringConstantOperand(Expression const& expr)
    {
      auto const* constant = std::get_if<ConstantExpression>(&expr);
      return constant != nullptr && isStringConstant(*constant);
    }

    std::optional<std::uint64_t> parseUnsigned(std::string_view value)
    {
      // NOLINTNEXTLINE(misc-const-correctness): std::from_chars writes through this out parameter.
      std::uint64_t parsedValue = 0;
      auto const [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsedValue);

      if (ec != std::errc{} || ptr != value.data() + value.size())
      {
        return std::nullopt;
      }

      auto const parsed = parsedValue;
      return parsed;
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
      if (field == Field::TagBloom)
      {
        detail::throwQueryError("unit literal '{}' requires a numeric field context", constant.lexeme);
      }

      auto lexeme = std::string_view{constant.lexeme};

      auto const negative = !lexeme.empty() && lexeme.front() == '-';

      if (negative)
      {
        lexeme.remove_prefix(1);
      }

      std::uint64_t total = 0;
      std::uint32_t segmentCount = 0;

      while (!lexeme.empty())
      {
        // NOLINTNEXTLINE(readability-qualified-auto)
        auto const suffixStart = std::ranges::find_if(lexeme, [](unsigned char ch) { return std::isalpha(ch) != 0; });

        if (suffixStart == lexeme.end())
        {
          detail::throwQueryError("invalid unit literal '{}'", constant.lexeme);
        }

        auto const suffixOffset = static_cast<std::size_t>(std::distance(lexeme.begin(), suffixStart));
        auto const numberPart = lexeme.substr(0, suffixOffset);
        auto const suffixAndRest = lexeme.substr(suffixOffset);
        // NOLINTNEXTLINE(readability-qualified-auto)
        auto const nextNumber =
          std::ranges::find_if(suffixAndRest, [](unsigned char ch) { return std::isdigit(ch) != 0; });
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

  QueryCompiler::QueryCompiler(library::DictionaryStore* dictionary)
    : _dictionary{dictionary}
  {
    gsl_Expects(dictionary != nullptr);
  }

  std::uint32_t QueryCompiler::addStringConstant(std::string_view str)
  {
    if (auto const it = std::ranges::find(_plan.stringConstants, str); it != _plan.stringConstants.end())
    {
      return static_cast<std::uint32_t>(std::distance(_plan.stringConstants.begin(), it));
    }

    _plan.stringConstants.emplace_back(str);

    return static_cast<std::uint32_t>(_plan.stringConstants.size() - 1);
  }

  std::uint32_t QueryCompiler::addInSet(InSet set)
  {
    if (set.stringValues)
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
    gsl_Expects(top + 1 == _nextReg);
    --_nextReg;
  }

  std::uint32_t QueryCompiler::compileExpression(Expression const& expr)
  {
    return std::visit(
      utility::makeVisitor(
        [this](std::unique_ptr<BinaryExpression> const& binaryPtr) -> std::uint32_t
        {
          gsl_Expects(binaryPtr != nullptr);
          return compileBinary(*binaryPtr);
        },
        [this](std::unique_ptr<UnaryExpression> const& unaryPtr) -> std::uint32_t
        {
          gsl_Expects(unaryPtr != nullptr);
          return compileUnary(*unaryPtr);
        },
        [this](VariableExpression const& var) -> std::uint32_t { return compileVariable(var); },
        [this](ConstantExpression const& constant) -> std::uint32_t { return compileConstant(constant); },
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
      return compileIn(binary.operand, binary.optOperation->operand);
    }

    if (binary.optOperation->op == Operator::And || binary.optOperation->op == Operator::Or)
    {
      auto const leftReg = compilePredicate(binary.operand);
      auto const rightReg = compilePredicate(binary.optOperation->operand);
      auto const opcode = toOpCode(binary.optOperation->op);

      // And/Or consume the top two results (left immediately below right) and write the
      // result back into the left register.
      gsl_Expects(rightReg == leftReg + 1);
      _plan.instructions.push_back(Instruction{
        .op = opcode,
        .field = 0,
        .operand = static_cast<std::int32_t>(rightReg),
        .constValue = 0,
        .size = 0,
        .data = nullptr,
      });
      popReg(rightReg);
      return leftReg;
    }

    // Comparison (Eq/Ne/Lt/Le/Gt/Ge/Like). Compile the left operand first.
    auto const leftReg = compileExpression(binary.operand);

    // Save the left field (and its Custom dictionaryId) before compiling the right operand,
    // which overwrites _lastField.
    auto const leftField = _lastField;
    auto const leftCustomId = _lastFieldCustomId;
    auto const opcode = toOpCode(binary.optOperation->op);

    if (opcode == OpCode::Like && isUnsupportedLikeField(leftField))
    {
      detail::throwQueryError("LIKE operator not supported for coverArt or tags");
    }

    // Dictionary fields store interned IDs, so an ordered comparison (<, <=, >, >=)
    // must compare the resolved text rather than the ID. Require a string operand
    // and keep it as a string constant (like LIKE) so the evaluator resolves the
    // field's ID back to text at compare time.
    if (isOrderedComparison(opcode) && isDictionaryField(leftField) &&
        !isStringConstantOperand(binary.optOperation->operand))
    {
      detail::throwQueryError(
        "ordered comparison on the '{}' field requires a string operand", fieldDisplayName(leftField));
    }

    auto const previousResolveStringConstantsToIds = _resolveStringConstantsToIds;
    auto restoreResolveMode =
      gsl_lite::finally([this, previousResolveStringConstantsToIds]
                        { _resolveStringConstantsToIds = previousResolveStringConstantsToIds; });

    if (isDictionaryField(leftField) && (opcode == OpCode::Like || isOrderedComparison(opcode)))
    {
      _resolveStringConstantsToIds = false;
    }

    auto const rightReg = compileExpression(binary.optOperation->operand);

    // Carry the left field (and its Custom dictionaryId) directly on the comparison so the
    // evaluator resolves the operand's type without scanning back for the LoadField. The
    // comparison consumes the top two results and writes the result into the left register.
    gsl_Expects(rightReg == leftReg + 1);
    _plan.instructions.push_back(Instruction{
      .op = opcode,
      .field = static_cast<std::uint8_t>(leftField),
      .operand = static_cast<std::int32_t>(rightReg),
      .constValue = leftCustomId,
      .size = 0,
      .data = nullptr,
    });
    popReg(rightReg);
    return leftReg;
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
        .field = 0,
        .operand = static_cast<std::int32_t>(reg),
        .constValue = 0,
        .size = 0,
        .data = nullptr,
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

    auto const fieldResult = detail::resolveVariableField(*var);

    if (!fieldResult)
    {
      detail::throwQueryError(fieldResult.error());
    }

    auto const field = *fieldResult;
    _lastField = field;

    if (isColdField(field))
    {
      _hasColdAccess = true;
    }
    else
    {
      _hasHotAccess = true;
    }

    std::int64_t constValue = 0;

    if ((var->type == VariableType::Custom || var->type == VariableType::Tag) && _dictionary != nullptr)
    {
      auto const dictionaryId = _dictionary->getOrIntern(var->name);
      constValue = static_cast<std::int64_t>(dictionaryId.raw());
    }

    _lastFieldCustomId = (var->type == VariableType::Custom) ? constValue : 0;

    auto const reg = pushReg();
    _plan.instructions.push_back(Instruction{
      .op = OpCode::Exists,
      .field = static_cast<std::uint8_t>(field),
      .operand = static_cast<std::int32_t>(reg),
      .constValue = constValue,
      .size = 0,
      .data = nullptr,
    });

    return reg;
  }

  std::uint32_t QueryCompiler::compileVariable(VariableExpression const& var)
  {
    // Tags are hot data
    if (var.type == VariableType::Tag)
    {
      _hasHotAccess = true;

      // Try to resolve tag name to ID via dictionary for bloom filter
      if (_dictionary != nullptr)
      {
        auto const tagId = _dictionary->getOrIntern(var.name);

        // Generate implicit tag comparison: track.tags().has(tagId)
        // This handles standalone "#tagname" queries like "#rock"
        // First, load the tag field (for the Eq instruction to detect it's a tag comparison)
        auto const fieldReg = pushReg();
        _plan.instructions.push_back(Instruction{
          .op = OpCode::LoadField,
          .field = static_cast<std::uint8_t>(Field::Tag),
          .operand = static_cast<std::int32_t>(fieldReg),
          .constValue = 0,
          .size = 0,
          .data = nullptr,
        });

        // Then load the tag ID as constant
        auto const constReg = pushReg();
        _plan.instructions.push_back(Instruction{
          .op = OpCode::LoadConstant,
          .field = 0,
          .operand = static_cast<std::int32_t>(constReg),
          .constValue = static_cast<std::int64_t>(tagId.raw()),
          .size = 0,
          .data = nullptr,
        });

        // Eq instruction - the Tag field is encoded directly so PlanEvaluator uses tags.has().
        // It consumes the loaded id and writes the result into the tag-field register.
        _plan.instructions.push_back(Instruction{
          .op = OpCode::Eq,
          .field = static_cast<std::uint8_t>(Field::Tag),
          .operand = static_cast<std::int32_t>(constReg),
          .constValue = 0,
          .size = 0,
          .data = nullptr,
        });

        popReg(constReg);
        return fieldReg;
      }
    }

    auto const fieldResult = detail::resolveVariableField(var);

    if (!fieldResult)
    {
      detail::throwQueryError(fieldResult.error());
    }

    auto const field = *fieldResult;
    _lastField = field; // Track for string resolution context

    // Track access profile for hot/cold determination based on field storage location
    if (isColdField(field))
    {
      _hasColdAccess = true;
    }
    else
    {
      _hasHotAccess = true;
    }

    // For custom fields, pre-resolve dictionaryId and store as constant (Option B)
    // If resolution fails (key not in dictionary), store 0 - evaluator will return empty string
    std::int64_t constValue = 0;

    if (var.type == VariableType::Custom)
    {
      if (_dictionary != nullptr)
      {
        auto const dictionaryId = _dictionary->getOrIntern(var.name);
        constValue = static_cast<std::int64_t>(dictionaryId.raw());
      }
      else
      {
        constValue = 0;
      }
    }

    // Remember the Custom key id (0 for non-Custom) so a following comparison carries it.
    _lastFieldCustomId = constValue;

    auto const reg = pushReg();
    _plan.instructions.push_back(Instruction{
      .op = OpCode::LoadField,
      .field = static_cast<std::uint8_t>(field),
      .operand = static_cast<std::int32_t>(reg),
      .constValue = constValue,
      .size = 0,
      .data = nullptr,
    });

    return reg;
  }

  std::uint32_t QueryCompiler::compileConstant(ConstantExpression const& constant)
  {
    return std::visit(utility::makeVisitor(
                        [this](bool val) -> std::uint32_t
                        {
                          auto const reg = pushReg();
                          _plan.instructions.push_back(Instruction{
                            .op = OpCode::LoadConstant,
                            .field = 0,
                            .operand = static_cast<std::int32_t>(reg),
                            .constValue = val ? 1 : 0,
                            .size = 0,
                            .data = nullptr,
                          });
                          return reg;
                        },
                        [this](std::int64_t val) -> std::uint32_t
                        {
                          auto const reg = pushReg();
                          _plan.instructions.push_back(Instruction{
                            .op = OpCode::LoadConstant,
                            .field = 0,
                            .operand = static_cast<std::int32_t>(reg),
                            .constValue = val,
                            .size = 0,
                            .data = nullptr,
                          });
                          return reg;
                        },
                        [this](UnitConstantExpression const& val) -> std::uint32_t
                        {
                          auto const reg = pushReg();
                          _plan.instructions.push_back(Instruction{
                            .op = OpCode::LoadConstant,
                            .field = 0,
                            .operand = static_cast<std::int32_t>(reg),
                            .constValue = scaleUnitConstant(val, _lastField),
                            .size = 0,
                            .data = nullptr,
                          });
                          return reg;
                        },
                        [this](std::string const& val) -> std::uint32_t
                        {
                          if (_lastField == Field::Codec)
                          {
                            if (auto const optCodec = parseAudioCodecName(val); optCodec)
                            {
                              auto const reg = pushReg();
                              _plan.instructions.push_back(Instruction{
                                .op = OpCode::LoadConstant,
                                .field = 0,
                                .operand = static_cast<std::int32_t>(reg),
                                .constValue = audioCodecStorageValue(*optCodec),
                                .size = 0,
                                .data = nullptr,
                              });
                              return reg;
                            }

                            detail::throwQueryError("unknown audio codec '{}'", val);
                          }

                          // Check if we should resolve this string via dictionary
                          // For metadata ID fields (artist, album, genre), resolve to numeric ID
                          auto const resolvedId = resolveStringConstant(val, _lastField);

                          // Resolved (metadata ID field) stores the numeric ID; otherwise the
                          // string is interned and the constant holds its index.
                          auto const constValue =
                            resolvedId >= 0 ? resolvedId : static_cast<std::int64_t>(addStringConstant(val));

                          auto const reg = pushReg();
                          _plan.instructions.push_back(Instruction{
                            .op = OpCode::LoadConstant,
                            .field = 0,
                            .operand = static_cast<std::int32_t>(reg),
                            .constValue = constValue,
                            .size = 0,
                            .data = nullptr,
                          });
                          return reg;
                        }),
                      constant);
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

    if (list.values.size() >= kInSetCompilationThreshold)
    {
      if (auto const optCompiled = compileInSetList(lhs, list); optCompiled)
      {
        return *optCompiled;
      }
    }

    // Not eligible for set compilation: expand into a chain of OR-ed equalities,
    // accumulating the running result in the register of the first comparison.
    auto optAccumReg = std::optional<std::uint32_t>{};

    for (auto const& value : list.values)
    {
      auto const leftReg = compileExpression(lhs);

      auto const leftField = _lastField;
      auto const leftCustomId = _lastFieldCustomId;

      auto const rightReg = compileConstant(value);

      gsl_Expects(rightReg == leftReg + 1);
      _plan.instructions.push_back(Instruction{
        .op = OpCode::Eq,
        .field = static_cast<std::uint8_t>(leftField),
        .operand = static_cast<std::int32_t>(rightReg),
        .constValue = leftCustomId,
        .size = 0,
        .data = nullptr,
      });
      popReg(rightReg); // Eq result is now in leftReg

      if (!optAccumReg)
      {
        optAccumReg = leftReg;
        continue;
      }

      // OR the new comparison (the top register) into the accumulator just below it.
      gsl_Expects(leftReg == *optAccumReg + 1);
      _plan.instructions.push_back(Instruction{
        .op = OpCode::Or,
        .field = 0,
        .operand = static_cast<std::int32_t>(leftReg),
        .constValue = 0,
        .size = 0,
        .data = nullptr,
      });
      popReg(leftReg); // Or result is now in *optAccumReg
    }

    gsl_Expects(optAccumReg); // non-empty list guarantees at least one comparison
    return *optAccumReg;
  }

  std::uint32_t QueryCompiler::compileInRange(Expression const& lhs, RangeExpression const& range)
  {
    auto const lhsLowerReg = compileExpression(lhs);

    // A range compiles to Ge/Le bounds, i.e. ordered comparisons. For dictionary
    // fields those must compare resolved text, so require string bounds and keep
    // them as string constants (see compileBinary / executeComparison).
    auto const leftField = _lastField;
    auto const leftCustomId = _lastFieldCustomId;
    auto const dictionaryBounds = isDictionaryField(leftField);

    if (dictionaryBounds && (!isStringConstant(range.lower) || !isStringConstant(range.upper)))
    {
      detail::throwQueryError("range over the '{}' field requires string bounds", fieldDisplayName(leftField));
    }

    auto const previousResolveStringConstantsToIds = _resolveStringConstantsToIds;
    auto restoreResolveMode =
      gsl_lite::finally([this, previousResolveStringConstantsToIds]
                        { _resolveStringConstantsToIds = previousResolveStringConstantsToIds; });

    if (dictionaryBounds)
    {
      _resolveStringConstantsToIds = false;
    }

    auto const lowerReg = compileConstant(range.lower);

    gsl_Expects(lowerReg == lhsLowerReg + 1);
    _plan.instructions.push_back(Instruction{
      .op = OpCode::Ge,
      .field = static_cast<std::uint8_t>(leftField),
      .operand = static_cast<std::int32_t>(lowerReg),
      .constValue = leftCustomId,
      .size = 0,
      .data = nullptr,
    });
    popReg(lowerReg); // Ge result is now in lhsLowerReg
    auto const geReg = lhsLowerReg;

    auto const lhsUpperReg = compileExpression(lhs);

    auto const upperReg = compileConstant(range.upper);

    gsl_Expects(upperReg == lhsUpperReg + 1);
    _plan.instructions.push_back(Instruction{
      .op = OpCode::Le,
      .field = static_cast<std::uint8_t>(leftField),
      .operand = static_cast<std::int32_t>(upperReg),
      .constValue = leftCustomId,
      .size = 0,
      .data = nullptr,
    });
    popReg(upperReg); // Le result is now in lhsUpperReg
    auto const leReg = lhsUpperReg;

    // AND the two bounds together; the result lands in the Ge register.
    gsl_Expects(leReg == geReg + 1);
    _plan.instructions.push_back(Instruction{
      .op = OpCode::And,
      .field = 0,
      .operand = static_cast<std::int32_t>(leReg),
      .constValue = 0,
      .size = 0,
      .data = nullptr,
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

    auto const fieldResult = detail::resolveVariableField(*variable);

    if (!fieldResult)
    {
      detail::throwQueryError(fieldResult.error());
    }

    auto const field = *fieldResult;

    if (isTagField(field))
    {
      return std::nullopt;
    }

    auto set = InSet{};
    set.stringValues = isStringField(field) || field == Field::Custom;

    for (auto const& value : list.values)
    {
      if (auto const valueResult = appendInSetValue(set, value, field); valueResult == InSetValueStatus::NotCompatible)
      {
        return std::nullopt;
      }
    }

    auto const leftReg = compileExpression(lhs);

    auto const leftCustomId = _lastFieldCustomId;
    auto const setIndex = addInSet(std::move(set));
    // InSet rewrites its operand in place, so the result stays in the loaded register.
    // It spends constValue on the set index, so the Custom key id rides in size.
    _plan.instructions.push_back(Instruction{
      .op = OpCode::InSet,
      .field = static_cast<std::uint8_t>(field),
      .operand = static_cast<std::int32_t>(leftReg),
      .constValue = static_cast<std::int64_t>(setIndex),
      .size = static_cast<std::uint32_t>(leftCustomId),
      .data = nullptr,
    });

    return std::optional<std::uint32_t>{leftReg};
  }

  std::int64_t QueryCompiler::resolveStringConstant(std::string const& str, Field field)
  {
    // Only resolve for metadata ID fields and tag fields
    if (!_resolveStringConstantsToIds || (!isDictionaryField(field) && !isTagField(field)))
    {
      return -1;
    }

    if (_dictionary == nullptr)
    {
      return -1;
    }

    // Reserve in memory - if already exists, returns existing ID; if not, adds to memory only
    auto const id = _dictionary->getOrIntern(str);
    return static_cast<std::int64_t>(id.raw());
  }

  QueryCompiler::InSetValueStatus QueryCompiler::appendInSetValue(InSet& set,
                                                                  ConstantExpression const& constant,
                                                                  Field field)
  {
    return std::visit(utility::makeVisitor(
                        [&set](bool value) -> InSetValueStatus
                        {
                          if (set.stringValues)
                          {
                            return InSetValueStatus::NotCompatible;
                          }

                          set.numericValues.insert(value ? 1 : 0);
                          return InSetValueStatus::Appended;
                        },
                        [&set](std::int64_t value) -> InSetValueStatus
                        {
                          if (set.stringValues)
                          {
                            return InSetValueStatus::NotCompatible;
                          }

                          set.numericValues.insert(value);
                          return InSetValueStatus::Appended;
                        },
                        [&set, field](UnitConstantExpression const& value) -> InSetValueStatus
                        {
                          if (set.stringValues)
                          {
                            return InSetValueStatus::NotCompatible;
                          }

                          set.numericValues.insert(scaleUnitConstant(value, field));
                          return InSetValueStatus::Appended;
                        },
                        [this, &set, field](std::string const& value) -> InSetValueStatus
                        {
                          if (set.stringValues)
                          {
                            set.strings.push_back(value);
                            return InSetValueStatus::Appended;
                          }

                          if (field == Field::Codec)
                          {
                            if (auto const optCodec = parseAudioCodecName(value); optCodec)
                            {
                              set.numericValues.insert(audioCodecStorageValue(*optCodec));
                              return InSetValueStatus::Appended;
                            }

                            detail::throwQueryError("unknown audio codec '{}'", value);
                          }

                          if (!isDictionaryField(field) || _dictionary == nullptr)
                          {
                            return InSetValueStatus::NotCompatible;
                          }

                          auto const id = _dictionary->getOrIntern(value);
                          set.numericValues.insert(static_cast<std::int64_t>(id.raw()));
                          return InSetValueStatus::Appended;
                        }),
                      constant);
  }

  Result<ExecutionPlan> QueryCompiler::compile(Expression const& expr)

  try
  {
    _plan = ExecutionPlan{};
    _plan.dictionary = _dictionary;
    _plan.tagBloomMask = computeRequiredTagBloomMask(expr, _dictionary);
    _nextReg = 0;
    _lastFieldCustomId = 0;
    _hasHotAccess = false;
    _hasColdAccess = false;
    _resolveStringConstantsToIds = true;

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

    // Set access profile based on what data was accessed
    if (_hasHotAccess && _hasColdAccess)
    {
      _plan.accessProfile = AccessProfile::HotAndCold;
    }
    else if (_hasColdAccess)
    {
      _plan.accessProfile = AccessProfile::ColdOnly;
    }
    else if (_hasHotAccess)
    {
      _plan.accessProfile = AccessProfile::HotOnly;
    }
    else
    {
      _plan.accessProfile = AccessProfile::NoTrackData;
    }

    return _plan;
  }
  catch (detail::QueryException const& ex)
  {
    return std::unexpected{ex.error()};
  }

  Result<ExecutionPlan> compileQuery(Expression const& expr, library::DictionaryStore* dictionary)
  {
    auto compiler = dictionary != nullptr ? QueryCompiler{dictionary} : QueryCompiler{};
    return compiler.compile(expr);
  }
} // namespace ao::query
