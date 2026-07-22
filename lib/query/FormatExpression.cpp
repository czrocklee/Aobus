// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FormatExpression.h>
#include <ao/query/detail/FieldResolver.h>
#include <ao/query/detail/QueryError.h>
#include <ao/utility/VariantVisitor.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace ao::query
{
  namespace
  {
    bool isUnsupportedScalarField(Field field)
    {
      return field == Field::Tag || field == Field::TagBloom || field == Field::TagCount || field == Field::CoverArtId;
    }

    DictionaryId boundDictionaryId(std::span<DictionaryId const> ids, std::uint32_t symbol)
    {
      if (symbol == kNoFormatDictionarySymbol || symbol >= ids.size())
      {
        return kInvalidDictionaryId;
      }

      return ids[symbol];
    }

    std::string_view readDictionaryFieldValue(library::TrackView const& track,
                                              Field field,
                                              library::DictionaryReadContext* dictionary)
    {
      if (dictionary == nullptr)
      {
        return {};
      }

      auto const id = dictionaryFieldId(track, field);
      return id == kInvalidDictionaryId ? std::string_view{} : dictionary->get(id);
    }

    template<typename T>
    void appendDecimalText(std::string& output, T value)
    {
      if (value != 0)
      {
        std::format_to(std::back_inserter(output), "{}", value);
      }
    }

    void appendFieldText(std::string& output,
                         library::TrackView const& track,
                         FormatInstruction const& instr,
                         library::DictionaryReadContext* dictionary,
                         std::span<DictionaryId const> dictionaryIds)
    {
      auto const field = instr.field;

      if (isDictionaryField(field))
      {
        output.append(readDictionaryFieldValue(track, field, dictionary));
        return;
      }

      switch (field)
      {
        case Field::Title: output.append(track.metadata().title()); break;
        case Field::Uri: output.append(track.property().uri()); break;
        case Field::Custom:
        {
          auto const keyId = boundDictionaryId(dictionaryIds, instr.dictionarySymbol);

          if (keyId != kInvalidDictionaryId)
          {
            if (auto const optValue = track.customMetadata().get(keyId); optValue)
            {
              output.append(*optValue);
            }
          }

          break;
        }
        case Field::Year: appendDecimalText(output, track.metadata().year()); break;
        case Field::TrackNumber: appendDecimalText(output, track.metadata().trackNumber()); break;
        case Field::TrackTotal: appendDecimalText(output, track.metadata().trackTotal()); break;
        case Field::DiscNumber: appendDecimalText(output, track.metadata().discNumber()); break;
        case Field::DiscTotal: appendDecimalText(output, track.metadata().discTotal()); break;
        case Field::MovementNumber: appendDecimalText(output, track.classical().movementNumber()); break;
        case Field::MovementTotal: appendDecimalText(output, track.classical().movementTotal()); break;
        case Field::Duration: appendDecimalText(output, track.property().duration().count()); break;
        case Field::Bitrate: appendDecimalText(output, track.property().bitrate().raw()); break;
        case Field::SampleRate: appendDecimalText(output, track.property().sampleRate().raw()); break;
        case Field::Channels: appendDecimalText(output, track.property().channels().raw()); break;
        case Field::BitDepth: appendDecimalText(output, track.property().bitDepth().raw()); break;
        case Field::Codec:
          if (track.property().codec() != AudioCodec::Unknown)
          {
            output.append(audioCodecName(track.property().codec()));
          }

          break;
        default: break;
      }
    }
  } // namespace

  std::uint32_t FormatCompiler::addLiteral(std::string_view value)
  {
    if (auto const it = std::ranges::find(_plan.literals, value); it != _plan.literals.end())
    {
      return static_cast<std::uint32_t>(std::distance(_plan.literals.begin(), it));
    }

    _plan.literals.emplace_back(value);
    return static_cast<std::uint32_t>(_plan.literals.size() - 1);
  }

  std::uint32_t FormatCompiler::addDictionarySymbol(std::string_view text)
  {
    if (auto const it = std::ranges::find(_plan.dictionarySymbols, text); it != _plan.dictionarySymbols.end())
    {
      return static_cast<std::uint32_t>(std::distance(_plan.dictionarySymbols.begin(), it));
    }

    _plan.dictionarySymbols.emplace_back(text);
    return static_cast<std::uint32_t>(_plan.dictionarySymbols.size() - 1);
  }

  void FormatCompiler::compileExpression(Expression const& expr)
  {
    std::visit(utility::makeVisitor(
                 [this](std::unique_ptr<BinaryExpression> const& binaryPtr)
                 {
                   gsl_Expects(binaryPtr != nullptr);
                   compileBinary(*binaryPtr);
                 },
                 [](std::unique_ptr<UnaryExpression> const&)
                 { detail::throwQueryError("format expressions do not support unary operators"); },
                 [this](VariableExpression const& variable) { compileVariable(variable); },
                 [this](ConstantExpression const& constant) { compileConstant(constant); },
                 [](ListExpression const&) { detail::throwQueryError("format expressions do not support lists"); },
                 [](RangeExpression const&) { detail::throwQueryError("format expressions do not support ranges"); }),
               expr);
  }

  void FormatCompiler::compileBinary(BinaryExpression const& binary)
  {
    if (!binary.optOperation)
    {
      compileExpression(binary.operand);
      return;
    }

    if (binary.optOperation->op != Operator::Add)
    {
      detail::throwQueryError("format expressions only support '+' concatenation");
    }

    compileExpression(binary.operand);
    compileExpression(binary.optOperation->operand);
  }

  void FormatCompiler::compileVariable(VariableExpression const& variable)
  {
    auto const fieldResult = detail::resolveVariableField(variable);

    if (!fieldResult)
    {
      detail::throwQueryError(fieldResult.error());
    }

    auto const field = *fieldResult;

    if (isUnsupportedScalarField(field))
    {
      detail::throwQueryError("field '{}' cannot be formatted as a scalar string", variable.name);
    }

    if (isColdField(field))
    {
      _hasColdAccess = true;
    }
    else
    {
      _hasHotAccess = true;
    }

    auto dictionarySymbol = kNoFormatDictionarySymbol;

    if (variable.type == VariableType::Custom)
    {
      dictionarySymbol = addDictionarySymbol(variable.name);
      _hasDictionaryAccess = true;
    }
    else if (isDictionaryField(field))
    {
      _hasDictionaryAccess = true;
    }

    _plan.instructions.push_back(FormatInstruction{
      .op = FormatOpCode::AppendField,
      .field = field,
      .literalIndex = 0,
      .dictionarySymbol = dictionarySymbol,
    });
  }

  void FormatCompiler::compileConstant(ConstantExpression const& constant)
  {
    auto literal = std::visit(utility::makeVisitor([](bool value) -> std::string { return value ? "true" : "false"; },
                                                   [](std::int64_t value) { return std::format("{}", value); },
                                                   [](UnitConstantExpression const& value) { return value.lexeme; },
                                                   [](std::string const& value) { return value; }),
                              constant);

    _plan.instructions.push_back(FormatInstruction{
      .op = FormatOpCode::AppendLiteral,
      .field = Field::Title,
      .literalIndex = addLiteral(literal),
      .dictionarySymbol = kNoFormatDictionarySymbol,
    });
  }

  Result<FormatPlan> FormatCompiler::compile(Expression const& expr)

  try
  {
    _plan = FormatPlan{};
    _hasHotAccess = false;
    _hasColdAccess = false;
    _hasDictionaryAccess = false;

    compileExpression(expr);

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

    _plan.requiresDictionary = _hasDictionaryAccess;
    return _plan;
  }
  catch (detail::QueryException const& ex)
  {
    return std::unexpected{ex.error()};
  }

  Result<FormatPlan> compileFormat(Expression const& expr)
  {
    return FormatCompiler{}.compile(expr);
  }

  struct FormatBinding::Impl final
  {
    explicit Impl(FormatPlan const& sourcePlan, library::DictionaryReadContext* readContext)
      : plan{&sourcePlan}, dictionary{readContext}
    {
      gsl_Expects(!sourcePlan.requiresDictionary || readContext != nullptr);
      dictionaryIds.resize(sourcePlan.dictionarySymbols.size(), kInvalidDictionaryId);

      if (readContext == nullptr)
      {
        return;
      }

      std::ignore = readContext->bind(sourcePlan.dictionarySymbols, dictionaryIds);
    }

    FormatPlan const* plan;
    library::DictionaryReadContext* dictionary;
    std::vector<DictionaryId> dictionaryIds;
  };

  FormatBinding::FormatBinding(FormatPlan const& plan)
    : _implPtr{std::make_unique<Impl>(plan, nullptr)}
  {
  }

  FormatBinding::FormatBinding(FormatPlan const& plan, library::DictionaryReadContext& dictionary)
    : _implPtr{std::make_unique<Impl>(plan, &dictionary)}
  {
  }

  FormatBinding::~FormatBinding() = default;
  FormatBinding::FormatBinding(FormatBinding&&) noexcept = default;
  FormatBinding& FormatBinding::operator=(FormatBinding&&) noexcept = default;

  std::string FormatEvaluator::evaluate(FormatBinding const& binding, library::TrackView const& track) const
  {
    auto output = std::string{};
    evaluate(binding, track, output);
    return output;
  }

  std::string FormatEvaluator::evaluate(FormatPlan const& plan, library::TrackView const& track) const
  {
    gsl_Expects(!plan.requiresDictionary);
    auto const binding = FormatBinding{plan};
    return evaluate(binding, track);
  }

  void FormatEvaluator::evaluate(FormatBinding const& binding,
                                 library::TrackView const& track,
                                 std::string& output) const
  {
    auto const& state = *binding._implPtr;
    auto const& plan = *state.plan;
    gsl_Expects(hasRequiredTrackData(plan.accessProfile, track));
    output.clear();

    for (auto const& instr : plan.instructions)
    {
      switch (instr.op)
      {
        case FormatOpCode::AppendLiteral:
          if (instr.literalIndex < plan.literals.size())
          {
            output.append(plan.literals[instr.literalIndex]);
          }

          break;
        case FormatOpCode::AppendField:
          appendFieldText(output, track, instr, state.dictionary, state.dictionaryIds);
          break;
      }
    }
  }

  void FormatEvaluator::evaluate(FormatPlan const& plan, library::TrackView const& track, std::string& output) const
  {
    gsl_Expects(!plan.requiresDictionary);
    auto const binding = FormatBinding{plan};
    evaluate(binding, track, output);
  }
} // namespace ao::query
