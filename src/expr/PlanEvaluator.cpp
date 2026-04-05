// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/expr/PlanEvaluator.h>

#include <algorithm>
#include <cstring>
#include <ranges>

namespace rs::expr
{

  // Number of reserved registers for intermediate values
  constexpr std::size_t kReservedRegisters = 16;

  namespace
  {
    // Helper to find the previous LoadField instruction
    Instruction const* findPrevLoadField(std::vector<Instruction> const& instructions, Instruction const* current)
    {
      auto const it = std::ranges::find_if(instructions, [current](auto const& instr) { return &instr == current; });
      if (it == instructions.end()) { return nullptr; }

      auto const revRange = std::ranges::subrange(instructions.begin(), it) | std::views::reverse;

      if (auto const found = std::ranges::find(revRange, OpCode::LoadField, &Instruction::op);
          found != revRange.end())
      {
        return &(*found);
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
        case Field::Custom: return true;
        default: return false;
      }
    }

    // Get string constant from plan's string constants table
    std::string_view getStringConstant(ExecutionPlan const* plan, std::int64_t stringIdx)
    {
      if (plan == nullptr || stringIdx < 0) { return {}; }
      auto idx = static_cast<size_t>(stringIdx);
      if (idx >= plan->stringConstants.size()) { return {}; }
      return plan->stringConstants[idx];
    }

    // Free function to load string field value (can be used by helpers)
    std::string_view loadStringFieldValue(core::TrackView const& track, Field field, Instruction const* instr)
    {
      switch (field)
      {
        case Field::Title: return track.metadata().title();
        case Field::Uri: return track.property().uri();
        case Field::Custom:
          if (instr != nullptr && instr->constValue > 0)
          {
            auto dictId = core::DictionaryId{static_cast<std::uint32_t>(instr->constValue)};
            return track.custom().get(dictId).value_or("");
          }
          return {};
        default: return {};
      }
    }

    // Free function to load field value
    std::int64_t loadFieldValue(core::TrackView const& track, Field field)
    {
      switch (field)
      {
        // Property fields (@ prefix)
        case Field::DurationMs: return static_cast<std::int64_t>(track.property().durationMs());
        case Field::Bitrate: return static_cast<std::int64_t>(track.property().bitrate());
        case Field::SampleRate: return static_cast<std::int64_t>(track.property().sampleRate());
        case Field::Channels: return static_cast<std::int64_t>(track.property().channels());
        case Field::BitDepth: return static_cast<std::int64_t>(track.property().bitDepth());
        case Field::CodecId: return static_cast<std::int64_t>(track.property().codecId());
        case Field::Rating: return static_cast<std::int64_t>(track.property().rating());

        // Metadata ID fields
        case Field::ArtistId: return static_cast<std::int64_t>(track.metadata().artistId().value());
        case Field::AlbumId: return static_cast<std::int64_t>(track.metadata().albumId().value());
        case Field::GenreId: return static_cast<std::int64_t>(track.metadata().genreId().value());
        case Field::AlbumArtistId: return static_cast<std::int64_t>(track.metadata().albumArtistId().value());
        case Field::CoverArtId: return static_cast<std::int64_t>(track.metadata().coverArtId());

        // Metadata numeric fields
        case Field::Year: return static_cast<std::int64_t>(track.metadata().year());
        case Field::TrackNumber: return static_cast<std::int64_t>(track.metadata().trackNumber());
        case Field::TotalTracks: return static_cast<std::int64_t>(track.metadata().totalTracks());
        case Field::DiscNumber: return static_cast<std::int64_t>(track.metadata().discNumber());
        case Field::TotalDiscs: return static_cast<std::int64_t>(track.metadata().totalDiscs());

        // Tag fields
        case Field::TagBloom: return static_cast<std::int64_t>(track.tags().bloom());
        case Field::TagCount: return static_cast<std::int64_t>(track.tags().count());

        // Custom field - placeholder return, actual lookup requires cold data access
        case Field::Custom:
        default: return 0;
      }
    }

    // Execute comparison operation (helper for Ne, Lt, Le, Gt, Ge)
    template<typename Op>
    void executeComparison(std::vector<std::int64_t>& registers,
                           core::TrackView const& track,
                           ExecutionPlan const* plan,
                           Instruction const& instr,
                           Op&& op)
    {
      auto const* prevLoadField = findPrevLoadField(plan->instructions, &instr);
      auto stringIdx = registers[instr.operand];

      // Check for string comparison first
      if (prevLoadField != nullptr && isStringField(static_cast<Field>(prevLoadField->field)))
      {
        auto field = static_cast<Field>(prevLoadField->field);
        std::string_view fieldStr = loadStringFieldValue(track, field, prevLoadField);
        std::string_view constantStr = getStringConstant(plan, stringIdx);
        registers[instr.operand - 1] = std::invoke(std::forward<Op>(op), fieldStr, constantStr) ? 1 : 0;
      }
      else
      {
        // Numeric comparison
        auto rhs = registers[instr.operand];
        auto lhs = registers[instr.operand - 1];
        registers[instr.operand - 1] = std::invoke(std::forward<Op>(op), lhs, rhs) ? 1 : 0;
      }
    }

