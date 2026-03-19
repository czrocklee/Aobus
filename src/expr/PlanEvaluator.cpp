// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/expr/PlanEvaluator.h>

#include <algorithm>
#include <cstring>

namespace rs::expr
{

  // Number of reserved registers for intermediate values
  constexpr std::size_t kReservedRegisters = 16;

  namespace
  {
    // Helper to find the previous LoadField instruction
    Instruction const* findPrevLoadField(std::vector<Instruction> const& instructions, Instruction const* current)
    {
      for (size_t i = 0; i < instructions.size(); ++i)
      {
        if (&instructions[i] == current)
        {
          // Search backwards for a LoadField
          for (size_t j = i; j > 0; --j)
          {
            if (instructions[j - 1].op == OpCode::LoadField)
            {
              return &instructions[j - 1];
            }
          }
          break;
        }
      }
      return nullptr;
    }

    // Check if a field requires string comparison (not numeric)
    bool isStringField(Field field)
    {
      switch (field)
      {
        case Field::Title:
        case Field::Uri:
        case Field::Custom:
          return true;
        default:
          return false;
      }
    }

    // Check if a field is stored in cold storage
    bool isColdField(Field field)
    {
      switch (field)
      {
        case Field::Uri:
        case Field::CoverArtId:
        case Field::TrackNumber:
        case Field::TotalTracks:
        case Field::DiscNumber:
        case Field::TotalDiscs:
        case Field::Custom:
          return true;
        default:
          return false;
      }
    }

    // Compare two strings lexicographically, return -1, 0, or 1
    int compareStrings(std::string_view lhs, std::string_view rhs)
    {
      if (lhs < rhs) return -1;
      if (lhs > rhs) return 1;
      return 0;
    }
  }

  std::int64_t PlanEvaluator::loadField(core::TrackHotView const& hotView, Field field) const
  {
    switch (field)
    {
      // Property fields (@ prefix)
      case Field::BitDepth:
        return static_cast<std::int64_t>(hotView.property().bitDepth());
      case Field::CodecId:
        return static_cast<std::int64_t>(hotView.property().codecId());
      case Field::Rating:
        return static_cast<std::int64_t>(hotView.property().rating());

      // Metadata ID fields
      case Field::ArtistId:
        return static_cast<std::int64_t>(hotView.metadata().artistId().value());
      case Field::AlbumId:
        return static_cast<std::int64_t>(hotView.metadata().albumId().value());
      case Field::GenreId:
        return static_cast<std::int64_t>(hotView.metadata().genreId().value());
      case Field::AlbumArtistId:
        return static_cast<std::int64_t>(hotView.metadata().albumArtistId().value());

      // Metadata numeric fields
      case Field::Year:
        return static_cast<std::int64_t>(hotView.metadata().year());

      // Tag fields
      case Field::TagBloom:
        return static_cast<std::int64_t>(hotView.tags().bloom());
      case Field::TagCount:
        return static_cast<std::int64_t>(hotView.tags().count());

      default:
        return 0;
    }
  }

  std::string_view PlanEvaluator::loadStringField(core::TrackHotView const& hotView, Field field) const
  {
    switch (field)
    {
      case Field::Title:
        return hotView.metadata().title();
      default:
        return {};
    }
  }

  std::int64_t PlanEvaluator::loadField(core::TrackColdView const& coldView, Field field) const
  {
    switch (field)
    {
      case Field::DurationMs:
        return static_cast<std::int64_t>(coldView.property().durationMs());
      case Field::Bitrate:
        return static_cast<std::int64_t>(coldView.property().bitrate());
      case Field::SampleRate:
        return static_cast<std::int64_t>(coldView.property().sampleRate());
      case Field::Channels:
        return static_cast<std::int64_t>(coldView.property().channels());
      case Field::CoverArtId:
        return static_cast<std::int64_t>(coldView.property().coverArtId());
      case Field::TrackNumber:
        return static_cast<std::int64_t>(coldView.property().trackNumber());
      case Field::TotalTracks:
        return static_cast<std::int64_t>(coldView.property().totalTracks());
      case Field::DiscNumber:
        return static_cast<std::int64_t>(coldView.property().discNumber());
      case Field::TotalDiscs:
        return static_cast<std::int64_t>(coldView.property().totalDiscs());
      default:
        return 0;
    }
  }

