// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Type.h>
#include <ao/library/AudioCodec.h>
#include <ao/library/TrackView.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/PlanEvaluator.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::query
{
  // Number of reserved registers for intermediate values
  constexpr std::size_t kReservedRegisters = 16;

  namespace
  {
    bool requiresHotData(ExecutionPlan const& plan)
    {
      return plan.accessProfile != AccessProfile::ColdOnly;
    }

    bool requiresColdData(ExecutionPlan const& plan)
    {
      return plan.accessProfile != AccessProfile::HotOnly;
    }

    bool hasRequiredTrackData(ExecutionPlan const& plan, library::TrackView const& track)
    {
      if (requiresHotData(plan) && !track.isHotValid())
      {
        return false;
      }

      if (requiresColdData(plan) && !track.isColdValid())
      {
        return false;
      }

      return true;
    }

    // Helper to find the previous LoadField instruction
    Instruction const* findPrevLoadField(std::vector<Instruction> const& instructions, Instruction const* current)
    {
      auto const it = std::ranges::find(instructions, current, [](auto const& instr) { return &instr; });

      if (it == instructions.end())
      {
        return nullptr;
      }

      auto const found =
        std::ranges::find_last(std::ranges::subrange{instructions.begin(), it}, OpCode::LoadField, &Instruction::op);

      if (found.empty())
      {
        return nullptr;
      }

      return &found.front();
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

    // Get string constant from plan's string constants table
    std::string_view getStringConstant(ExecutionPlan const* plan, std::int64_t stringIdx)
    {
      if (plan == nullptr || stringIdx < 0)
      {
        return {};
      }

      auto const idx = static_cast<size_t>(stringIdx);

      if (idx >= plan->stringConstants.size())
      {
        return {};
      }

      return plan->stringConstants[idx];
    }

    std::string_view loadDictionaryFieldValue(library::TrackView const& track, Field field, ExecutionPlan const* plan)
    {
      gsl_Expects(plan != nullptr);
      gsl_Expects(plan->dictionary != nullptr);

      if (plan == nullptr || plan->dictionary == nullptr)
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

      return plan->dictionary->get(dictionaryId);
    }

    std::string_view loadStringFieldValue(library::TrackView const& track, Field field, Instruction const* instr)
    {
      switch (field)
      {
        case Field::Title: return track.metadata().title();
        case Field::Uri: return track.property().uri();
        case Field::Custom:

          if (instr != nullptr && instr->constValue > 0)
          {
            auto dictId = DictionaryId{static_cast<std::uint32_t>(instr->constValue)};
            return track.custom().get(dictId).value_or("");
          }

          return {};

        default: return {};
      }
    }

    // Free function to load field value
    std::int64_t loadFieldValue(library::TrackView const& track, Field field)
    {
      switch (field)
      {
        // Property fields (@ prefix)
        case Field::Duration: return static_cast<std::int64_t>(track.property().duration().count());
        case Field::Bitrate: return static_cast<std::int64_t>(track.property().bitrate());
        case Field::SampleRate: return static_cast<std::int64_t>(track.property().sampleRate());
        case Field::Channels: return static_cast<std::int64_t>(track.property().channels());
        case Field::BitDepth: return static_cast<std::int64_t>(track.property().bitDepth());
        case Field::Codec: return static_cast<std::int64_t>(library::audioCodecStorageValue(track.property().codec()));
        case Field::Rating: return static_cast<std::int64_t>(track.metadata().rating());

        // Metadata ID fields
        case Field::ArtistId: return static_cast<std::int64_t>(track.metadata().artistId().raw());
        case Field::AlbumId: return static_cast<std::int64_t>(track.metadata().albumId().raw());
        case Field::GenreId: return static_cast<std::int64_t>(track.metadata().genreId().raw());
        case Field::AlbumArtistId: return static_cast<std::int64_t>(track.metadata().albumArtistId().raw());
        case Field::ComposerId: return static_cast<std::int64_t>(track.metadata().composerId().raw());
        case Field::WorkId: return static_cast<std::int64_t>(track.metadata().workId().raw());
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

    // Helper for safe register access to avoid sign-conversion warnings
    inline std::int64_t& reg(std::vector<std::int64_t>& registers, std::int32_t index)
    {
      gsl_Expects(index >= 0);
      return registers[static_cast<std::size_t>(index)];
    }

    // Execute comparison operation (helper for Ne, Lt, Le, Gt, Ge)
    template<typename Op>
    void executeComparison(std::vector<std::int64_t>& registers,
                           library::TrackView const& track,
                           ExecutionPlan const* plan,
                           Instruction const& instr,
                           Op&& op)
    {
      auto const* prevLoadField = findPrevLoadField(plan->instructions, &instr);

      if (auto const stringIdx = reg(registers, instr.operand);
          prevLoadField != nullptr && isStringField(static_cast<Field>(prevLoadField->field)))
      {
        auto field = static_cast<Field>(prevLoadField->field);
        auto const fieldStr = loadStringFieldValue(track, field, prevLoadField);
        auto const constantStr = getStringConstant(plan, stringIdx);
        reg(registers, instr.operand - 1) = std::invoke(std::forward<Op>(op), fieldStr, constantStr) ? 1 : 0;
      }
      else
      {
        // Numeric comparison
        auto rhs = reg(registers, instr.operand);
        auto lhs = reg(registers, instr.operand - 1);
        reg(registers, instr.operand - 1) = std::invoke(std::forward<Op>(op), lhs, rhs) ? 1 : 0;
      }
    }

    // Execute equality comparison (special case for tag lookups)
    void executeEq(std::vector<std::int64_t>& registers,
                   library::TrackView const& track,
                   ExecutionPlan const* plan,
                   Instruction const& instr,
                   std::vector<Instruction> const& instructions)
    {
      auto const* prevLoadField = findPrevLoadField(instructions, &instr);

      if (prevLoadField != nullptr && static_cast<Field>(prevLoadField->field) == Field::Tag)
      {
        auto tagIdToMatch = DictionaryId{static_cast<std::uint32_t>(reg(registers, instr.operand))};
        auto matches = track.tags().has(tagIdToMatch);
        reg(registers, instr.operand - 1) = matches ? 1 : 0;
      }
      else
      {
        if (auto stringIdx = reg(registers, instr.operand);
            prevLoadField != nullptr && isStringField(static_cast<Field>(prevLoadField->field)))
        {
          auto field = static_cast<Field>(prevLoadField->field);
          auto const fieldStr = loadStringFieldValue(track, field, prevLoadField);
          auto const constantStr = getStringConstant(plan, stringIdx);
          reg(registers, instr.operand - 1) = (fieldStr == constantStr) ? 1 : 0;
        }
        else
        {
          auto rhs = reg(registers, instr.operand);
          auto lhs = reg(registers, instr.operand - 1);
          reg(registers, instr.operand - 1) = (lhs == rhs) ? 1 : 0;
        }
      }
    }

    // Execute LIKE comparison (substring matching for string fields)
    void executeLike(std::vector<std::int64_t>& registers,
                     library::TrackView const& track,
                     ExecutionPlan const* plan,
                     Instruction const& instr,
                     std::vector<Instruction> const& instructions)
    {
      // instr.field contains the left field from the LoadField instruction
      auto field = static_cast<Field>(instr.field);
      auto rhs = reg(registers, instr.operand);
      auto const* prevLoadField = findPrevLoadField(instructions, &instr);

      auto fieldStr = std::string_view{};

      if (isStringField(field))
      {
        fieldStr = loadStringFieldValue(track, field, prevLoadField);
      }
      else if (isDictionaryField(field))
      {
        fieldStr = loadDictionaryFieldValue(track, field, plan);
      }

      auto const constantStr = getStringConstant(plan, rhs);
      auto found = fieldStr.contains(constantStr);
      reg(registers, instr.operand - 1) = found ? 1 : 0;
    }
  }

  bool PlanEvaluator::matches(ExecutionPlan const& plan, library::TrackView const& track) const
  {
    // Fast path: empty query matches all
    if (plan.matchesAll)
    {
      return true;
    }

    if (!hasRequiredTrackData(plan, track))
    {
      return false;
    }

    // Bloom filter fast-path rejection for tag queries
    if (plan.tagBloomMask != 0)
    {
      if (auto trackBloom = track.tags().bloom(); (trackBloom & plan.tagBloomMask) != plan.tagBloomMask)
      {
        return false;
      }
    }

    // Full evaluation
    return evaluateFull(plan, track);
  }

  bool PlanEvaluator::evaluateFull(ExecutionPlan const& plan, library::TrackView const& track) const
  {
    if (plan.matchesAll)
    {
      return true;
    }

    if (!hasRequiredTrackData(plan, track))
    {
      return false;
    }

    _registers.clear();
    _registers.resize(plan.instructions.size() + kReservedRegisters, 0);

    for (auto const& instr : plan.instructions)
    {
      switch (instr.op)
      {
        case OpCode::LoadField:
          reg(_registers, instr.operand) = loadFieldValue(track, static_cast<Field>(instr.field));
          break;

        case OpCode::LoadConstant: reg(_registers, instr.operand) = instr.constValue; break;

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
          auto rhs = reg(_registers, instr.operand);
          auto lhs = reg(_registers, instr.operand - 1);
          reg(_registers, instr.operand - 1) = (lhs != 0 && rhs != 0) ? 1 : 0;
          break;
        }

        case OpCode::Or:
        {
          auto rhs = reg(_registers, instr.operand);
          auto lhs = reg(_registers, instr.operand - 1);
          reg(_registers, instr.operand - 1) = (lhs != 0 || rhs != 0) ? 1 : 0;
          break;
        }

        case OpCode::Not:
        {
          auto val = reg(_registers, instr.operand);
          reg(_registers, instr.operand) = (val == 0) ? 1 : 0;
          break;
        }

        case OpCode::Like: executeLike(_registers, track, &plan, instr, plan.instructions); break;

        case OpCode::Nop:
        default: break;
      }
    }

    return _registers.empty() ? true : (_registers[0] != 0);
  }
} // namespace ao::query
