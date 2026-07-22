// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Field.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/detail/Bytecode.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::query
{
  namespace
  {
    constexpr std::size_t kReservedRegisters = 16;
    constexpr std::uint32_t kBloomBitMask = 31;

    DictionaryId boundDictionaryId(std::span<DictionaryId const> ids, std::uint32_t symbol)
    {
      if (symbol == kNoDictionarySymbol || symbol >= ids.size())
      {
        return kInvalidDictionaryId;
      }

      return ids[symbol];
    }

    std::string_view stringConstant(ExecutionPlan const& plan, std::int64_t stringIndex)
    {
      if (stringIndex < 0)
      {
        return {};
      }

      auto const index = static_cast<std::size_t>(stringIndex);
      return index < plan.stringConstants.size() ? std::string_view{plan.stringConstants[index]} : std::string_view{};
    }

    std::string_view readDictionaryFieldValue(library::TrackView const& track,
                                              Field field,
                                              library::DictionaryReadContext* dictionary)
    {
      if (dictionary == nullptr)
      {
        return {};
      }

      auto const dictionaryId = dictionaryFieldId(track, field);
      return dictionaryId == kInvalidDictionaryId ? std::string_view{} : dictionary->get(dictionaryId);
    }

    std::string_view readStringFieldValue(library::TrackView const& track, Field field, DictionaryId customKey)
    {
      switch (field)
      {
        case Field::Title: return track.metadata().title();
        case Field::Uri: return track.property().uri();
        case Field::Custom:
          return customKey == kInvalidDictionaryId ? std::string_view{}
                                                   : track.customMetadata().get(customKey).value_or("");
        default: return {};
      }
    }

    std::int64_t readFieldValue(library::TrackView const& track, Field field)
    {
      switch (field)
      {
        case Field::Duration: return static_cast<std::int64_t>(track.property().duration().count());
        case Field::Bitrate: return static_cast<std::int64_t>(track.property().bitrate().raw());
        case Field::SampleRate: return static_cast<std::int64_t>(track.property().sampleRate().raw());
        case Field::Channels: return static_cast<std::int64_t>(track.property().channels().raw());
        case Field::BitDepth: return static_cast<std::int64_t>(track.property().bitDepth().raw());
        case Field::Codec: return static_cast<std::int64_t>(audioCodecStorageValue(track.property().codec()));

        case Field::ArtistId: return static_cast<std::int64_t>(track.metadata().artistId().raw());
        case Field::AlbumId: return static_cast<std::int64_t>(track.metadata().albumId().raw());
        case Field::GenreId: return static_cast<std::int64_t>(track.metadata().genreId().raw());
        case Field::AlbumArtistId: return static_cast<std::int64_t>(track.metadata().albumArtistId().raw());
        case Field::ComposerId: return static_cast<std::int64_t>(track.metadata().composerId().raw());
        case Field::ConductorId: return static_cast<std::int64_t>(track.classical().conductorId().raw());
        case Field::EnsembleId: return static_cast<std::int64_t>(track.classical().ensembleId().raw());
        case Field::WorkId: return static_cast<std::int64_t>(track.classical().workId().raw());
        case Field::MovementId: return static_cast<std::int64_t>(track.classical().movementId().raw());
        case Field::SoloistId: return static_cast<std::int64_t>(track.classical().soloistId().raw());
        case Field::CoverArtId:
          return static_cast<std::int64_t>(track.coverArt().primary().value_or(library::CoverArt{}).resourceId.raw());

        case Field::Year: return static_cast<std::int64_t>(track.metadata().year());
        case Field::TrackNumber: return static_cast<std::int64_t>(track.metadata().trackNumber());
        case Field::TrackTotal: return static_cast<std::int64_t>(track.metadata().trackTotal());
        case Field::DiscNumber: return static_cast<std::int64_t>(track.metadata().discNumber());
        case Field::DiscTotal: return static_cast<std::int64_t>(track.metadata().discTotal());
        case Field::MovementNumber: return static_cast<std::int64_t>(track.classical().movementNumber());
        case Field::MovementTotal: return static_cast<std::int64_t>(track.classical().movementTotal());

        case Field::TagBloom: return static_cast<std::int64_t>(track.tags().bloom());
        case Field::TagCount: return static_cast<std::int64_t>(track.tags().count());

        case Field::Custom:
        default: return 0;
      }
    }

    std::int64_t& reg(std::vector<std::int64_t>& registers, std::int32_t index)
    {
      gsl_Expects(index >= 0);
      return registers[static_cast<std::size_t>(index)];
    }

    template<typename Op>
    void executeComparison(std::vector<std::int64_t>& registers,
                           library::TrackView const& track,
                           ExecutionPlan const& plan,
                           library::DictionaryReadContext* dictionary,
                           std::span<DictionaryId const> dictionaryIds,
                           Instruction const& instr,
                           bool unresolvedResult,
                           Op&& op)
    {
      auto const field = static_cast<Field>(instr.field);
      auto const rhs = reg(registers, instr.operand);

      if (isStringField(field))
      {
        auto const customKey = boundDictionaryId(dictionaryIds, instr.dictionarySymbol);

        if (field == Field::Custom && customKey == kInvalidDictionaryId)
        {
          reg(registers, instr.operand - 1) = unresolvedResult ? 1 : 0;
          return;
        }

        auto const fieldText = readStringFieldValue(track, field, customKey);
        auto const constantText = stringConstant(plan, rhs);
        reg(registers, instr.operand - 1) = std::invoke(std::forward<Op>(op), fieldText, constantText) ? 1 : 0;
        return;
      }

      if (isDictionaryField(field) && isOrderedComparison(instr.op))
      {
        auto const fieldText = readDictionaryFieldValue(track, field, dictionary);
        auto const constantText = stringConstant(plan, rhs);
        reg(registers, instr.operand - 1) = std::invoke(std::forward<Op>(op), fieldText, constantText) ? 1 : 0;
        return;
      }

      if (isDictionaryField(field) && instr.dictionarySymbol != kNoDictionarySymbol)
      {
        auto const boundId = boundDictionaryId(dictionaryIds, instr.dictionarySymbol);

        if (boundId == kInvalidDictionaryId)
        {
          reg(registers, instr.operand - 1) = unresolvedResult ? 1 : 0;
          return;
        }

        auto const lhs = reg(registers, instr.operand - 1);
        reg(registers, instr.operand - 1) =
          std::invoke(std::forward<Op>(op), lhs, static_cast<std::int64_t>(boundId.raw())) ? 1 : 0;
        return;
      }

      auto const lhs = reg(registers, instr.operand - 1);
      reg(registers, instr.operand - 1) = std::invoke(std::forward<Op>(op), lhs, rhs) ? 1 : 0;
    }

    void executeEq(std::vector<std::int64_t>& registers,
                   library::TrackView const& track,
                   ExecutionPlan const& plan,
                   std::span<DictionaryId const> dictionaryIds,
                   Instruction const& instr)
    {
      auto const field = static_cast<Field>(instr.field);

      if (field == Field::Tag)
      {
        auto const tagId = boundDictionaryId(dictionaryIds, instr.dictionarySymbol);
        reg(registers, instr.operand - 1) = tagId != kInvalidDictionaryId && track.tags().has(tagId) ? 1 : 0;
        return;
      }

      if (isStringField(field))
      {
        auto const customKey = boundDictionaryId(dictionaryIds, instr.dictionarySymbol);

        if (field == Field::Custom && customKey == kInvalidDictionaryId)
        {
          reg(registers, instr.operand - 1) = 0;
          return;
        }

        auto const fieldText = readStringFieldValue(track, field, customKey);
        auto const constantText = stringConstant(plan, reg(registers, instr.operand));
        reg(registers, instr.operand - 1) = fieldText == constantText ? 1 : 0;
        return;
      }

      if (isDictionaryField(field) && instr.dictionarySymbol != kNoDictionarySymbol)
      {
        auto const boundId = boundDictionaryId(dictionaryIds, instr.dictionarySymbol);
        reg(registers, instr.operand - 1) =
          boundId != kInvalidDictionaryId && std::cmp_equal(reg(registers, instr.operand - 1), boundId.raw()) ? 1 : 0;
        return;
      }

      auto const rhs = reg(registers, instr.operand);
      auto const lhs = reg(registers, instr.operand - 1);
      reg(registers, instr.operand - 1) = lhs == rhs ? 1 : 0;
    }

    void executeLike(std::vector<std::int64_t>& registers,
                     library::TrackView const& track,
                     ExecutionPlan const& plan,
                     library::DictionaryReadContext* dictionary,
                     std::span<DictionaryId const> dictionaryIds,
                     Instruction const& instr)
    {
      auto const field = static_cast<Field>(instr.field);
      auto fieldText = std::string_view{};

      if (isStringField(field))
      {
        auto const customKey = boundDictionaryId(dictionaryIds, instr.dictionarySymbol);

        if (field == Field::Custom && customKey == kInvalidDictionaryId)
        {
          reg(registers, instr.operand - 1) = 0;
          return;
        }

        fieldText = readStringFieldValue(track, field, customKey);
      }
      else if (isDictionaryField(field))
      {
        fieldText = readDictionaryFieldValue(track, field, dictionary);
      }

      auto const constantText = stringConstant(plan, reg(registers, instr.operand));
      reg(registers, instr.operand - 1) = fieldText.contains(constantText) ? 1 : 0;
    }

    bool executeExists(library::TrackView const& track,
                       std::span<DictionaryId const> dictionaryIds,
                       Instruction const& instr)
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
        case Field::ConductorId:
        case Field::EnsembleId:
        case Field::WorkId:
        case Field::MovementId:
        case Field::SoloistId:
          return readFieldValue(track, field) != static_cast<std::int64_t>(kInvalidDictionaryId.raw());

        case Field::CoverArtId:
          return track.coverArt().primary().value_or(library::CoverArt{}).resourceId != kInvalidResourceId;

        case Field::Year:
        case Field::TrackNumber:
        case Field::TrackTotal:
        case Field::DiscNumber:
        case Field::DiscTotal:
        case Field::MovementNumber:
        case Field::MovementTotal:
        case Field::Duration:
        case Field::Bitrate:
        case Field::SampleRate:
        case Field::Channels:
        case Field::BitDepth: return readFieldValue(track, field) > 0;

        case Field::Codec: return track.property().codec() != AudioCodec::Unknown;

        case Field::Tag:
        {
          auto const tagId = boundDictionaryId(dictionaryIds, instr.dictionarySymbol);
          return tagId != kInvalidDictionaryId && track.tags().has(tagId);
        }
        case Field::Custom:
        {
          auto const customKey = boundDictionaryId(dictionaryIds, instr.dictionarySymbol);
          return customKey != kInvalidDictionaryId && track.customMetadata().contains(customKey);
        }
        case Field::TagBloom:
        case Field::TagCount: return readFieldValue(track, field) > 0;

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
                      ExecutionPlan const& plan,
                      std::span<DictionaryId const> dictionaryIds,
                      std::span<boost::unordered_flat_set<std::int64_t> const> dictionarySets,
                      Instruction const& instr)
    {
      if (instr.constValue < 0)
      {
        reg(registers, instr.operand) = 0;
        return;
      }

      auto const setIndex = static_cast<std::size_t>(instr.constValue);

      if (setIndex >= plan.inSets.size())
      {
        reg(registers, instr.operand) = 0;
        return;
      }

      auto const& set = plan.inSets[setIndex];

      if (set.valueKind == InSetValueKind::String)
      {
        auto const field = static_cast<Field>(instr.field);
        auto const customKey = boundDictionaryId(dictionaryIds, instr.dictionarySymbol);

        if (field == Field::Custom && customKey == kInvalidDictionaryId)
        {
          reg(registers, instr.operand) = 0;
          return;
        }

        auto const fieldValue = readStringFieldValue(track, field, customKey);
        reg(registers, instr.operand) = containsString(set, fieldValue) ? 1 : 0;
        return;
      }

      if (set.valueKind == InSetValueKind::Dictionary)
      {
        gsl_Expects(setIndex < dictionarySets.size());
      }

      auto const& values = set.valueKind == InSetValueKind::Dictionary ? dictionarySets[setIndex] : set.numericValues;
      reg(registers, instr.operand) = values.contains(reg(registers, instr.operand)) ? 1 : 0;
    }
  } // namespace

  struct PlanBinding::Impl final
  {
    explicit Impl(ExecutionPlan const& sourcePlan, library::DictionaryReadContext* readContext)
      : plan{&sourcePlan}, dictionary{readContext}, dictionarySets{sourcePlan.inSets.size()}
    {
      gsl_Expects(!sourcePlan.requiresDictionary || readContext != nullptr);

      dictionaryIds.resize(sourcePlan.dictionarySymbols.size(), kInvalidDictionaryId);

      if (readContext == nullptr)
      {
        return;
      }

      generation = readContext->bind(sourcePlan.dictionarySymbols, dictionaryIds);

      for (std::size_t index = 0; index < sourcePlan.inSets.size(); ++index)
      {
        auto const& sourceSet = sourcePlan.inSets[index];

        if (sourceSet.valueKind != InSetValueKind::Dictionary)
        {
          continue;
        }

        auto& boundSet = dictionarySets[index];
        boundSet.reserve(sourceSet.dictionarySymbols.size());

        for (auto const symbol : sourceSet.dictionarySymbols)
        {
          if (auto const id = boundDictionaryId(dictionaryIds, symbol); id != kInvalidDictionaryId)
          {
            boundSet.insert(static_cast<std::int64_t>(id.raw()));
          }
        }
      }

      bool allRequiredTagsBound = true;

      for (auto const symbol : sourcePlan.requiredTagSymbols)
      {
        auto const id = boundDictionaryId(dictionaryIds, symbol);

        if (id == kInvalidDictionaryId)
        {
          allRequiredTagsBound = false;
          break;
        }

        tagBloomMask |= std::uint32_t{1} << (id.raw() & kBloomBitMask);
      }

      if (!allRequiredTagsBound)
      {
        // The bloom shortcut is sound only when every required symbol has a
        // stable ID. The full evaluator owns unresolved-symbol semantics.
        tagBloomMask = 0;
      }
    }

    ExecutionPlan const* plan;
    library::DictionaryReadContext* dictionary;
    std::vector<DictionaryId> dictionaryIds;
    std::vector<boost::unordered_flat_set<std::int64_t>> dictionarySets;
    std::uint64_t generation = 0;
    std::uint32_t tagBloomMask = 0;
  };

  PlanBinding::PlanBinding(ExecutionPlan const& plan)
    : _implPtr{std::make_unique<Impl>(plan, nullptr)}
  {
  }

  PlanBinding::PlanBinding(ExecutionPlan const& plan, library::DictionaryReadContext& dictionary)
    : _implPtr{std::make_unique<Impl>(plan, &dictionary)}
  {
  }

  PlanBinding::~PlanBinding() = default;
  PlanBinding::PlanBinding(PlanBinding&&) noexcept = default;
  PlanBinding& PlanBinding::operator=(PlanBinding&&) noexcept = default;

  bool PlanEvaluator::matches(PlanBinding const& binding, library::TrackView const& track) const
  {
    auto const& state = *binding._implPtr;
    auto const& plan = *state.plan;
    gsl_Expects(hasRequiredTrackData(plan.accessProfile, track));

    if (plan.matchesAll)
    {
      return true;
    }

    if (state.tagBloomMask != 0 && (track.tags().bloom() & state.tagBloomMask) != state.tagBloomMask)
    {
      return false;
    }

    return evaluateFull(binding, track);
  }

  bool PlanEvaluator::matches(ExecutionPlan const& plan, library::TrackView const& track) const
  {
    gsl_Expects(!plan.requiresDictionary);
    auto const binding = PlanBinding{plan};
    return matches(binding, track);
  }

  bool PlanEvaluator::evaluateFull(PlanBinding const& binding, library::TrackView const& track) const
  {
    auto const& state = *binding._implPtr;
    auto const& plan = *state.plan;
    gsl_Expects(hasRequiredTrackData(plan.accessProfile, track));

    if (plan.matchesAll)
    {
      return true;
    }

    _registers.assign(plan.instructions.size() + kReservedRegisters, 0);

    for (auto const& instr : plan.instructions)
    {
      switch (instr.op)
      {
        case OpCode::LoadField:
          reg(_registers, instr.operand) = readFieldValue(track, static_cast<Field>(instr.field));
          break;

        case OpCode::LoadConstant: reg(_registers, instr.operand) = instr.constValue; break;

        case OpCode::Eq: executeEq(_registers, track, plan, state.dictionaryIds, instr); break;

        case OpCode::Ne:
          executeComparison(_registers,
                            track,
                            plan,
                            state.dictionary,
                            state.dictionaryIds,
                            instr,
                            true,
                            [](auto lhs, auto rhs) { return lhs != rhs; });
          break;

        case OpCode::Lt:
          executeComparison(_registers,
                            track,
                            plan,
                            state.dictionary,
                            state.dictionaryIds,
                            instr,
                            false,
                            [](auto lhs, auto rhs) { return lhs < rhs; });
          break;

        case OpCode::Le:
          executeComparison(_registers,
                            track,
                            plan,
                            state.dictionary,
                            state.dictionaryIds,
                            instr,
                            false,
                            [](auto lhs, auto rhs) { return lhs <= rhs; });
          break;

        case OpCode::Gt:
          executeComparison(_registers,
                            track,
                            plan,
                            state.dictionary,
                            state.dictionaryIds,
                            instr,
                            false,
                            [](auto lhs, auto rhs) { return lhs > rhs; });
          break;

        case OpCode::Ge:
          executeComparison(_registers,
                            track,
                            plan,
                            state.dictionary,
                            state.dictionaryIds,
                            instr,
                            false,
                            [](auto lhs, auto rhs) { return lhs >= rhs; });
          break;

        case OpCode::And:
        {
          auto const rhs = reg(_registers, instr.operand);
          auto const lhs = reg(_registers, instr.operand - 1);
          reg(_registers, instr.operand - 1) = lhs != 0 && rhs != 0 ? 1 : 0;
          break;
        }

        case OpCode::Or:
        {
          auto const rhs = reg(_registers, instr.operand);
          auto const lhs = reg(_registers, instr.operand - 1);
          reg(_registers, instr.operand - 1) = lhs != 0 || rhs != 0 ? 1 : 0;
          break;
        }

        case OpCode::Not: reg(_registers, instr.operand) = reg(_registers, instr.operand) == 0 ? 1 : 0; break;

        case OpCode::Like: executeLike(_registers, track, plan, state.dictionary, state.dictionaryIds, instr); break;

        case OpCode::Exists:
          reg(_registers, instr.operand) = executeExists(track, state.dictionaryIds, instr) ? 1 : 0;
          break;

        case OpCode::InSet:
          executeInSet(_registers, track, plan, state.dictionaryIds, state.dictionarySets, instr);
          break;

        case OpCode::Nop:
        default: break;
      }
    }

    return _registers.empty() || _registers[0] != 0;
  }

  bool PlanEvaluator::evaluateFull(ExecutionPlan const& plan, library::TrackView const& track) const
  {
    gsl_Expects(!plan.requiresDictionary);
    auto const binding = PlanBinding{plan};
    return evaluateFull(binding, track);
  }
} // namespace ao::query
