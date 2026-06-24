// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/Predicate.h>
#include <ao/query/QueryCompiler.h>
#include <ao/query/detail/Bytecode.h>
#include <ao/utility/String.h>
#include <ao/utility/VariantVisitor.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cctype>
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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "query/UnitDispatch.h"
#pragma GCC diagnostic pop

namespace ao::query
{
  namespace
  {
    // Bloom filter uses 5 bits per tag (bit mask 31 = 0x1F)
    constexpr std::uint32_t kBloomBitMask = 31;
    // The Phase 0 Query IN threshold sweep shows the membership plan is faster
    // than repeated field-load/equality expansion even for a one-item list.
    constexpr std::size_t kInSetCompilationThreshold = 1;

    std::unexpected<Error> rejectQuery(std::string_view message)
    {
      return makeError(Error::Code::FormatRejected, std::string{message});
    }

    // Formatting overload mirroring throwException: builds the FormatRejected
    // message inline so call sites read like the old throw without wrapping every
    // diagnostic in std::format(...). The sizeof...(Args) > 0 constraint keeps the
    // no-argument literal case on the cheaper string_view overload above.
    template<typename... Args>
      requires(sizeof...(Args) > 0)
    std::unexpected<Error> rejectQuery(std::format_string<Args...> fmt, Args&&... args)
    {
      return makeError(Error::Code::FormatRejected, std::format(fmt, std::forward<Args>(args)...));
    }

