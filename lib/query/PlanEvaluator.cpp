// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/Type.h>
#include <ao/library/TrackView.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Field.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/detail/Bytecode.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::query
{
  // Number of reserved registers for intermediate values
  constexpr std::size_t kReservedRegisters = 16;

  namespace
  {
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

      return dictionaryFieldValue(track, field, *plan->dictionary);
    }

    std::string_view loadStringFieldValue(library::TrackView const& track, Field field, std::int64_t customDictId)
    {
      switch (field)
      {
        case Field::Title: return track.metadata().title();
        case Field::Uri: return track.property().uri();
        case Field::Custom:

          if (customDictId > 0)
          {
            auto dictId = DictionaryId{static_cast<std::uint32_t>(customDictId)};
            return track.customMetadata().get(dictId).value_or("");
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
        case Field::Bitrate: return static_cast<std::int64_t>(track.property().bitrate().raw());
        case Field::SampleRate: return static_cast<std::int64_t>(track.property().sampleRate().raw());
        case Field::Channels: return static_cast<std::int64_t>(track.property().channels().raw());
        case Field::BitDepth: return static_cast<std::int64_t>(track.property().bitDepth().raw());
        case Field::Codec: return static_cast<std::int64_t>(audioCodecStorageValue(track.property().codec()));

        // Metadata ID fields
        case Field::ArtistId: return static_cast<std::int64_t>(track.metadata().artistId().raw());
        case Field::AlbumId: return static_cast<std::int64_t>(track.metadata().albumId().raw());
        case Field::GenreId: return static_cast<std::int64_t>(track.metadata().genreId().raw());
        case Field::AlbumArtistId: return static_cast<std::int64_t>(track.metadata().albumArtistId().raw());
        case Field::ComposerId: return static_cast<std::int64_t>(track.metadata().composerId().raw());
        case Field::WorkId: return static_cast<std::int64_t>(track.metadata().workId().raw());
        case Field::CoverArtId:
          return static_cast<std::int64_t>(track.coverArt().primary().value_or(library::CoverArt{}).resourceId.raw());

        // Metadata numeric fields
        case Field::Year: return static_cast<std::int64_t>(track.metadata().year());
        case Field::TrackNumber: return static_cast<std::int64_t>(track.metadata().trackNumber());
        case Field::TrackTotal: return static_cast<std::int64_t>(track.metadata().trackTotal());
        case Field::DiscNumber: return static_cast<std::int64_t>(track.metadata().discNumber());
        case Field::DiscTotal: return static_cast<std::int64_t>(track.metadata().discTotal());

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
      auto const field = static_cast<Field>(instr.field);

      if (auto const stringIdx = reg(registers, instr.operand); isStringField(field))
      {
        auto const fieldStr = loadStringFieldValue(track, field, instr.constValue);
        auto const constantStr = getStringConstant(plan, stringIdx);
        reg(registers, instr.operand - 1) = std::invoke(std::forward<Op>(op), fieldStr, constantStr) ? 1 : 0;
      }
      else if (isDictionaryField(field) && isOrderedComparison(instr.op))
      {
        // Dictionary fields hold interned IDs whose order is arbitrary, so an
        // ordered comparison resolves the ID back to text and compares that.
        auto const fieldStr = loadDictionaryFieldValue(track, field, plan);
        auto const constantStr = getStringConstant(plan, stringIdx);
        reg(registers, instr.operand - 1) = std::invoke(std::forward<Op>(op), fieldStr, constantStr) ? 1 : 0;
      }
      else
      {
        // Numeric comparison
        auto rhs = stringIdx;
        auto lhs = reg(registers, instr.operand - 1);
        reg(registers, instr.operand - 1) = std::invoke(std::forward<Op>(op), lhs, rhs) ? 1 : 0;
      }
    }

    // Execute equality comparison (special case for tag lookups)
    void executeEq(std::vector<std::int64_t>& registers,
                   library::TrackView const& track,
                   ExecutionPlan const* plan,
                   Instruction const& instr)
    {
      if (auto const field = static_cast<Field>(instr.field); field == Field::Tag)
      {
        auto tagIdToMatch = DictionaryId{static_cast<std::uint32_t>(reg(registers, instr.operand))};
        auto matches = track.tags().has(tagIdToMatch);
        reg(registers, instr.operand - 1) = matches ? 1 : 0;
      }
      else
      {
        if (auto stringIdx = reg(registers, instr.operand); isStringField(field))
        {
          auto const fieldStr = loadStringFieldValue(track, field, instr.constValue);
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
                     Instruction const& instr)
    {
      // instr.field carries the left field; instr.constValue its Custom dictId (if any).
      auto field = static_cast<Field>(instr.field);
      auto rhs = reg(registers, instr.operand);

      auto fieldStr = std::string_view{};

      if (isStringField(field))
      {
        fieldStr = loadStringFieldValue(track, field, instr.constValue);
      }
      else if (isDictionaryField(field))
      {
        fieldStr = loadDictionaryFieldValue(track, field, plan);
      }

      auto const constantStr = getStringConstant(plan, rhs);
      auto found = fieldStr.contains(constantStr);
      reg(registers, instr.operand - 1) = found ? 1 : 0;
    }

    bool executeExists(library::TrackView const& track, Instruction const& instr)
    {
      switch (auto const field = static_cast<Field>(instr.field); field)
      {
        case Field::Title: return !track.metadata().title().empty();
        case Field::Uri: return !track.property().uri().empty();

        case Field::ArtistId:
        case Field::AlbumId:
        case Field::GenreId:
        case Field::AlbumArtistId:
        case Field::ComposerId:
        case Field::WorkId:
          return loadFieldValue(track, field) != static_cast<std::int64_t>(kInvalidDictionaryId.raw());

        case Field::CoverArtId:
          return track.coverArt().primary().value_or(library::CoverArt{}).resourceId != kInvalidResourceId;

        case Field::Year:
        case Field::TrackNumber:
        case Field::TrackTotal:
        case Field::DiscNumber:
        case Field::DiscTotal:
        case Field::Duration:
        case Field::Bitrate:
        case Field::SampleRate:
        case Field::Channels:
        case Field::BitDepth: return loadFieldValue(track, field) > 0;

        case Field::Codec: return track.property().codec() != AudioCodec::Unknown;

        case Field::Tag:
          return instr.constValue > 0 && track.tags().has(DictionaryId{static_cast<std::uint32_t>(instr.constValue)});

        case Field::Custom:
          return instr.constValue > 0 &&
                 track.customMetadata().contains(DictionaryId{static_cast<std::uint32_t>(instr.constValue)});

        case Field::TagBloom:
        case Field::TagCount: return loadFieldValue(track, field) > 0;

        default: return false;
      }
    }

    bool containsString(InSet const& set, std::string_view value)
    {
      auto const less = [](std::string_view lhs, std::string_view rhs) { return lhs < rhs; };
      return std::ranges::binary_search(
        set.strings, value, less, [](std::string const& item) { return std::string_view{item}; });
    }

    void executeInSet(std::vector<std::int64_t>& registers,
                      library::TrackView const& track,
                      ExecutionPlan const* plan,
                      Instruction const& instr)
    {
      if (plan == nullptr || instr.constValue < 0)
      {
        reg(registers, instr.operand) = 0;
        return;
      }

      auto const setIdx = static_cast<std::size_t>(instr.constValue);

      if (setIdx >= plan->inSets.size())
      {
        reg(registers, instr.operand) = 0;
        return;
      }

      auto const& set = plan->inSets[setIdx];

      if (set.stringValues)
      {
        // instr.field is the left field; instr.size carries its Custom dictId (if any).
        auto const field = static_cast<Field>(instr.field);
        auto fieldValue = std::string_view{};

        if (isStringField(field) || field == Field::Custom)
        {
          fieldValue = loadStringFieldValue(track, field, instr.size);
        }
        else if (isDictionaryField(field) && plan->dictionary != nullptr)
        {
          fieldValue = loadDictionaryFieldValue(track, field, plan);
        }

        reg(registers, instr.operand) = containsString(set, fieldValue) ? 1 : 0;
        return;
      }

      reg(registers, instr.operand) = set.numericValues.contains(reg(registers, instr.operand)) ? 1 : 0;
    }
  }

  bool PlanEvaluator::matches(ExecutionPlan const& plan, library::TrackView const& track) const
  {
    // Fast path: empty query matches all
    if (plan.matchesAll)
    {
      return true;
    }

    if (!hasRequiredTrackData(plan.accessProfile, track))
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

    if (!hasRequiredTrackData(plan.accessProfile, track))
    {
      return false;
    }

    _registers.assign(plan.instructions.size() + kReservedRegisters, 0);

    for (auto const& instr : plan.instructions)
    {
      switch (instr.op)
      {
        case OpCode::LoadField:
          reg(_registers, instr.operand) = loadFieldValue(track, static_cast<Field>(instr.field));
          break;

        case OpCode::LoadConstant: reg(_registers, instr.operand) = instr.constValue; break;

        case OpCode::Eq: executeEq(_registers, track, &plan, instr); break;

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

        case OpCode::Like: executeLike(_registers, track, &plan, instr); break;

        case OpCode::Exists: reg(_registers, instr.operand) = executeExists(track, instr) ? 1 : 0; break;

        case OpCode::InSet: executeInSet(_registers, track, &plan, instr); break;

        case OpCode::Nop:
        default: break;
      }
    }

    return _registers.empty() ? true : (_registers[0] != 0);
  }
} // namespace ao::query
