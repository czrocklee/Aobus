// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/Exception.h>
#include <rs/expr/ExecutionPlan.h>
#include <rs/utility/VariantVisitor.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <exception>
#include <limits>
#include <ranges>
#include <string>

namespace rs::expr
{
  namespace
  {
    // Bloom filter uses 5 bits per tag (bit mask 31 = 0x1F)
    constexpr std::uint32_t kBloomBitMask = 31;

#include "expr/MetadataDispatch.h"
#include "expr/PropertyDispatch.h"
#include "expr/UnitDispatch.h"

    Field variableTypeToField(VariableType type, std::string_view name)
    {
      switch (type)
      {
        case VariableType::Property:
        {
          if (auto const* entry = property_dispatch::Table::lookupPropertyField(name.data(), name.size());
              entry != nullptr)
          {
            return entry->field;
          }

          break;
        }
        case VariableType::Metadata:
        {
          if (auto const* entry = metadata_dispatch::Table::lookupMetadataField(name.data(), name.size());
              entry != nullptr)
          {
            return entry->field;
          }

          break;
        }
        case VariableType::Tag: return Field::Tag;
        case VariableType::Custom: return Field::Custom;
        default: break;
      }
      return Field::TagBloom;
    }

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
        default: return OpCode::Nop;
      }
    }

    bool isDictionaryField(Field field)
    {
      switch (field)
      {
        case Field::ArtistId:
        case Field::AlbumId:
        case Field::GenreId:
        case Field::AlbumArtistId:
        case Field::ComposerId:
        case Field::WorkId: return true;
        default: return false;
      }
    }

    bool isTagField(Field field)
    {
      return field == Field::Tag;
    }

    bool isUnsupportedLikeField(Field field)
    {
      return field == Field::CoverArtId || field == Field::Tag;
    }

    bool isColdField(Field field)
    {
      // Cold fields are stored in TrackColdHeader or custom KV storage
      switch (field)
      {
        case Field::Uri:         // cold: TrackColdHeader
        case Field::CoverArtId:  // cold: TrackColdHeader
        case Field::WorkId:      // cold: TrackColdHeader
        case Field::TrackNumber: // cold: TrackColdHeader
        case Field::TotalTracks: // cold: TrackColdHeader
        case Field::DiscNumber:  // cold: TrackColdHeader
        case Field::TotalDiscs:  // cold: TrackColdHeader
        case Field::Custom:      // cold: custom KV storage
        case Field::DurationMs:  // cold: TrackColdHeader
        case Field::Bitrate:     // cold: TrackColdHeader
        case Field::SampleRate:  // cold: TrackColdHeader
        case Field::Channels:    // cold: TrackColdHeader
          return true;
        default: return false;
      }
    }

    std::string toLower(std::string_view value)
    {
      return value | std::views::transform([](unsigned char ch) { return static_cast<char>(std::tolower(ch)); }) |
             std::ranges::to<std::string>();
    }

    char const* fieldName(Field field)
    {
      switch (field)
      {
        case Field::DurationMs: return "duration";
        case Field::Bitrate: return "bitrate";
        case Field::SampleRate: return "sampleRate";
        case Field::Channels: return "channels";
        case Field::BitDepth: return "bitDepth";
        case Field::Year: return "year";
        case Field::TrackNumber: return "trackNumber";
        case Field::TotalTracks: return "totalTracks";
        case Field::DiscNumber: return "discNumber";
        case Field::TotalDiscs: return "totalDiscs";
        default: return "field";
      }
    }

    std::optional<std::uint64_t> parseUnsigned(std::string_view value)
    {
      std::uint64_t parsed = 0;

      if (auto const [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
          ec != std::errc{} || ptr != value.data() + value.size())
      {
        return std::nullopt;
      }

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
        auto const next = checkedMul(value, 10);

        if (!next)
        {
          return std::nullopt;
        }

        value = *next;
      }

      return value;
    }

    std::uint64_t unitMultiplier(Field field, std::string_view unit)
    {
      auto const normalized = toLower(unit);

      switch (field)
      {
        case Field::DurationMs:
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

      RS_THROW_FORMAT(rs::Exception, "unit '{}' is not supported for {} constants", normalized, fieldName(field));
    }

    std::int64_t scaleUnitConstant(UnitConstantExpression const& constant, Field field)
    {
      if (field == Field::TagBloom)
      {
        RS_THROW_FORMAT(rs::Exception, "unit literal '{}' requires a numeric field context", constant.lexeme);
      }

      auto lexeme = std::string_view{constant.lexeme};
      auto const negative = !lexeme.empty() && lexeme.front() == '-';

      if (negative)
      {
        lexeme.remove_prefix(1);
      }

      auto const suffixStart = std::ranges::find_if(lexeme, [](unsigned char ch) { return std::isalpha(ch) != 0; });

      if (suffixStart == lexeme.end())
      {
        RS_THROW_FORMAT(rs::Exception, "invalid unit literal '{}'", constant.lexeme);
      }

      auto const suffixOffset = static_cast<std::size_t>(std::distance(lexeme.begin(), suffixStart));
      auto const numberPart = lexeme.substr(0, suffixOffset);
      auto const suffixPart = lexeme.substr(suffixOffset);

      if (numberPart.empty() || suffixPart.empty())
      {
        RS_THROW_FORMAT(rs::Exception, "invalid unit literal '{}'", constant.lexeme);
      }

      auto const dotPos = numberPart.find('.');

      if (dotPos != std::string_view::npos && numberPart.find('.', dotPos + 1) != std::string_view::npos)
      {
        RS_THROW_FORMAT(rs::Exception, "invalid unit literal '{}'", constant.lexeme);
      }

      auto const wholePart = numberPart.substr(0, dotPos);
      auto const fractionPart = dotPos == std::string_view::npos ? std::string_view{} : numberPart.substr(dotPos + 1);

      if (wholePart.empty() || (dotPos != std::string_view::npos && fractionPart.empty()))
      {
        RS_THROW_FORMAT(rs::Exception, "invalid unit literal '{}'", constant.lexeme);
      }

      auto const whole = parseUnsigned(wholePart);
      auto const fraction =
        fractionPart.empty() ? std::optional<std::uint64_t>{std::uint64_t{0}} : parseUnsigned(fractionPart);
      auto const denominator = pow10(fractionPart.size());

      if (!whole || !fraction || !denominator)
      {
        RS_THROW_FORMAT(rs::Exception, "invalid unit literal '{}'", constant.lexeme);
      }

      auto const scaledWhole = checkedMul(*whole, *denominator);
      auto const numerator = scaledWhole ? checkedAdd(*scaledWhole, *fraction) : std::nullopt;
      auto const multiplier = unitMultiplier(field, suffixPart);
      auto const scaledNumerator = numerator ? checkedMul(*numerator, multiplier) : std::nullopt;

      if (!scaledNumerator)
      {
        RS_THROW_FORMAT(rs::Exception, "unit literal '{}' is out of range", constant.lexeme);
      }

      if (*scaledNumerator % *denominator != 0)
      {
        RS_THROW_FORMAT(rs::Exception,
                        "unit literal '{}' does not resolve to an integer {} value",
                        constant.lexeme,
                        fieldName(field));
      }

      auto const magnitude = *scaledNumerator / *denominator;

      if (!negative)
      {
        if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
          RS_THROW_FORMAT(rs::Exception, "unit literal '{}' is out of range", constant.lexeme);
        }

        return static_cast<std::int64_t>(magnitude);
      }

      auto const negativeLimit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;

      if (magnitude > negativeLimit)
      {
        RS_THROW_FORMAT(rs::Exception, "unit literal '{}' is out of range", constant.lexeme);
      }

      if (magnitude == negativeLimit)
      {
        return std::numeric_limits<std::int64_t>::min();
      }

      return -static_cast<std::int64_t>(magnitude);
    }
  }

  QueryCompiler::QueryCompiler(core::DictionaryStore* dict)
    : _dict{dict}
  {
  }

  std::uint32_t QueryCompiler::addStringConstant(std::string_view str)
  {
    if (auto it = std::ranges::find(_plan.stringConstants, str); it != _plan.stringConstants.end())
    {
      return static_cast<std::uint32_t>(std::distance(_plan.stringConstants.begin(), it));
    }

    _plan.stringConstants.emplace_back(str);
    return static_cast<std::uint32_t>(_plan.stringConstants.size() - 1);
  }

  void QueryCompiler::compileExpression(Expression const& expr)
  {
    std::visit(utility::makeVisitor([this](std::unique_ptr<BinaryExpression> const& binary) { compileBinary(*binary); },
                                    [this](std::unique_ptr<UnaryExpression> const& unary) { compileUnary(*unary); },
                                    [this](VariableExpression const& var) { compileVariable(var); },
                                    [this](ConstantExpression const& constant) { compileConstant(constant); }),
               expr);
  }

  void QueryCompiler::compileBinary(BinaryExpression const& binary)
  {
    // Compile left operand
    compileExpression(binary.operand);

    // Save left field before compiling right operand (which will overwrite _lastField)
    auto const leftField = _lastField;

    if (binary.operation)
    {
      auto opcode = toOpCode(binary.operation->op);

      if (opcode == OpCode::Like && isUnsupportedLikeField(leftField))
      {
        RS_THROW(rs::Exception, "LIKE operator not supported for coverArt or tags");
      }

      auto const previousResolveStringConstantsToIds = _resolveStringConstantsToIds;

      if (opcode == OpCode::Like && isDictionaryField(leftField))
      {
        _resolveStringConstantsToIds = false;
      }

      // Compile right operand
      compileExpression(binary.operation->operand);
      _resolveStringConstantsToIds = previousResolveStringConstantsToIds;

      // Right operand result is in _nextReg - 1
      // Binary op will store result in operand - 1 = (_nextReg - 1) - 1 = _nextReg - 2
      auto rightReg = _nextReg - 1;

      // Store leftField in field for LIKE instructions so executeLike can use it directly
      auto instrField = (opcode == OpCode::Like) ? static_cast<std::uint8_t>(leftField) : std::uint8_t{0};

      _plan.instructions.push_back(Instruction{
        .op = opcode,
        .field = instrField,
        .operand = static_cast<std::int32_t>(rightReg),
        .constValue = 0,
        .strLen = 0,
        .strData = nullptr,
      });

      // After binary op, the result is stored in the left register (rightReg - 1)
      // The right register is now free, so decrement _nextReg
      _nextReg--;
    }
  }

  void QueryCompiler::compileUnary(UnaryExpression const& unary)
  {
    compileExpression(unary.operand);

    auto opcode = toOpCode(unary.op);
    _plan.instructions.push_back(Instruction{
      .op = opcode,
      .field = 0,
      .operand = static_cast<std::int32_t>(_nextReg - 1),
      .constValue = 0,
      .strLen = 0,
      .strData = nullptr,
    });
  }

  void QueryCompiler::compileVariable(VariableExpression const& var)
  {
    // Tags are hot data
    
    if (var.type == VariableType::Tag)
    {
      _hasHotAccess = true;
      // Try to resolve tag name to ID via dictionary for bloom filter
      
      if (_dict != nullptr)
      {
        auto tagId = _dict->getId(var.name);
        // getId throws if not found, so any returned ID is valid (including 0)
        _plan.tagBloomMask |= (std::uint32_t{1} << (tagId.value() & kBloomBitMask));

        // Generate implicit tag comparison: track.tags().has(tagId)
        // This handles standalone "#tagname" queries like "#rock"
        // First, load the tag field (for the Eq instruction to detect it's a tag comparison)
        _plan.instructions.push_back(Instruction{
          .op = OpCode::LoadField,
          .field = static_cast<std::uint8_t>(Field::Tag),
          .operand = static_cast<std::int32_t>(_nextReg++),
          .constValue = 0,
          .strLen = 0,
          .strData = nullptr,
        });

        // Then load the tag ID as constant
        _plan.instructions.push_back(Instruction{
          .op = OpCode::LoadConstant,
          .field = 0,
          .operand = static_cast<std::int32_t>(_nextReg++),
          .constValue = static_cast<std::int64_t>(tagId.value()),
          .strLen = 0,
          .strData = nullptr,
        });

        // Eq instruction - PlanEvaluator will detect Tag field and use tags.has()
        _plan.instructions.push_back(Instruction{
          .op = OpCode::Eq,
          .field = 0,
          .operand = static_cast<std::int32_t>(_nextReg - 1), // Right operand
          .constValue = 0,
          .strLen = 0,
          .strData = nullptr,
        });

        _nextReg--; // Eq result is in the left register
        return;
      }
    }

    auto field = variableTypeToField(var.type, var.name);
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
        try
        {
          auto dictId = _dict->getId(var.name);
          constValue = static_cast<std::int64_t>(dictId.value());
        }
        catch (std::exception const&)
        {
          constValue = 0; // Key not found - will return empty string at evaluation
        }
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
      .strLen = 0,
      .strData = nullptr,
    });
  }

  void QueryCompiler::compileConstant(ConstantExpression const& constant)
  {
    std::visit(utility::makeVisitor(
                 [this](bool val)
                 {
                   _plan.instructions.push_back(Instruction{
                     .op = OpCode::LoadConstant,
                     .field = 0,
                     .operand = static_cast<std::int32_t>(_nextReg++),
                     .constValue = val ? 1 : 0,
                     .strLen = 0,
                     .strData = nullptr,
                   });
                 },
                 [this](std::int64_t val)
                 {
                   _plan.instructions.push_back(Instruction{
                     .op = OpCode::LoadConstant,
                     .field = 0,
                     .operand = static_cast<std::int32_t>(_nextReg++),
                     .constValue = val,
                     .strLen = 0,
                     .strData = nullptr,
                   });
                 },
                 [this](UnitConstantExpression const& val)
                 {
                   auto const scaled = scaleUnitConstant(val, _lastField);
                   _plan.instructions.push_back(Instruction{
                     .op = OpCode::LoadConstant,
                     .field = 0,
                     .operand = static_cast<std::int32_t>(_nextReg++),
                     .constValue = scaled,
                     .strLen = 0,
                     .strData = nullptr,
                   });
                 },
                 [this](std::string const& val)
                 {
                   // Check if we should resolve this string via dictionary
                   // For metadata ID fields (artist, album, genre), resolve to numeric ID
                   auto resolvedId = resolveStringConstant(val, _lastField);

                   if (resolvedId >= 0)
                   {
                     // Successfully resolved to ID - store as numeric constant
                     _plan.instructions.push_back(Instruction{
                       .op = OpCode::LoadConstant,
                       .field = 0,
                       .operand = static_cast<std::int32_t>(_nextReg++),
                       .constValue = resolvedId,
                       .strLen = 0,
                       .strData = nullptr,
                     });
                   }
                   else
                   {
                     // Not resolved (no dictionary or not a metadata ID field) - store as string constant
                     auto idx = addStringConstant(val);
                     _plan.instructions.push_back(Instruction{
                       .op = OpCode::LoadConstant,
                       .field = 0,
                       .operand = static_cast<std::int32_t>(_nextReg++),
                       .constValue = static_cast<std::int64_t>(idx),
                       .strLen = static_cast<std::uint32_t>(val.size()),
                       .strData = nullptr,
                     });
                   }
                 
                 }),
               constant);
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
    auto id = _dict->reserve(str);
    return static_cast<std::int64_t>(id.value());
  }

  ExecutionPlan QueryCompiler::compile(Expression const& expr)
  {
    _plan = ExecutionPlan{};
    _plan.dictionary = _dict;
    _nextReg = 0;
    _hasHotAccess = false;
    _hasColdAccess = false;
    _resolveStringConstantsToIds = true;

    // Check if the expression is a constant "true"
    
    if (auto const* constant = std::get_if<ConstantExpression>(&expr))
    {
      if (bool const* val = std::get_if<bool>(constant))
      {
        if (*val)
        {
          _plan.matchesAll = true;
        }
      }
    }

    compileExpression(expr);

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

} // namespace rs::expr