  std::string_view PlanEvaluator::loadStringField(core::TrackColdView const& coldView, Field field) const
  {
    switch (field)
    {
      case Field::Uri:
        return coldView.property().uri();
      default:
        return {};
    }
  }

  std::int64_t PlanEvaluator::loadConstant(Instruction const& instr) const
  {
    return instr.constValue;
  }

  bool PlanEvaluator::matches(ExecutionPlan const& plan, core::TrackHotView const& hotView) const
  {
    if (plan.matchesAll)
    {
      return true;
    }

    if (plan.tagBloomMask != 0)
    {
      auto trackBloom = hotView.tags().bloom();
      if ((trackBloom & plan.tagBloomMask) != plan.tagBloomMask)
      {
        return false;
      }
    }

    return evaluateFull(plan, hotView);
  }

  bool PlanEvaluator::matches(ExecutionPlan const& plan, core::TrackColdView const& coldView) const
  {
    if (plan.matchesAll)
    {
      return true;
    }

    // Cold-only queries don't have bloom filter optimization
    return evaluateFull(plan, coldView);
  }

  bool PlanEvaluator::matches(ExecutionPlan const& plan, core::TrackHotView const& hotView, core::TrackColdView const& coldView) const
  {
    if (plan.matchesAll)
    {
      return true;
    }

    if (plan.tagBloomMask != 0)
    {
      auto trackBloom = hotView.tags().bloom();
      if ((trackBloom & plan.tagBloomMask) != plan.tagBloomMask)
      {
        return false;
      }
    }

    return evaluateFull(plan, hotView, coldView);
  }

  bool PlanEvaluator::matches(ExecutionPlan const& plan, core::TrackView const& view) const
  {
    return matches(plan, view.hot(), view.cold());
  }