    Result<OpCode> toOpCode(Operator op)
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
        case Operator::In: return rejectQuery("operator 'in' requires list compilation");
        case Operator::Add:
          return rejectQuery("string concatenation is not a query predicate; use it in a format expression");
        case Operator::Exists: return OpCode::Exists;
        default: return rejectQuery("unsupported operator");
      }
    }

    bool isUnsupportedLikeField(Field field)
    {
      return field == Field::CoverArtId || field == Field::Tag;
    }

    std::uint32_t tagBloomBit(library::DictionaryStore* dict, std::string_view tagName)
    {
      if (dict == nullptr)
      {
        return 0;
      }

      auto const tagId = dict->getOrIntern(tagName);
      return std::uint32_t{1} << (tagId.raw() & kBloomBitMask);
    }

    std::uint32_t computeRequiredTagBloomMask(Expression const& expr, library::DictionaryStore* dict);

    std::uint32_t computeRequiredTagBloomMask(BinaryExpression const& binary, library::DictionaryStore* dict)
    {
      auto const lhsMask = computeRequiredTagBloomMask(binary.operand, dict);

      if (!binary.optOperation)
      {
        return lhsMask;
      }

      auto const rhsMask = computeRequiredTagBloomMask(binary.optOperation->operand, dict);

      switch (binary.optOperation->op)
      {
        case Operator::And: return lhsMask | rhsMask;

        // OR can only require tags that are shared by every matching branch.
        case Operator::Or: return lhsMask & rhsMask;

        default: return 0;
      }
    }

    std::uint32_t computeRequiredTagBloomMask(UnaryExpression const& unary, library::DictionaryStore* dict)
    {
      if (unary.op == Operator::Not)
      {
        return 0;
      }

      return computeRequiredTagBloomMask(unary.operand, dict);
    }

    std::uint32_t computeRequiredTagBloomMask(Expression const& expr, library::DictionaryStore* dict)
    {
      return std::visit(utility::makeVisitor(
                          [dict](VariableExpression const& variable)
                          {
                            if (variable.type != VariableType::Tag)
                            {
                              return std::uint32_t{0};
                            }

                            return tagBloomBit(dict, variable.name);
                          },
                          [](ConstantExpression const&) { return std::uint32_t{0}; },
                          [](ListExpression const&) { return std::uint32_t{0}; },
                          [](RangeExpression const&) { return std::uint32_t{0}; },
                          [dict](std::unique_ptr<BinaryExpression> const& binary)
                          {
                            if (!binary)
                            {
                              return std::uint32_t{0};
                            }

                            return computeRequiredTagBloomMask(*binary, dict);
                          },
                          [dict](std::unique_ptr<UnaryExpression> const& unary)
                          {
                            if (!unary)
                            {
                              return std::uint32_t{0};
                            }

                            return computeRequiredTagBloomMask(*unary, dict);
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
                             [](std::unique_ptr<BinaryExpression> const& binary) -> VariableExpression const*
                             {
                               if (!binary)
                               {
                                 return nullptr;
                               }

                               return bareNonTagVariableInPredicatePosition(*binary);
                             },
                             [](std::unique_ptr<UnaryExpression> const& unary) -> VariableExpression const*
                             {
                               if (!unary)
                               {
                                 return nullptr;
                               }

                               return bareNonTagVariableInPredicatePosition(*unary);
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

    Result<std::uint64_t> unitMultiplier(Field field, std::string_view unit)
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

      return rejectQuery("unit '{}' is not supported for {} constants", normalized, fieldDisplayName(field));
    }

    Result<std::uint64_t> scaleUnitSegment(std::string_view numberPart,
                                           std::string_view suffixPart,
                                           Field field,
                                           UnitConstantExpression const& constant)
    {
      if (numberPart.empty() || suffixPart.empty())
      {
        return rejectQuery("invalid unit literal '{}'", constant.lexeme);
      }

      auto const dotPos = numberPart.find('.');

      if (dotPos != std::string_view::npos && numberPart.find('.', dotPos + 1) != std::string_view::npos)
      {
        return rejectQuery("invalid unit literal '{}'", constant.lexeme);
      }

      auto const wholePart = numberPart.substr(0, dotPos);
      auto const fractionPart = dotPos == std::string_view::npos ? std::string_view{} : numberPart.substr(dotPos + 1);

      if (wholePart.empty() || (dotPos != std::string_view::npos && fractionPart.empty()))
      {
        return rejectQuery("invalid unit literal '{}'", constant.lexeme);
      }

      auto const optWhole = parseUnsigned(wholePart);
      auto const optFraction =
        fractionPart.empty() ? std::optional<std::uint64_t>{std::uint64_t{0}} : parseUnsigned(fractionPart);
      auto const optDenominator = pow10(fractionPart.size());

      if (!optWhole || !optFraction || !optDenominator)
      {
        return rejectQuery("invalid unit literal '{}'", constant.lexeme);
      }

      auto const optScaledWhole = checkedMul(*optWhole, *optDenominator);
      auto const optNumerator = optScaledWhole ? checkedAdd(*optScaledWhole, *optFraction) : std::nullopt;
      auto const multiplier = unitMultiplier(field, suffixPart);

      if (!multiplier)
      {
        return multiplier;
      }

      auto const optScaledNumerator = optNumerator ? checkedMul(*optNumerator, *multiplier) : std::nullopt;

      if (!optScaledNumerator)
      {
        return rejectQuery("unit literal '{}' is out of range", constant.lexeme);
      }

      if (*optScaledNumerator % *optDenominator != 0)
      {
        return rejectQuery(
          "unit literal '{}' does not resolve to an integer {} value", constant.lexeme, fieldDisplayName(field));
      }

      auto const magnitude = *optScaledNumerator / *optDenominator;
      return magnitude;
    }

    Result<std::int64_t> scaleUnitConstant(UnitConstantExpression const& constant, Field field)
    {
      if (field == Field::TagBloom)
      {
        return rejectQuery("unit literal '{}' requires a numeric field context", constant.lexeme);
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
        auto const* const suffixStart =
          std::ranges::find_if(lexeme, [](unsigned char ch) { return std::isalpha(ch) != 0; });

        if (suffixStart == lexeme.end())
        {
          return rejectQuery("invalid unit literal '{}'", constant.lexeme);
        }

        auto const suffixOffset = static_cast<std::size_t>(std::distance(lexeme.begin(), suffixStart));
        auto const numberPart = lexeme.substr(0, suffixOffset);
        auto const suffixAndRest = lexeme.substr(suffixOffset);
        auto const* const nextNumber =
          std::ranges::find_if(suffixAndRest, [](unsigned char ch) { return std::isdigit(ch) != 0; });
        auto const suffixSize = static_cast<std::size_t>(std::distance(suffixAndRest.begin(), nextNumber));
        auto const suffixPart = suffixAndRest.substr(0, suffixSize);
        auto const magnitude = scaleUnitSegment(numberPart, suffixPart, field, constant);

        if (!magnitude)
        {
          return std::unexpected{magnitude.error()};
        }

        auto const optTotal = checkedAdd(total, *magnitude);

        if (!optTotal)
        {
          return rejectQuery("unit literal '{}' is out of range", constant.lexeme);
        }

        total = *optTotal;
        segmentCount++;
        lexeme = suffixAndRest.substr(suffixSize);
      }

      if (segmentCount > 1 && field != Field::Duration)
      {
        return rejectQuery("compound unit literal '{}' is only supported for duration constants", constant.lexeme);
      }

      if (!negative)
      {
        if (total > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
          return rejectQuery("unit literal '{}' is out of range", constant.lexeme);
        }

        return static_cast<std::int64_t>(total);
      }

      auto const negativeLimit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;

      if (total > negativeLimit)
      {
        return rejectQuery("unit literal '{}' is out of range", constant.lexeme);
      }

      if (total == negativeLimit)
      {
        return std::numeric_limits<std::int64_t>::min();
      }

      return -static_cast<std::int64_t>(total);
    }
  }

  QueryCompiler::QueryCompiler(library::DictionaryStore* dict)
    : _dict{dict}
  {
    gsl_Expects(dict != nullptr);
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

  Result<std::uint32_t> QueryCompiler::compileExpression(Expression const& expr)
  {
    return std::visit(
      utility::makeVisitor(
        [this](std::unique_ptr<BinaryExpression> const& binary) -> Result<std::uint32_t>
        {
          gsl_Expects(binary != nullptr);
          return compileBinary(*binary);
        },
        [this](std::unique_ptr<UnaryExpression> const& unary) -> Result<std::uint32_t>
        {
          gsl_Expects(unary != nullptr);
          return compileUnary(*unary);
        },
        [this](VariableExpression const& var) -> Result<std::uint32_t> { return compileVariable(var); },
        [this](ConstantExpression const& constant) -> Result<std::uint32_t> { return compileConstant(constant); },
        [this](ListExpression const& list) -> Result<std::uint32_t> { return compileList(list); },
        [this](RangeExpression const& range) -> Result<std::uint32_t> { return compileRange(range); }),
      expr);
  }

  Result<std::uint32_t> QueryCompiler::compilePredicate(Expression const& expr)
  {
    if (auto const* var = bareNonTagVariableInPredicatePosition(expr); var != nullptr)
    {
      auto const name = variableDisplayName(*var);
      return rejectQuery(
        "bare field '{}' is not a predicate; use '{}?' for existence, '!{}?' for missing, or compare it "
        "explicitly",
        name,
        name,
        name);
    }

    if (!isPredicateExpression(expr))
    {
      return rejectQuery("query expression is not a predicate");
    }

    return compileExpression(expr);
  }

  Result<std::uint32_t> QueryCompiler::compileBinary(BinaryExpression const& binary)
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

      if (!leftReg)
      {
        return std::unexpected{leftReg.error()};
      }

      auto const rightReg = compilePredicate(binary.optOperation->operand);

      if (!rightReg)
      {
        return std::unexpected{rightReg.error()};
      }

      auto const opcode = toOpCode(binary.optOperation->op);

      if (!opcode)
      {
        return std::unexpected{opcode.error()};
      }

      // And/Or consume the top two results (left immediately below right) and write the
      // result back into the left register.
      gsl_Expects(*rightReg == *leftReg + 1);
      _plan.instructions.push_back(Instruction{
        .op = *opcode,
        .field = 0,
        .operand = static_cast<std::int32_t>(*rightReg),
        .constValue = 0,
        .size = 0,
        .data = nullptr,
      });
      popReg(*rightReg);
      return *leftReg;
    }

    // Comparison (Eq/Ne/Lt/Le/Gt/Ge/Like). Compile the left operand first.
    auto const leftReg = compileExpression(binary.operand);

    if (!leftReg)
    {
      return std::unexpected{leftReg.error()};
    }

    // Save the left field (and its Custom dictId) before compiling the right operand,
    // which overwrites _lastField.
    auto const leftField = _lastField;
    auto const leftCustomId = _lastFieldCustomId;
    auto const opcode = toOpCode(binary.optOperation->op);

    if (!opcode)
    {
      return std::unexpected{opcode.error()};
    }

    if (*opcode == OpCode::Like && isUnsupportedLikeField(leftField))
    {
      return rejectQuery("LIKE operator not supported for coverArt or tags");
    }

    // Dictionary fields store interned IDs, so an ordered comparison (<, <=, >, >=)
    // must compare the resolved text rather than the ID. Require a string operand
    // and keep it as a string constant (like LIKE) so the evaluator resolves the
    // field's ID back to text at compare time.
    if (isOrderedComparison(*opcode) && isDictionaryField(leftField) &&
        !isStringConstantOperand(binary.optOperation->operand))
    {
      return rejectQuery("ordered comparison on the '{}' field requires a string operand", fieldDisplayName(leftField));
    }

    auto const previousResolveStringConstantsToIds = _resolveStringConstantsToIds;
    auto restoreResolveMode =
      gsl_lite::finally([this, previousResolveStringConstantsToIds]
                        { _resolveStringConstantsToIds = previousResolveStringConstantsToIds; });

    if (isDictionaryField(leftField) && (*opcode == OpCode::Like || isOrderedComparison(*opcode)))
    {
      _resolveStringConstantsToIds = false;
    }

    auto const rightReg = compileExpression(binary.optOperation->operand);

    if (!rightReg)
    {
      return std::unexpected{rightReg.error()};
    }

    // Carry the left field (and its Custom dictId) directly on the comparison so the
    // evaluator resolves the operand's type without scanning back for the LoadField. The
    // comparison consumes the top two results and writes the result into the left register.
    gsl_Expects(*rightReg == *leftReg + 1);
    _plan.instructions.push_back(Instruction{
      .op = *opcode,
      .field = static_cast<std::uint8_t>(leftField),
      .operand = static_cast<std::int32_t>(*rightReg),
      .constValue = leftCustomId,
      .size = 0,
      .data = nullptr,
    });
    popReg(*rightReg);
    return *leftReg;
  }

  Result<std::uint32_t> QueryCompiler::compileUnary(UnaryExpression const& unary)
  {
    if (unary.op == Operator::Exists)
    {
      return compileExists(unary.operand);
    }

    if (unary.op == Operator::Not)
    {
      auto const reg = compilePredicate(unary.operand);

      if (!reg)
      {
        return std::unexpected{reg.error()};
      }

      // Not rewrites its operand in place; the result stays in the same register.
      _plan.instructions.push_back(Instruction{
        .op = OpCode::Not,
        .field = 0,
        .operand = static_cast<std::int32_t>(*reg),
        .constValue = 0,
        .size = 0,
        .data = nullptr,
      });
      return *reg;
    }

    return rejectQuery("unsupported unary operator");
  }

  Result<std::uint32_t> QueryCompiler::compileExists(Expression const& operand)
  {
    auto const* var = std::get_if<VariableExpression>(&operand);

    if (var == nullptr)
    {
      return rejectQuery("operator '?' requires a field operand");
    }

    auto const fieldResult = resolveVariableField(*var);

    if (!fieldResult)
    {
      return std::unexpected{fieldResult.error()};
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

    if ((var->type == VariableType::Custom || var->type == VariableType::Tag) && _dict != nullptr)
    {
      auto const dictId = _dict->getOrIntern(var->name);
      constValue = static_cast<std::int64_t>(dictId.raw());
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

  Result<std::uint32_t> QueryCompiler::compileVariable(VariableExpression const& var)
  {
    // Tags are hot data
    if (var.type == VariableType::Tag)
    {
      _hasHotAccess = true;

      // Try to resolve tag name to ID via dictionary for bloom filter
      if (_dict != nullptr)
      {
        auto const tagId = _dict->getOrIntern(var.name);

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

    auto const fieldResult = resolveVariableField(var);

    if (!fieldResult)
    {
      return std::unexpected{fieldResult.error()};
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

    // For custom fields, pre-resolve dictId and store as constant (Option B)
    // If resolution fails (key not in dictionary), store 0 - evaluator will return empty string
    std::int64_t constValue = 0;

    if (var.type == VariableType::Custom)
    {
      if (_dict != nullptr)
      {
        auto const dictId = _dict->getOrIntern(var.name);
        constValue = static_cast<std::int64_t>(dictId.raw());
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

  Result<std::uint32_t> QueryCompiler::compileConstant(ConstantExpression const& constant)
  {
    return std::visit(utility::makeVisitor(
                        [this](bool val) -> Result<std::uint32_t>
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
                        [this](std::int64_t val) -> Result<std::uint32_t>
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
                        [this](UnitConstantExpression const& val) -> Result<std::uint32_t>
                        {
                          auto const scaled = scaleUnitConstant(val, _lastField);

                          if (!scaled)
                          {
                            return std::unexpected{scaled.error()};
                          }

                          auto const reg = pushReg();
                          _plan.instructions.push_back(Instruction{
                            .op = OpCode::LoadConstant,
                            .field = 0,
                            .operand = static_cast<std::int32_t>(reg),
                            .constValue = *scaled,
                            .size = 0,
                            .data = nullptr,
                          });
                          return reg;
                        },
                        [this](std::string const& val) -> Result<std::uint32_t>
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

                            return rejectQuery("unknown audio codec '{}'", val);
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

  Result<std::uint32_t> QueryCompiler::compileList(ListExpression const& /*list*/)
  {
    return rejectQuery("list expressions are only supported as the right operand of 'in'");
  }

  Result<std::uint32_t> QueryCompiler::compileRange(RangeExpression const& /*range*/)
  {
    return rejectQuery("range expressions are only supported as the right operand of 'in'");
  }

  Result<std::uint32_t> QueryCompiler::compileIn(Expression const& lhs, Expression const& rhs)
  {
    if (auto const* list = std::get_if<ListExpression>(&rhs); list != nullptr)
    {
      return compileInWithList(lhs, *list);
    }

    auto const* range = std::get_if<RangeExpression>(&rhs);

    if (range == nullptr)
    {
      return rejectQuery("operator 'in' expects a list or range right operand");
    }

    return compileInRange(lhs, *range);
  }

  Result<std::uint32_t> QueryCompiler::compileInWithList(Expression const& lhs, ListExpression const& list)
  {
    if (list.values.empty())
    {
      return rejectQuery("operator 'in' expects a non-empty list");
    }

    if (list.values.size() >= kInSetCompilationThreshold)
    {
      auto const compiled = compileInSetList(lhs, list);

      if (!compiled)
      {
        return std::unexpected{compiled.error()};
      }

      if (compiled->has_value())
      {
        return **compiled;
      }
    }

    // Not eligible for set compilation: expand into a chain of OR-ed equalities,
    // accumulating the running result in the register of the first comparison.
    auto optAccumReg = std::optional<std::uint32_t>{};

    for (auto const& value : list.values)
    {
      auto const leftReg = compileExpression(lhs);

      if (!leftReg)
      {
        return std::unexpected{leftReg.error()};
      }

      auto const leftField = _lastField;
      auto const leftCustomId = _lastFieldCustomId;

      auto const rightReg = compileConstant(value);

      if (!rightReg)
      {
        return std::unexpected{rightReg.error()};
      }

      gsl_Expects(*rightReg == *leftReg + 1);
      _plan.instructions.push_back(Instruction{
        .op = OpCode::Eq,
        .field = static_cast<std::uint8_t>(leftField),
        .operand = static_cast<std::int32_t>(*rightReg),
        .constValue = leftCustomId,
        .size = 0,
        .data = nullptr,
      });
      popReg(*rightReg); // Eq result is now in *leftReg

      if (!optAccumReg)
      {
        optAccumReg = *leftReg;
        continue;
      }

      // OR the new comparison (the top register) into the accumulator just below it.
      gsl_Expects(*leftReg == *optAccumReg + 1);
      _plan.instructions.push_back(Instruction{
        .op = OpCode::Or,
        .field = 0,
        .operand = static_cast<std::int32_t>(*leftReg),
        .constValue = 0,
        .size = 0,
        .data = nullptr,
      });
      popReg(*leftReg); // Or result is now in *optAccumReg
    }

    gsl_Expects(optAccumReg.has_value()); // non-empty list guarantees at least one comparison
    return *optAccumReg;
  }

  Result<std::uint32_t> QueryCompiler::compileInRange(Expression const& lhs, RangeExpression const& range)
  {
    auto const lhsLowerReg = compileExpression(lhs);

    if (!lhsLowerReg)
    {
      return std::unexpected{lhsLowerReg.error()};
    }

    // A range compiles to Ge/Le bounds, i.e. ordered comparisons. For dictionary
    // fields those must compare resolved text, so require string bounds and keep
    // them as string constants (see compileBinary / executeComparison).
    auto const leftField = _lastField;
    auto const leftCustomId = _lastFieldCustomId;
    auto const dictionaryBounds = isDictionaryField(leftField);

    if (dictionaryBounds && (!isStringConstant(range.lower) || !isStringConstant(range.upper)))
    {
      return rejectQuery("range over the '{}' field requires string bounds", fieldDisplayName(leftField));
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

    if (!lowerReg)
    {
      return std::unexpected{lowerReg.error()};
    }

    gsl_Expects(*lowerReg == *lhsLowerReg + 1);
    _plan.instructions.push_back(Instruction{
      .op = OpCode::Ge,
      .field = static_cast<std::uint8_t>(leftField),
      .operand = static_cast<std::int32_t>(*lowerReg),
      .constValue = leftCustomId,
      .size = 0,
      .data = nullptr,
    });
    popReg(*lowerReg); // Ge result is now in *lhsLowerReg
    auto const geReg = *lhsLowerReg;

    auto const lhsUpperReg = compileExpression(lhs);

    if (!lhsUpperReg)
    {
      return std::unexpected{lhsUpperReg.error()};
    }

    auto const upperReg = compileConstant(range.upper);

    if (!upperReg)
    {
      return std::unexpected{upperReg.error()};
    }

    gsl_Expects(*upperReg == *lhsUpperReg + 1);
    _plan.instructions.push_back(Instruction{
      .op = OpCode::Le,
      .field = static_cast<std::uint8_t>(leftField),
      .operand = static_cast<std::int32_t>(*upperReg),
      .constValue = leftCustomId,
      .size = 0,
      .data = nullptr,
    });
    popReg(*upperReg); // Le result is now in *lhsUpperReg
    auto const leReg = *lhsUpperReg;

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

  Result<std::optional<std::uint32_t>> QueryCompiler::compileInSetList(Expression const& lhs,
                                                                       ListExpression const& list)
  {
    auto const* variable = std::get_if<VariableExpression>(&lhs);

    if (variable == nullptr)
    {
      return std::optional<std::uint32_t>{};
    }

    auto const fieldResult = resolveVariableField(*variable);

    if (!fieldResult)
    {
      return std::unexpected{fieldResult.error()};
    }

    auto const field = *fieldResult;

    if (isTagField(field))
    {
      return std::optional<std::uint32_t>{};
    }

    auto set = InSet{};
    set.stringValues = isStringField(field) || field == Field::Custom;

    for (auto const& value : list.values)
    {
      auto const valueResult = appendInSetValue(set, value, field);

      if (!valueResult)
      {
        return std::unexpected{valueResult.error()};
      }

      if (*valueResult == InSetValueStatus::NotCompatible)
      {
        return std::optional<std::uint32_t>{};
      }
    }

    auto const leftReg = compileExpression(lhs);

    if (!leftReg)
    {
      return std::unexpected{leftReg.error()};
    }

    auto const leftCustomId = _lastFieldCustomId;
    auto const setIndex = addInSet(std::move(set));
    // InSet rewrites its operand in place, so the result stays in the loaded register.
    // It spends constValue on the set index, so the Custom key id rides in size.
    _plan.instructions.push_back(Instruction{
      .op = OpCode::InSet,
      .field = static_cast<std::uint8_t>(field),
      .operand = static_cast<std::int32_t>(*leftReg),
      .constValue = static_cast<std::int64_t>(setIndex),
      .size = static_cast<std::uint32_t>(leftCustomId),
      .data = nullptr,
    });

    return std::optional<std::uint32_t>{*leftReg};
  }

  std::int64_t QueryCompiler::resolveStringConstant(std::string const& str, Field field)
  {
    // Only resolve for metadata ID fields and tag fields
    if (!_resolveStringConstantsToIds || (!isDictionaryField(field) && !isTagField(field)))
    {
      return -1;
    }

    if (_dict == nullptr)
    {
      return -1;
    }

    // Reserve in memory - if already exists, returns existing ID; if not, adds to memory only
    auto const id = _dict->getOrIntern(str);
    return static_cast<std::int64_t>(id.raw());
  }

  Result<QueryCompiler::InSetValueStatus> QueryCompiler::appendInSetValue(InSet& set,
                                                                          ConstantExpression const& constant,
                                                                          Field field)
  {
    return std::visit(utility::makeVisitor(
                        [&set](bool value) -> Result<InSetValueStatus>
                        {
                          if (set.stringValues)
                          {
                            return InSetValueStatus::NotCompatible;
                          }

                          set.numericValues.insert(value ? 1 : 0);
                          return InSetValueStatus::Appended;
                        },
                        [&set](std::int64_t value) -> Result<InSetValueStatus>
                        {
                          if (set.stringValues)
                          {
                            return InSetValueStatus::NotCompatible;
                          }

                          set.numericValues.insert(value);
                          return InSetValueStatus::Appended;
                        },
                        [&set, field](UnitConstantExpression const& value) -> Result<InSetValueStatus>
                        {
                          if (set.stringValues)
                          {
                            return InSetValueStatus::NotCompatible;
                          }

                          auto const scaled = scaleUnitConstant(value, field);

                          if (!scaled)
                          {
                            return std::unexpected{scaled.error()};
                          }

                          set.numericValues.insert(*scaled);
                          return InSetValueStatus::Appended;
                        },
                        [this, &set, field](std::string const& value) -> Result<InSetValueStatus>
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

                            return rejectQuery("unknown audio codec '{}'", value);
                          }

                          if (!isDictionaryField(field) || _dict == nullptr)
                          {
                            return InSetValueStatus::NotCompatible;
                          }

                          auto const id = _dict->getOrIntern(value);
                          set.numericValues.insert(static_cast<std::int64_t>(id.raw()));
                          return InSetValueStatus::Appended;
                        }),
                      constant);
  }

  Result<ExecutionPlan> QueryCompiler::compile(Expression const& expr)
  {
    _plan = ExecutionPlan{};
    _plan.dictionary = _dict;
    _plan.tagBloomMask = computeRequiredTagBloomMask(expr, _dict);
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

    if (auto compileResult = compilePredicate(expr); !compileResult)
    {
      return std::unexpected{compileResult.error()};
    }

    // Set access profile based on what data was accessed
    if (_hasHotAccess && _hasColdAccess)
    {
      _plan.accessProfile = AccessProfile::HotAndCold;
    }
    else if (_hasColdAccess)
    {
      _plan.accessProfile = AccessProfile::ColdOnly;
    }
    else
    {
      _plan.accessProfile = AccessProfile::HotOnly;
    }

    return _plan;
  }

  Result<ExecutionPlan> compileQuery(Expression const& expr, library::DictionaryStore* dict)
  {
    auto compiler = dict != nullptr ? QueryCompiler{dict} : QueryCompiler{};
    return compiler.compile(expr);
  }
} // namespace ao::query
