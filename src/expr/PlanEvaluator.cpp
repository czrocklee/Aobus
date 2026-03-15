/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <cstring>
#include <rs/expr/PlanEvaluator.h>

namespace rs::expr
{

  // Number of reserved registers for intermediate values
  constexpr std::size_t kReservedRegisters = 16;

  namespace
  {
    // Helper to check if track has a tag ID
    [[maybe_unused]] bool trackHasTagId(core::TrackView const& track, core::DictionaryId tagId)
    {
      return track.tags().has(tagId);
    }

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
  }

  std::int64_t PlanEvaluator::loadField(core::TrackView const& track, Field field) const
  {
    switch (field)
    {
      case Field::TagBloom:
        return static_cast<std::int64_t>(track.tags().bloom());
      case Field::DurationMs:
        return static_cast<std::int64_t>(track.property().durationMs());
      case Field::Bitrate:
        return static_cast<std::int64_t>(track.property().bitrate());
      case Field::SampleRate:
        return static_cast<std::int64_t>(track.property().sampleRate());
      case Field::ArtistId:
        return static_cast<std::int64_t>(track.metadata().artistId().value());
      case Field::AlbumId:
        return static_cast<std::int64_t>(track.metadata().albumId().value());
      case Field::GenreId:
        return static_cast<std::int64_t>(track.metadata().genreId().value());
      case Field::Year:
        return static_cast<std::int64_t>(track.metadata().year());
      case Field::TrackNumber:
        return static_cast<std::int64_t>(track.metadata().trackNumber());
      case Field::CodecId:
        return static_cast<std::int64_t>(track.property().codecId());
      case Field::Channels:
        return static_cast<std::int64_t>(track.property().channels());
      case Field::BitDepth:
        return static_cast<std::int64_t>(track.property().bitDepth());
      case Field::Rating:
        return static_cast<std::int64_t>(track.property().rating());
      case Field::TagCount:
        return static_cast<std::int64_t>(track.tags().count());
      default:
        // Field::Tag and other unsupported fields return 0
        return 0;
    }
  }

  std::string_view PlanEvaluator::loadStringField(core::TrackView const& track, Field field) const
  {
    switch (field)
    {
      case Field::Title:
        return track.metadata().title();
      case Field::Uri:
        return track.property().uri();
      default:
        // Field::Tag and other unsupported fields return empty
        return {};
    }
  }

  std::int64_t PlanEvaluator::loadConstant(Instruction const& instr) const
  {
    // For string constants, constValue contains the string index
    // For numeric constants, constValue contains the value itself
    return instr.constValue;
  }

  bool PlanEvaluator::matches(ExecutionPlan const& plan, core::TrackView const& track) const
  {
    // Fast path: empty query matches all
    if (plan.matchesAll)
    {
      return true;
    }

    // Bloom filter fast-path rejection for tag queries
    if (plan.tagBloomMask != 0)
    {
      auto trackBloom = track.tags().bloom();
      if ((trackBloom & plan.tagBloomMask) != plan.tagBloomMask)
      {
        return false;
      }
    }

    // Full evaluation
    return evaluateFull(plan, track);
  }

  // NOLINTNEXTLINE(readability-function-size) - Complex eval function, splitting would reduce readability
  bool PlanEvaluator::evaluateFull(ExecutionPlan const& plan, core::TrackView const& track) const
  {
    if (plan.matchesAll)
    {
      return true;
    }

    // Return false for invalid/empty track views
    if (!track.isValid())
    {
      return false;
    }

    // Set current plan for string constant access
    _currentPlan = &plan;

    _registers.clear();
    _registers.resize(plan.instructions.size() + kReservedRegisters, 0);

    for (auto const& instr : plan.instructions)
    {
      switch (instr.op)
      {
        case OpCode::LoadField:
          _registers[instr.operand] = loadField(track, static_cast<Field>(instr.field));
          break;

        case OpCode::LoadConstant:
          _registers[instr.operand] = loadConstant(instr);
          // For bool/string types that need special handling, the value is already in registers
          break;

        case OpCode::Eq:
        {
          // Check if this is a tag field comparison
          Instruction const* prevLoadField = findPrevLoadField(plan.instructions, &instr);
          bool isTagComparison = prevLoadField && static_cast<Field>(prevLoadField->field) == Field::Tag;

          if (isTagComparison)
          {
            // For tag comparison, check if any track tag matches the constant
            auto tagIdToMatch = core::DictionaryId{static_cast<std::uint32_t>(_registers[instr.operand])};
            auto matches = track.tags().has(tagIdToMatch);
            _registers[instr.operand - 1] = matches ? 1 : 0;
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
          auto rhs = _registers[instr.operand];
          auto lhs = _registers[instr.operand - 1];
          _registers[instr.operand - 1] = (lhs != rhs) ? 1 : 0;
          break;
        }

        case OpCode::Lt:
        {
          auto rhs = _registers[instr.operand];
          auto lhs = _registers[instr.operand - 1];
          _registers[instr.operand - 1] = (lhs < rhs) ? 1 : 0;
          break;
        }

        case OpCode::Le:
        {
          auto rhs = _registers[instr.operand];
          auto lhs = _registers[instr.operand - 1];
          _registers[instr.operand - 1] = (lhs <= rhs) ? 1 : 0;
          break;
        }

        case OpCode::Gt:
        {
          auto rhs = _registers[instr.operand];
          auto lhs = _registers[instr.operand - 1];
          _registers[instr.operand - 1] = (lhs > rhs) ? 1 : 0;
          break;
        }

        case OpCode::Ge:
        {
          auto rhs = _registers[instr.operand];
          auto lhs = _registers[instr.operand - 1];
          _registers[instr.operand - 1] = (lhs >= rhs) ? 1 : 0;
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
          // For string comparison:
          // - The register[instr.operand - 1] should contain the result of the previous LoadField
          // - Actually for Like, we need to look at the previous instruction to get the field
          // - The register[instr.operand] contains the string constant index
          auto stringIdx = _registers[instr.operand];

          // Find the LoadField instruction that loads the string field
          // Search backwards from the current Like instruction
          Instruction const* prevLoadField = nullptr;
          for (size_t i = 0; i < plan.instructions.size(); ++i)
          {
            if (&plan.instructions[i] == &instr)
            {
              // Search backwards for a LoadField
              for (size_t j = i; j > 0; --j)
              {
                if (plan.instructions[j - 1].op == OpCode::LoadField)
                {
                  prevLoadField = &plan.instructions[j - 1];
                  break;
                }
              }
              break;
            }
          }

          // Load the string from the track if we found the field
          std::string_view fieldStr;
          if (prevLoadField)
          {
            auto field = static_cast<Field>(prevLoadField->field);
            fieldStr = loadStringField(track, field);
          }

          // Load the string constant from the plan
          std::string_view constantStr;
          if (_currentPlan && stringIdx >= 0 && static_cast<size_t>(stringIdx) < _currentPlan->stringConstants.size())
          {
            constantStr = _currentPlan->stringConstants[stringIdx];
          }

          // Check if field contains the constant (case-sensitive substring search)
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

} // namespace rs::expr
