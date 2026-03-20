// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/expr/ExecutionPlan.h>
#include <rs/utility/VariantVisitor.h>

#include <algorithm>
#include <exception>
#include <ranges>

namespace rs::expr
{

  namespace
  {
    // Bloom filter uses 5 bits per tag (bit mask 31 = 0x1F)
    constexpr std::uint32_t kBloomBitMask = 31;

    Field variableTypeToField(VariableType type, std::string_view name)
    {
      switch (type)
      {
        case VariableType::Property:
          if (name == "duration" || name == "l") return Field::DurationMs;
          if (name == "bitrate" || name == "br") return Field::Bitrate;
          if (name == "sampleRate" || name == "sr") return Field::SampleRate;
          if (name == "channels") return Field::Channels;
          if (name == "bitDepth" || name == "bd") return Field::BitDepth;
          break;
        case VariableType::Metadata:
          if (name == "year" || name == "y") return Field::Year;
          if (name == "trackNumber" || name == "tn") return Field::TrackNumber;
          if (name == "totalTracks" || name == "tt") return Field::TotalTracks;
          if (name == "discNumber" || name == "dn") return Field::DiscNumber;
          if (name == "totalDiscs" || name == "td") return Field::TotalDiscs;
          if (name == "artist" || name == "a") return Field::ArtistId;
          if (name == "album" || name == "al") return Field::AlbumId;
          if (name == "genre" || name == "g") return Field::GenreId;
          if (name == "albumArtist" || name == "aa") return Field::AlbumArtistId;
          if (name == "coverArt" || name == "ca") return Field::CoverArtId;
          if (name == "title" || name == "t") return Field::Title;
          break;
        case VariableType::Tag:
          return Field::Tag;
        case VariableType::Custom:
          return Field::Custom;
        default:
          break;
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

    bool isMetadataFieldId(Field field)
    {
      return field == Field::ArtistId || field == Field::AlbumId || field == Field::GenreId;
    }

    bool isTagField(Field field)
    {
      return field == Field::Tag;
    }

    bool isColdField(Field field)
    {
      // Cold fields are stored in TrackColdHeader or custom KV storage
      switch (field)
      {
        case Field::Uri:              // cold: TrackColdHeader
        case Field::CoverArtId:        // cold: TrackColdHeader
        case Field::TrackNumber:       // cold: TrackColdHeader
        case Field::TotalTracks:       // cold: TrackColdHeader
        case Field::DiscNumber:        // cold: TrackColdHeader
        case Field::TotalDiscs:        // cold: TrackColdHeader
        case Field::Custom:            // cold: custom KV storage
          return true;
        default:
          return false;
      }
    }
  }

  QueryCompiler::QueryCompiler(core::DictionaryStore const* dict) : _dict{dict}
  {
  }

  std::uint32_t QueryCompiler::addStringConstant(std::string_view str)
  {
    if (auto it = std::ranges::find_if(_plan.stringConstants, [str](auto const& v) { return v == str; });
        it != _plan.stringConstants.end())
    {
      return static_cast<std::uint32_t>(std::distance(_plan.stringConstants.begin(), it));
    }

    _plan.stringConstants.emplace_back(str);
    return static_cast<std::uint32_t>(_plan.stringConstants.size() - 1);
  }

  void QueryCompiler::compileExpression(Expression const& expr)
  {
    std::visit(
      utility::makeVisitor([this](std::unique_ptr<BinaryExpression> const& binary) { if (binary) compileBinary(*binary); },
                           [this](std::unique_ptr<UnaryExpression> const& unary) { if (unary) compileUnary(*unary); },
                           [this](VariableExpression const& var) { compileVariable(var); },
                           [this](ConstantExpression const& constant) { compileConstant(constant); }),
      expr);
  }

  void QueryCompiler::compileBinary(BinaryExpression const& binary)
  {
    // Compile left operand
    compileExpression(binary.operand);

    if (binary.operation)
    {
      // Compile right operand
      compileExpression(binary.operation->operand);

      // Right operand result is in _nextReg - 1
      // Binary op will store result in operand - 1 = (_nextReg - 1) - 1 = _nextReg - 2
      auto rightReg = _nextReg - 1;

      auto opcode = toOpCode(binary.operation->op);
      _plan.instructions.push_back(Instruction{
        .op = opcode,
        .field = 0,
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
      if (_dict)
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
      if (_dict)
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
                 [this](bool val) {
                   _plan.instructions.push_back(Instruction{
                     .op = OpCode::LoadConstant,
                     .field = 0,
                     .operand = static_cast<std::int32_t>(_nextReg++),
                     .constValue = val ? 1 : 0,
                     .strLen = 0,
                     .strData = nullptr,
                   });
                 },
                 [this](std::int64_t val) {
                   _plan.instructions.push_back(Instruction{
                     .op = OpCode::LoadConstant,
                     .field = 0,
                     .operand = static_cast<std::int32_t>(_nextReg++),
                     .constValue = val,
                     .strLen = 0,
                     .strData = nullptr,
                   });
                 },
                 [this](std::string const& val) {
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

  std::int64_t QueryCompiler::resolveStringConstant(std::string const& str, Field field) const
  {
    // Only resolve for metadata ID fields and tag fields
    if (!isMetadataFieldId(field) && !isTagField(field)) { return -1; }

    if (!_dict) { return -1; }

    // Look up the string in the dictionary
    auto id = _dict->getId(str);

    if (id.value() == 0)
    {
      return -1; 
    }

    return static_cast<std::int64_t>(id.value());
  }

  ExecutionPlan QueryCompiler::compile(Expression const& expr)
  {
    _plan = ExecutionPlan{};
    _nextReg = 0;
    _hasHotAccess = false;
    _hasColdAccess = false;

    // Check if the expression is a constant "true"
    if (auto const* constant = std::get_if<ConstantExpression>(&expr))
    {
      if (bool const* val = std::get_if<bool>(constant))
      {
        if (*val) { _plan.matchesAll = true; }
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
