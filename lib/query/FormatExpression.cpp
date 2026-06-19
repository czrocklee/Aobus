// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Exception.h>
#include <ao/Type.h>
#include <ao/library/AudioCodec.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FormatExpression.h>
#include <ao/utility/VariantVisitor.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cstdint>
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

    std::string_view loadDictionaryFieldValue(library::TrackView const& track,
                                              Field field,
                                              library::DictionaryStore const* dict)
    {
      if (dict == nullptr)
      {
        return {};
      }

      auto dictionaryId = kInvalidDictionaryId;

      switch (field)
      {
        case Field::ArtistId: dictionaryId = track.metadata().artistId(); break;
        case Field::AlbumId: dictionaryId = track.metadata().albumId(); break;
        case Field::GenreId: dictionaryId = track.metadata().genreId(); break;
        case Field::AlbumArtistId: dictionaryId = track.metadata().albumArtistId(); break;
        case Field::ComposerId: dictionaryId = track.metadata().composerId(); break;
        case Field::WorkId: dictionaryId = track.metadata().workId(); break;
        default: return {};
      }

      if (dictionaryId == kInvalidDictionaryId)
      {
        return {};
      }

      return dict->get(dictionaryId);
    }

    template<typename T>
    std::string decimalTextOrEmpty(T value)
    {
      if (value == 0)
      {
        return {};
      }

      return std::format("{}", value);
    }

    std::string loadFieldText(library::TrackView const& track,
                              FormatInstruction const& instr,
                              library::DictionaryStore const* dict)
    {
      auto const field = instr.field;

      if (isDictionaryField(field))
      {
        return std::string{loadDictionaryFieldValue(track, field, dict)};
      }

      switch (field)
      {
        case Field::Title: return std::string{track.metadata().title()};
        case Field::Uri: return std::string{track.property().uri()};
        case Field::Custom:
        {
          if (instr.constValue <= 0)
          {
            return {};
          }

          auto const dictId = DictionaryId{static_cast<std::uint32_t>(instr.constValue)};
          return std::string{track.customMetadata().get(dictId).value_or("")};
        }
        case Field::Year: return decimalTextOrEmpty(track.metadata().year());
        case Field::TrackNumber: return decimalTextOrEmpty(track.metadata().trackNumber());
        case Field::TrackTotal: return decimalTextOrEmpty(track.metadata().trackTotal());
        case Field::DiscNumber: return decimalTextOrEmpty(track.metadata().discNumber());
        case Field::DiscTotal: return decimalTextOrEmpty(track.metadata().discTotal());
        case Field::Duration: return decimalTextOrEmpty(track.property().duration().count());
        case Field::Bitrate: return decimalTextOrEmpty(track.property().bitrate().raw());
        case Field::SampleRate: return decimalTextOrEmpty(track.property().sampleRate().raw());
        case Field::Channels: return decimalTextOrEmpty(track.property().channels().raw());
        case Field::BitDepth: return decimalTextOrEmpty(track.property().bitDepth().raw());
        case Field::Codec:
          return track.property().codec() == library::AudioCodec::Unknown
                   ? std::string{}
                   : std::string{library::audioCodecName(track.property().codec())};
        default: return {};
      }
    }
  } // namespace

  FormatCompiler::FormatCompiler(library::DictionaryStore* dict)
    : _dict{dict}
  {
    gsl_Expects(dict != nullptr);
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
                 [this](std::unique_ptr<BinaryExpression> const& binary)
                 {
                   gsl_Expects(binary != nullptr);
                   compileBinary(*binary);
                 },
                 [](std::unique_ptr<UnaryExpression> const&)
                 { throwException<Exception>("format expressions do not support unary operators"); },
                 [this](VariableExpression const& variable) { compileVariable(variable); },
                 [this](ConstantExpression const& constant) { compileConstant(constant); },
                 [](ListExpression const&) { throwException<Exception>("format expressions do not support lists"); },
                 [](RangeExpression const&) { throwException<Exception>("format expressions do not support ranges"); }),
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
      throwException<Exception>("format expressions only support '+' concatenation");
    }

    compileExpression(binary.operand);
    compileExpression(binary.optOperation->operand);
  }

  void FormatCompiler::compileVariable(VariableExpression const& variable)
  {
    auto const field = resolveVariableField(variable);

    if (isUnsupportedScalarField(field))
    {
      throwException<Exception>("field '{}' cannot be formatted as a scalar string", variable.name);
    }

    if (_dict == nullptr && (isDictionaryField(field) || variable.type == VariableType::Custom))
    {
      throwException<Exception>("format field '{}' requires a DictionaryStore", variableDisplayName(variable));
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
      auto const dictId = _dict->getOrIntern(variable.name);
      constValue = static_cast<std::int64_t>(dictId.raw());
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
    auto literal =
      std::visit(utility::makeVisitor([](bool value) { return value ? std::string{"true"} : std::string{"false"}; },
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

  FormatPlan FormatCompiler::compile(Expression const& expr)
  {
    _plan = FormatPlan{};
    _plan.dictionary = _dict;
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

  std::string FormatEvaluator::evaluate(FormatPlan const& plan, library::TrackView const& track) const
  {
    if (!hasRequiredTrackData(plan.accessProfile, track))
    {
      return {};
    }

    auto output = std::string{};

    for (auto const& instr : plan.instructions)
    {
      switch (instr.op)
      {
        case FormatOpCode::AppendLiteral:
          if (instr.literalIndex < plan.literals.size())
          {
            output += plan.literals[instr.literalIndex];
          }

          break;
        case FormatOpCode::AppendField: output += loadFieldText(track, instr, plan.dictionary); break;
      }
    }

    return output;
  }
} // namespace ao::query
