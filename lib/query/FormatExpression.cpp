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
#include <string>
#include <string_view>
#include <variant>

namespace ao::query
{
  namespace
  {
    bool isUnsupportedScalarField(Field field)
    {
      return field == Field::Tag || field == Field::TagBloom || field == Field::TagCount || field == Field::CoverArtId;
    }

    std::string_view readDictionaryFieldValue(library::TrackView const& track,
                                              Field field,
                                              library::DictionaryStore const* dictionary)
    {
      if (dictionary == nullptr)
      {
        return {};
      }

      return dictionaryFieldValue(track, field, *dictionary);
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
                         library::DictionaryStore const* dictionary)
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
          if (instr.constValue > 0)
          {
            auto const dictionaryId = DictionaryId{static_cast<std::uint32_t>(instr.constValue)};

            if (auto const optValue = track.customMetadata().get(dictionaryId); optValue)
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

  FormatCompiler::FormatCompiler(library::DictionaryStore* dictionary)
    : _dictionary{dictionary}
  {
    gsl_Expects(dictionary != nullptr);
  }

  std::uint32_t FormatCompiler::addLiteral(std::string_view value)
  {
    if (auto const it = std::ranges::find(_plan.literals, value); it != _plan.literals.end())
    {
      return static_cast<std::uint32_t>(std::distance(_plan.literals.begin(), it));
    }

    _plan.literals.emplace_back(value);
    return static_cast<std::uint32_t>(_plan.literals.size() - 1);
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

    if (_dictionary == nullptr && (isDictionaryField(field) || variable.type == VariableType::Custom))
    {
      detail::throwQueryError("format field '{}' requires a DictionaryStore", variableDisplayName(variable));
    }

    if (isColdField(field))
    {
      _hasColdAccess = true;
    }
    else
    {
      _hasHotAccess = true;
    }

    std::int64_t constValue = 0;

    if (variable.type == VariableType::Custom)
    {
      auto const dictionaryId = _dictionary->getOrIntern(variable.name);
      constValue = static_cast<std::int64_t>(dictionaryId.raw());
    }

    _plan.instructions.push_back(FormatInstruction{
      .op = FormatOpCode::AppendField,
      .field = field,
      .constValue = constValue,
      .literalIndex = 0,
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
      .constValue = 0,
      .literalIndex = addLiteral(literal),
    });
  }

  Result<FormatPlan> FormatCompiler::compile(Expression const& expr)

  try
  {
    _plan = FormatPlan{};
    _plan.dictionary = _dictionary;
    _hasHotAccess = false;
    _hasColdAccess = false;

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

    return _plan;
  }
  catch (detail::QueryException const& ex)
  {
    return std::unexpected{ex.error()};
  }

  Result<FormatPlan> compileFormat(Expression const& expr, library::DictionaryStore* dictionary)
  {
    auto compiler = dictionary != nullptr ? FormatCompiler{dictionary} : FormatCompiler{};
    return compiler.compile(expr);
  }

  std::string FormatEvaluator::evaluate(FormatPlan const& plan, library::TrackView const& track) const
  {
    auto output = std::string{};
    evaluate(plan, track, output);
    return output;
  }

  void FormatEvaluator::evaluate(FormatPlan const& plan, library::TrackView const& track, std::string& output) const
  {
    output.clear();

    if (!hasRequiredTrackData(plan.accessProfile, track))
    {
      return;
    }

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
        case FormatOpCode::AppendField: appendFieldText(output, track, instr, plan.dictionary); break;
      }
    }
  }
} // namespace ao::query
