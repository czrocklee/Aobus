// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/Predicate.h>
#include <ao/query/QueryCompiler.h>
#include <ao/query/detail/Bytecode.h>
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

    bool isOrderedComparison(OpCode op)
    {
      return op == OpCode::Lt || op == OpCode::Le || op == OpCode::Gt || op == OpCode::Ge;
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

    std::string toLower(std::string_view value)
    {
      return value | std::views::transform([](unsigned char ch) { return static_cast<char>(std::tolower(ch)); }) |
             std::ranges::to<std::string>();
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
      auto const normalized = toLower(unit);

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

  Result<> QueryCompiler::compileExpression(Expression const& expr)
  {
    return std::visit(utility::makeVisitor(
                        [this](std::unique_ptr<BinaryExpression> const& binary) -> Result<>
                        {
                          gsl_Expects(binary != nullptr);
                          return compileBinary(*binary);
                        },
                        [this](std::unique_ptr<UnaryExpression> const& unary) -> Result<>
                        {
                          gsl_Expects(unary != nullptr);
                          return compileUnary(*unary);
                        },
                        [this](VariableExpression const& var) -> Result<> { return compileVariable(var); },
                        [this](ConstantExpression const& constant) -> Result<> { return compileConstant(constant); },
                        [this](ListExpression const& list) -> Result<> { return compileList(list); },
                        [this](RangeExpression const& range) -> Result<> { return compileRange(range); }),
                      expr);
  }

  Result<> QueryCompiler::compilePredicate(Expression const& expr)
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

  Result<> QueryCompiler::compileBinary(BinaryExpression const& binary)
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
      if (auto result = compilePredicate(binary.operand); !result)
      {
        return result;
      }

      if (auto result = compilePredicate(binary.optOperation->operand); !result)
      {
        return result;
      }

      auto const opcode = toOpCode(binary.optOperation->op);

      if (!opcode)
      {
        return std::unexpected{opcode.error()};
      }

      auto const rightReg = _nextReg - 1;
      _plan.instructions.push_back(Instruction{
        .op = *opcode,
        .field = 0,
        .operand = static_cast<std::int32_t>(rightReg),
        .constValue = 0,
        .size = 0,
        .data = nullptr,
      });
      _nextReg--;
      return {};
    }

    // Compile left operand
    if (auto result = compileExpression(binary.operand); !result)
    {
      return result;
    }

    // Save left field before compiling right operand (which will overwrite _lastField)
    if (auto const leftField = _lastField; binary.optOperation)
    {
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
        return rejectQuery(
          "ordered comparison on the '{}' field requires a string operand", fieldDisplayName(leftField));
      }

      auto const previousResolveStringConstantsToIds = _resolveStringConstantsToIds;
      auto restoreResolveMode =
        gsl_lite::finally([this, previousResolveStringConstantsToIds]
                          { _resolveStringConstantsToIds = previousResolveStringConstantsToIds; });

      if (isDictionaryField(leftField) && (*opcode == OpCode::Like || isOrderedComparison(*opcode)))
      {
        _resolveStringConstantsToIds = false;
      }

      // Compile right operand
      if (auto result = compileExpression(binary.optOperation->operand); !result)
      {
        return result;
      }

      // Right operand result is in _nextReg - 1
      // Binary op will store result in operand - 1 = (_nextReg - 1) - 1 = _nextReg - 2
      auto const rightReg = _nextReg - 1;

      // Store leftField in field for LIKE instructions so executeLike can use it directly
      std::uint8_t const instrField =
        (*opcode == OpCode::Like) ? static_cast<std::uint8_t>(leftField) : std::uint8_t{0};

      _plan.instructions.push_back(Instruction{
        .op = *opcode,
        .field = instrField,
        .operand = static_cast<std::int32_t>(rightReg),
        .constValue = 0,
        .size = 0,
        .data = nullptr,
      });

      // After binary op, the result is stored in the left register (rightReg - 1)
      // The right register is now free, so decrement _nextReg
      _nextReg--;
    }

    return {};
  }

  Result<> QueryCompiler::compileUnary(UnaryExpression const& unary)
  {
    if (unary.op == Operator::Exists)
    {
      return compileExists(unary.operand);
    }

    if (unary.op == Operator::Not)
    {
      if (auto result = compilePredicate(unary.operand); !result)
      {
        return result;
      }

      _plan.instructions.push_back(Instruction{
        .op = OpCode::Not,
        .field = 0,
        .operand = static_cast<std::int32_t>(_nextReg - 1),
        .constValue = 0,
        .size = 0,
        .data = nullptr,
      });
      return {};
    }

    return rejectQuery("unsupported unary operator");
  }

  Result<> QueryCompiler::compileExists(Expression const& operand)
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

    _plan.instructions.push_back(Instruction{
      .op = OpCode::Exists,
      .field = static_cast<std::uint8_t>(field),
      .operand = static_cast<std::int32_t>(_nextReg++),
      .constValue = constValue,
      .size = 0,
      .data = nullptr,
    });

    return {};
  }

  Result<> QueryCompiler::compileVariable(VariableExpression const& var)
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
        _plan.instructions.push_back(Instruction{
          .op = OpCode::LoadField,
          .field = static_cast<std::uint8_t>(Field::Tag),
          .operand = static_cast<std::int32_t>(_nextReg++),
          .constValue = 0,
          .size = 0,
          .data = nullptr,
        });

        // Then load the tag ID as constant
        _plan.instructions.push_back(Instruction{
          .op = OpCode::LoadConstant,
          .field = 0,
          .operand = static_cast<std::int32_t>(_nextReg++),
          .constValue = static_cast<std::int64_t>(tagId.raw()),
          .size = 0,
          .data = nullptr,
        });

        // Eq instruction - PlanEvaluator will detect Tag field and use tags.has()
        _plan.instructions.push_back(Instruction{
          .op = OpCode::Eq,
          .field = 0,
          .operand = static_cast<std::int32_t>(_nextReg - 1), // Right operand
          .constValue = 0,
          .size = 0,
          .data = nullptr,
        });

        _nextReg--; // Eq result is in the left register
        return {};
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

    _plan.instructions.push_back(Instruction{
      .op = OpCode::LoadField,
      .field = static_cast<std::uint8_t>(field),
      .operand = static_cast<std::int32_t>(_nextReg++),
      .constValue = constValue,
      .size = 0,
      .data = nullptr,
    });

    return {};
  }

  Result<> QueryCompiler::compileConstant(ConstantExpression const& constant)
  {
    return std::visit(utility::makeVisitor(
                        [this](bool val) -> Result<>
                        {
                          _plan.instructions.push_back(Instruction{
                            .op = OpCode::LoadConstant,
                            .field = 0,
                            .operand = static_cast<std::int32_t>(_nextReg++),
                            .constValue = val ? 1 : 0,
                            .size = 0,
                            .data = nullptr,
                          });
                          return {};
                        },
                        [this](std::int64_t val) -> Result<>
                        {
                          _plan.instructions.push_back(Instruction{
                            .op = OpCode::LoadConstant,
                            .field = 0,
                            .operand = static_cast<std::int32_t>(_nextReg++),
                            .constValue = val,
                            .size = 0,
                            .data = nullptr,
                          });
                          return {};
                        },
                        [this](UnitConstantExpression const& val) -> Result<>
                        {
                          auto const scaled = scaleUnitConstant(val, _lastField);

                          if (!scaled)
                          {
                            return std::unexpected{scaled.error()};
                          }

                          _plan.instructions.push_back(Instruction{
                            .op = OpCode::LoadConstant,
                            .field = 0,
                            .operand = static_cast<std::int32_t>(_nextReg++),
                            .constValue = *scaled,
                            .size = 0,
                            .data = nullptr,
                          });
                          return {};
                        },
                        [this](std::string const& val) -> Result<>
                        {
                          if (_lastField == Field::Codec)
                          {
                            if (auto const optCodec = parseAudioCodecName(val); optCodec)
                            {
                              _plan.instructions.push_back(Instruction{
                                .op = OpCode::LoadConstant,
                                .field = 0,
                                .operand = static_cast<std::int32_t>(_nextReg++),
                                .constValue = audioCodecStorageValue(*optCodec),
                                .size = 0,
                                .data = nullptr,
                              });
                              return {};
                            }

                            return rejectQuery("unknown audio codec '{}'", val);
                          }

                          // Check if we should resolve this string via dictionary
                          // For metadata ID fields (artist, album, genre), resolve to numeric ID
                          auto const resolvedId = resolveStringConstant(val, _lastField);

                          if (resolvedId >= 0)
                          {
                            // Successfully resolved to ID - store as numeric constant
                            _plan.instructions.push_back(Instruction{
                              .op = OpCode::LoadConstant,
                              .field = 0,
                              .operand = static_cast<std::int32_t>(_nextReg++),
                              .constValue = resolvedId,
                              .size = 0,
                              .data = nullptr,
                            });
                          }
                          else
                          {
                            // Not resolved (no dictionary or not a metadata ID field) - store as string constant
                            auto const idx = addStringConstant(val);
                            _plan.instructions.push_back(Instruction{
                              .op = OpCode::LoadConstant,
                              .field = 0,
                              .operand = static_cast<std::int32_t>(_nextReg++),
                              .constValue = static_cast<std::int64_t>(idx),
                              .size = static_cast<std::uint32_t>(val.size()),
                              .data = nullptr,
                            });
                          }

                          return {};
                        }),
                      constant);
  }

  Result<> QueryCompiler::compileList(ListExpression const& /*list*/)
  {
    return rejectQuery("list expressions are only supported as the right operand of 'in'");
  }

  Result<> QueryCompiler::compileRange(RangeExpression const& /*range*/)
  {
    return rejectQuery("range expressions are only supported as the right operand of 'in'");
  }

  Result<> QueryCompiler::compileIn(Expression const& lhs, Expression const& rhs)
  {
    if (auto const* list = std::get_if<ListExpression>(&rhs); list != nullptr)
    {
      if (list->values.empty())
      {
        return rejectQuery("operator 'in' expects a non-empty list");
      }

      auto const inSetResult = list->values.size() >= kInSetCompilationThreshold
                                 ? compileInSetList(lhs, *list)
                                 : Result<InSetCompileStatus>{InSetCompileStatus::NotApplicable};

      if (!inSetResult)
      {
        return std::unexpected{inSetResult.error()};
      }

      if (*inSetResult == InSetCompileStatus::Compiled)
      {
        return {};
      }

      bool first = true;

      for (auto const& value : list->values)
      {
        if (auto result = compileExpression(lhs); !result)
        {
          return result;
        }

        if (auto result = compileConstant(value); !result)
        {
          return result;
        }

        auto const rightReg = _nextReg - 1;
        _plan.instructions.push_back(Instruction{
          .op = OpCode::Eq,
          .field = 0,
          .operand = static_cast<std::int32_t>(rightReg),
          .constValue = 0,
          .size = 0,
          .data = nullptr,
        });
        _nextReg--;

        if (first)
        {
          first = false;
          continue;
        }

        auto const rhsReg = _nextReg - 1;
        _plan.instructions.push_back(Instruction{
          .op = OpCode::Or,
          .field = 0,
          .operand = static_cast<std::int32_t>(rhsReg),
          .constValue = 0,
          .size = 0,
          .data = nullptr,
        });
        _nextReg--;
      }

      return {};
    }

    auto const* range = std::get_if<RangeExpression>(&rhs);

    if (range == nullptr)
    {
      return rejectQuery("operator 'in' expects a list or range right operand");
    }

    if (auto result = compileExpression(lhs); !result)
    {
      return result;
    }

    // A range compiles to Ge/Le bounds, i.e. ordered comparisons. For dictionary
    // fields those must compare resolved text, so require string bounds and keep
    // them as string constants (see compileBinary / executeComparison).
    auto const dictionaryBounds = isDictionaryField(_lastField);

    if (dictionaryBounds && (!isStringConstant(range->lower) || !isStringConstant(range->upper)))
    {
      return rejectQuery("range over the '{}' field requires string bounds", fieldDisplayName(_lastField));
    }

    auto const previousResolveStringConstantsToIds = _resolveStringConstantsToIds;
    auto restoreResolveMode =
      gsl_lite::finally([this, previousResolveStringConstantsToIds]
                        { _resolveStringConstantsToIds = previousResolveStringConstantsToIds; });

    if (dictionaryBounds)
    {
      _resolveStringConstantsToIds = false;
    }

    if (auto result = compileConstant(range->lower); !result)
    {
      return result;
    }

    auto const lowerReg = _nextReg - 1;
    _plan.instructions.push_back(Instruction{
      .op = OpCode::Ge,
      .field = 0,
      .operand = static_cast<std::int32_t>(lowerReg),
      .constValue = 0,
      .size = 0,
      .data = nullptr,
    });
    _nextReg--;

    if (auto result = compileExpression(lhs); !result)
    {
      return result;
    }

    if (auto result = compileConstant(range->upper); !result)
    {
      return result;
    }

    auto const upperReg = _nextReg - 1;
    _plan.instructions.push_back(Instruction{
      .op = OpCode::Le,
      .field = 0,
      .operand = static_cast<std::int32_t>(upperReg),
      .constValue = 0,
      .size = 0,
      .data = nullptr,
    });
    _nextReg--;

    auto const rhsReg = _nextReg - 1;
    _plan.instructions.push_back(Instruction{
      .op = OpCode::And,
      .field = 0,
      .operand = static_cast<std::int32_t>(rhsReg),
      .constValue = 0,
      .size = 0,
      .data = nullptr,
    });
    _nextReg--;

    return {};
  }

  Result<QueryCompiler::InSetCompileStatus> QueryCompiler::compileInSetList(Expression const& lhs,
                                                                            ListExpression const& list)
  {
    auto const* variable = std::get_if<VariableExpression>(&lhs);

    if (variable == nullptr)
    {
      return InSetCompileStatus::NotApplicable;
    }

    auto const fieldResult = resolveVariableField(*variable);

    if (!fieldResult)
    {
      return std::unexpected{fieldResult.error()};
    }

    auto const field = *fieldResult;

    if (isTagField(field))
    {
      return InSetCompileStatus::NotApplicable;
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
        return InSetCompileStatus::NotApplicable;
      }
    }

    if (auto compileResult = compileExpression(lhs); !compileResult)
    {
      return std::unexpected{compileResult.error()};
    }

    auto const setIndex = addInSet(std::move(set));
    auto const leftReg = _nextReg - 1;
    _plan.instructions.push_back(Instruction{
      .op = OpCode::InSet,
      .field = 0,
      .operand = static_cast<std::int32_t>(leftReg),
      .constValue = static_cast<std::int64_t>(setIndex),
      .size = 0,
      .data = nullptr,
    });

    return InSetCompileStatus::Compiled;
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

  void ExecutionPlan::indexFieldLoads()
  {
    fieldLoadIndex.assign(instructions.size(), -1);

    for (std::int32_t lastLoadField = -1; auto const idx : std::views::iota(0UZ, instructions.size()))
    {
      fieldLoadIndex[idx] = lastLoadField;

      if (instructions[idx].op == OpCode::LoadField)
      {
        lastLoadField = static_cast<std::int32_t>(idx);
      }
    }
  }

  Result<ExecutionPlan> QueryCompiler::compile(Expression const& expr)
  {
    _plan = ExecutionPlan{};
    _plan.dictionary = _dict;
    _plan.tagBloomMask = computeRequiredTagBloomMask(expr, _dict);
    _nextReg = 0;
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

    _plan.indexFieldLoads();

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