    // Execute equality comparison (special case for tag lookups)
    void executeEq(std::vector<std::int64_t>& registers,
                   core::TrackView const& track,
                   ExecutionPlan const* plan,
                   Instruction const& instr,
                   std::vector<Instruction> const& instructions)
    {
      auto const* prevLoadField = findPrevLoadField(instructions, &instr);
      bool isTagComparison = prevLoadField != nullptr && static_cast<Field>(prevLoadField->field) == Field::Tag;

      if (isTagComparison)
      {
        auto tagIdToMatch = core::DictionaryId{static_cast<std::uint32_t>(registers[instr.operand])};
        auto matches = track.tags().has(tagIdToMatch);
        registers[instr.operand - 1] = matches ? 1 : 0;
      }
      else
      {
        auto stringIdx = registers[instr.operand];
        if (prevLoadField != nullptr && isStringField(static_cast<Field>(prevLoadField->field)))
        {
          auto field = static_cast<Field>(prevLoadField->field);
          std::string_view fieldStr = loadStringFieldValue(track, field, prevLoadField);
          std::string_view constantStr = getStringConstant(plan, stringIdx);
          registers[instr.operand - 1] = (fieldStr == constantStr) ? 1 : 0;
        }
        else
        {
          auto rhs = registers[instr.operand];
          auto lhs = registers[instr.operand - 1];
          registers[instr.operand - 1] = (lhs == rhs) ? 1 : 0;
        }
      }
    }

    // Execute LIKE comparison (substring matching for string fields)
    void executeLike(std::vector<std::int64_t>& registers,
                    core::TrackView const& track,
                    ExecutionPlan const* plan,
                    Instruction const& instr,
                    std::vector<Instruction> const& /*instructions*/)
    {
      // instr.field contains the left field from the LoadField instruction
      auto field = static_cast<Field>(instr.field);
      auto rhs = registers[instr.operand];

      std::string_view fieldStr = loadStringFieldValue(track, field, nullptr);
      std::string_view constantStr = getStringConstant(plan, rhs);
      auto found = fieldStr.find(constantStr) != std::string_view::npos;
      registers[instr.operand - 1] = found ? 1 : 0;
    }
  }

  bool PlanEvaluator::matches(ExecutionPlan const& plan, core::TrackView const& track) const
  {
    // Fast path: empty query matches all
    if (plan.matchesAll) { return true; }

    // Bloom filter fast-path rejection for tag queries
    if (plan.tagBloomMask != 0)
    {
      auto trackBloom = track.tags().bloom();
      if ((trackBloom & plan.tagBloomMask) != plan.tagBloomMask) { return false; }
    }

    // Full evaluation
    return evaluateFull(plan, track);
  }

  // NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity)
  bool PlanEvaluator::evaluateFull(ExecutionPlan const& plan, core::TrackView const& track) const
  {
    if (plan.matchesAll) { return true; }

    // Return false for invalid/empty track views
    if (!track.isHotValid()) { return false; }

    _registers.clear();
    _registers.resize(plan.instructions.size() + kReservedRegisters, 0);

    for (auto const& instr : plan.instructions)
    {
      switch (instr.op)
      {
        case OpCode::LoadField:
          _registers[instr.operand] = loadFieldValue(track, static_cast<Field>(instr.field));
          break;

        case OpCode::LoadConstant: _registers[instr.operand] = instr.constValue; break;

        case OpCode::Eq: executeEq(_registers, track, &plan, instr, plan.instructions); break;

        case OpCode::Ne:
          executeComparison(_registers, track, &plan, instr, [](auto lhs, auto rhs) { return lhs != rhs; });
          break;

        case OpCode::Lt:
          executeComparison(_registers, track, &plan, instr, [](auto lhs, auto rhs) { return lhs < rhs; });
          break;

        case OpCode::Le:
          executeComparison(_registers, track, &plan, instr, [](auto lhs, auto rhs) { return lhs <= rhs; });
          break;

        case OpCode::Gt:
          executeComparison(_registers, track, &plan, instr, [](auto lhs, auto rhs) { return lhs > rhs; });
          break;

        case OpCode::Ge:
          executeComparison(_registers, track, &plan, instr, [](auto lhs, auto rhs) { return lhs >= rhs; });
          break;

        case OpCode::And:
        {
          auto rhs = _registers[instr.operand];
          auto lhs = _registers[instr.operand - 1];
          _registers[instr.operand - 1] = (lhs != 0 && rhs != 0) ? 1 : 0;
          break;
        }

        case OpCode::Or:
        {
          auto rhs = _registers[instr.operand];
          auto lhs = _registers[instr.operand - 1];
          _registers[instr.operand - 1] = (lhs != 0 || rhs != 0) ? 1 : 0;
          break;
        }

        case OpCode::Not:
        {
          auto val = _registers[instr.operand];
          _registers[instr.operand] = (val == 0) ? 1 : 0;
          break;
        }

        case OpCode::Like:
          executeLike(_registers, track, &plan, instr, plan.instructions);
          break;

        case OpCode::Nop:
        default: break;
      }
    }

    return _registers.empty() ? true : (_registers[0] != 0);
  }

} // namespace rs::expr