  // NOLINTNEXTLINE(readability-function-size)
  bool PlanEvaluator::evaluateFull(ExecutionPlan const& plan, core::TrackHotView const& hotView) const
  {
    if (plan.matchesAll)
    {
      return true;
    }

    if (!hotView.isValid())
    {
      return false;
    }

    _currentPlan = &plan;
    _registers.clear();
    _registers.resize(plan.instructions.size() + kReservedRegisters, 0);

    for (auto const& instr : plan.instructions)
    {
      switch (instr.op)
      {
        case OpCode::LoadField:
        {
          auto field = static_cast<Field>(instr.field);
          if (isColdField(field))
          {
            // Cold field not available in hot-only path
            _registers[instr.operand] = 0;
          }
          else
          {
            _registers[instr.operand] = loadField(hotView, field);
          }
          break;
        }

        case OpCode::LoadConstant:
          _registers[instr.operand] = loadConstant(instr);
          break;

        case OpCode::Eq:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          bool isTagComparison = prevLoadField && static_cast<Field>(prevLoadField->field) == Field::Tag;

          if (isTagComparison)
          {
            auto tagIdToMatch = core::DictionaryId{static_cast<std::uint32_t>(_registers[instr.operand])};
            auto matches = hotView.tags().has(tagIdToMatch);
            _registers[instr.operand - 1] = matches ? 1 : 0;
          }
          else if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = loadStringField(hotView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr == constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs == rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Ne:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = loadStringField(hotView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr != constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs != rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Lt:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = loadStringField(hotView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr < constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs < rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Le:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = loadStringField(hotView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr <= constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs <= rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Gt:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = loadStringField(hotView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr > constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs > rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Ge:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = loadStringField(hotView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr >= constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs >= rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::And:
        {
          auto rhs = _registers[instr.operand];
          auto lhs = _registers[instr.operand - 1];
          _registers[instr.operand - 1] = (lhs && rhs) ? 1 : 0;
          break;
        }

        case OpCode::Or:
        {
          auto rhs = _registers[instr.operand];
          auto lhs = _registers[instr.operand - 1];
          _registers[instr.operand - 1] = (lhs || rhs) ? 1 : 0;
          break;
        }

        case OpCode::Not:
        {
          auto val = _registers[instr.operand];
          _registers[instr.operand] = !val ? 1 : 0;
          break;
        }

        case OpCode::Like:
        {
          auto stringIdx = _registers[instr.operand];
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);

          std::string_view fieldStr;
          if (prevLoadField)
          {
            auto field = static_cast<Field>(prevLoadField->field);
            fieldStr = loadStringField(hotView, field);
          }

          std::string_view constantStr;
          if (_currentPlan && stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
          {
            constantStr = _currentPlan->stringConstants[stringIdx];
          }

          auto found = fieldStr.find(constantStr) != std::string_view::npos;
          _registers[instr.operand - 1] = found ? 1 : 0;
          break;
        }

        case OpCode::Nop:
        default:
          break;
      }
    }

    return _registers.empty() ? true : (_registers[0] != 0);
  }

  // NOLINTNEXTLINE(readability-function-size)
  bool PlanEvaluator::evaluateFull(ExecutionPlan const& plan, core::TrackColdView const& coldView) const
  {
    if (plan.matchesAll)
    {
      return true;
    }

    if (!coldView.isValid())
    {
      return false;
    }

    _currentPlan = &plan;
    _registers.clear();
    _registers.resize(plan.instructions.size() + kReservedRegisters, 0);

    for (auto const& instr : plan.instructions)
    {
      switch (instr.op)
      {
        case OpCode::LoadField:
        {
          auto field = static_cast<Field>(instr.field);
          if (!isColdField(field))
          {
            // Hot field not available in cold-only path
            _registers[instr.operand] = 0;
          }
          else if (field == Field::Custom)
          {
            // Custom field - use string index to look up key name, then get value from cold view
            auto stringIdx = _registers[instr.operand];
            std::string key;
            if (_currentPlan && stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
            {
              key = _currentPlan->stringConstants[stringIdx];
            }
            auto value = coldView.customValue(key);
            // Store 1 if custom value exists and matches (comparison happens in Eq)
            _registers[instr.operand] = value ? 1 : 0;
          }
          else
          {
            _registers[instr.operand] = loadField(coldView, field);
          }
          break;
        }

        case OpCode::LoadConstant:
          _registers[instr.operand] = loadConstant(instr);
          break;

        case OpCode::Eq:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = loadStringField(coldView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr == constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs == rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Ne:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = loadStringField(coldView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr != constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs != rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Lt:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = loadStringField(coldView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr < constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs < rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Le:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = loadStringField(coldView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr <= constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs <= rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Gt:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = loadStringField(coldView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr > constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs > rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Ge:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = loadStringField(coldView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr >= constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs >= rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::And:
        {
          auto rhs = _registers[instr.operand];
          auto lhs = _registers[instr.operand - 1];
          _registers[instr.operand - 1] = (lhs && rhs) ? 1 : 0;
          break;
        }

        case OpCode::Or:
        {
          auto rhs = _registers[instr.operand];
          auto lhs = _registers[instr.operand - 1];
          _registers[instr.operand - 1] = (lhs || rhs) ? 1 : 0;
          break;
        }

        case OpCode::Not:
        {
          auto val = _registers[instr.operand];
          _registers[instr.operand] = !val ? 1 : 0;
          break;
        }

        case OpCode::Like:
        {
          auto stringIdx = _registers[instr.operand];
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);

          std::string_view fieldStr;
          if (prevLoadField)
          {
            auto field = static_cast<Field>(prevLoadField->field);
            fieldStr = loadStringField(coldView, field);
          }

          std::string_view constantStr;
          if (_currentPlan && stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
          {
            constantStr = _currentPlan->stringConstants[stringIdx];
          }

          auto found = fieldStr.find(constantStr) != std::string_view::npos;
          _registers[instr.operand - 1] = found ? 1 : 0;
          break;
        }

        case OpCode::Nop:
        default:
          break;
      }
    }

    return _registers.empty() ? true : (_registers[0] != 0);
  }

  // NOLINTNEXTLINE(readability-function-size)
  bool PlanEvaluator::evaluateFull(ExecutionPlan const& plan, core::TrackHotView const& hotView, core::TrackColdView const& coldView) const
  {
    if (plan.matchesAll)
    {
      return true;
    }

    if (!hotView.isValid())
    {
      return false;
    }

    _currentPlan = &plan;
    _registers.clear();
    _registers.resize(plan.instructions.size() + kReservedRegisters, 0);

    for (auto const& instr : plan.instructions)
    {
      switch (instr.op)
      {
        case OpCode::LoadField:
        {
          auto field = static_cast<Field>(instr.field);
          if (isColdField(field))
          {
            _registers[instr.operand] = loadField(coldView, field);
          }
          else
          {
            _registers[instr.operand] = loadField(hotView, field);
          }
          break;
        }

        case OpCode::LoadConstant:
          _registers[instr.operand] = loadConstant(instr);
          break;

        case OpCode::Eq:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          bool isTagComparison = prevLoadField && static_cast<Field>(prevLoadField->field) == Field::Tag;

          if (isTagComparison)
          {
            auto tagIdToMatch = core::DictionaryId{static_cast<std::uint32_t>(_registers[instr.operand])};
            auto matches = hotView.tags().has(tagIdToMatch);
            _registers[instr.operand - 1] = matches ? 1 : 0;
          }
          else if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = isColdField(field) ? loadStringField(coldView, field) : loadStringField(hotView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr == constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs == rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Ne:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = isColdField(field) ? loadStringField(coldView, field) : loadStringField(hotView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr != constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs != rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Lt:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = isColdField(field) ? loadStringField(coldView, field) : loadStringField(hotView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr < constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs < rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Le:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = isColdField(field) ? loadStringField(coldView, field) : loadStringField(hotView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr <= constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs <= rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Gt:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = isColdField(field) ? loadStringField(coldView, field) : loadStringField(hotView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr > constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs > rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::Ge:
        {
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          if (prevLoadField && isStringField(static_cast<Field>(prevLoadField->field)))
          {
            auto field = static_cast<Field>(prevLoadField->field);
            std::string_view fieldStr = isColdField(field) ? loadStringField(coldView, field) : loadStringField(hotView, field);
            std::string_view constantStr;
            if (_currentPlan)
            {
              auto stringIdx = _registers[instr.operand];
              if (stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
              {
                constantStr = _currentPlan->stringConstants[stringIdx];
              }
            }
            _registers[instr.operand - 1] = (fieldStr >= constantStr) ? 1 : 0;
          }
          else
          {
            auto rhs = _registers[instr.operand];
            auto lhs = _registers[instr.operand - 1];
            _registers[instr.operand - 1] = (lhs >= rhs) ? 1 : 0;
          }
          break;
        }

        case OpCode::And:
        {
          auto rhs = _registers[instr.operand];
          auto lhs = _registers[instr.operand - 1];
          _registers[instr.operand - 1] = (lhs && rhs) ? 1 : 0;
          break;
        }

        case OpCode::Or:
        {
          auto rhs = _registers[instr.operand];
          auto lhs = _registers[instr.operand - 1];
          _registers[instr.operand - 1] = (lhs || rhs) ? 1 : 0;
          break;
        }

        case OpCode::Not:
        {
          auto val = _registers[instr.operand];
          _registers[instr.operand] = !val ? 1 : 0;
          break;
        }

        case OpCode::Like:
        {
          auto stringIdx = _registers[instr.operand];
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);

          std::string_view fieldStr;
          if (prevLoadField)
          {
            auto field = static_cast<Field>(prevLoadField->field);
            fieldStr = isColdField(field) ? loadStringField(coldView, field) : loadStringField(hotView, field);
          }

          std::string_view constantStr;
          if (_currentPlan && stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
          {
            constantStr = _currentPlan->stringConstants[stringIdx];
          }

          auto found = fieldStr.find(constantStr) != std::string_view::npos;
          _registers[instr.operand - 1] = found ? 1 : 0;
          break;
        }

        case OpCode::Nop:
        default:
          break;
      }
    }

    return _registers.empty() ? true : (_registers[0] != 0);
  }

  bool PlanEvaluator::evaluateFull(ExecutionPlan const& plan, core::TrackView const& view) const
  {
    return evaluateFull(plan, view.hot(), view.cold());
  }

} // namespace rs::expr