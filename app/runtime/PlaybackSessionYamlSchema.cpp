// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "PlaybackSessionYamlSchema.h"

#include "PlaybackSessionState.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompiler.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackField.h>
#include <ao/yaml/Serialization.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    bool isValidShuffleMode(ShuffleMode const mode) noexcept
    {
      switch (mode)
      {
        case ShuffleMode::Off:
        case ShuffleMode::On: return true;
      }

      return false;
    }

    bool isValidRepeatMode(RepeatMode const mode) noexcept
    {
      switch (mode)
      {
        case RepeatMode::Off:
        case RepeatMode::One:
        case RepeatMode::All: return true;
      }

      return false;
    }

    Result<> writeSortTerm(ryml::NodeRef node, TrackSortTerm const& term)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("field", static_cast<std::int32_t>(term.field)).scalar("ascending", term.ascending);
      return {};
    }

    Result<TrackSortTerm> readSortTerm(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys = std::to_array<std::string_view>({"field", "ascending"});

      std::int32_t field = 0;
      bool ascending = false;
      auto reader = yaml::MapReader{node, kKeys, context};
      reader.requiredScalar("field", field).requiredScalar("ascending", ascending);

      return std::move(reader).finish(TrackSortTerm{
        .field = static_cast<TrackSortField>(field),
        .ascending = ascending,
      });
    }

    Result<> validatePlaybackSessionState(PlaybackSessionState const& state)
    {
      if (state.sourceListId == kInvalidListId || state.currentTrackId == kInvalidTrackId)
      {
        return makeError(Error::Code::CorruptData, "Playback session contains an invalid source or current track id");
      }

      if (state.anchorIndex > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
          state.positionMs > static_cast<std::uint64_t>(std::chrono::milliseconds::max().count()))
      {
        return makeError(Error::Code::CorruptData, "Playback session anchor or position is out of range");
      }

      if (!std::isfinite(state.volume) || state.volume < 0.0F || state.volume > 1.0F ||
          !isValidShuffleMode(state.shuffleMode) || !isValidRepeatMode(state.repeatMode))
      {
        return makeError(Error::Code::CorruptData, "Playback session contains invalid playback values");
      }

      if (state.sortBy.size() > kPlaybackSessionMaxSortTerms)
      {
        return makeError(Error::Code::CorruptData, "Playback session contains too many sort terms");
      }

      auto seenFields = std::vector<TrackSortField>{};
      seenFields.reserve(state.sortBy.size());

      for (auto const& term : state.sortBy)
      {
        if (static_cast<std::size_t>(term.field) >= kTrackSortFieldCount ||
            std::ranges::contains(seenFields, term.field))
        {
          return makeError(Error::Code::CorruptData, "Playback session contains invalid or duplicate sort fields");
        }

        seenFields.push_back(term.field);
      }

      auto parsed = query::parse(state.quickFilterExpression.empty() ? "true" : state.quickFilterExpression);

      if (!parsed)
      {
        return std::unexpected{parsed.error()};
      }

      if (auto compiled = query::compileQuery(*parsed); !compiled)
      {
        return std::unexpected{compiled.error()};
      }

      return {};
    }
  } // namespace

  Result<> PlaybackSessionYamlSchema::serialize(ryml::NodeRef node, PlaybackSessionState const& state) const
  {
    auto writer = yaml::MapWriter{node};
    writer.scalar("schemaVersion", state.schemaVersion)
      .scalar("sourceListId", state.sourceListId.raw())
      .scalar("quickFilterExpression", state.quickFilterExpression)
      .sequence("sortBy", state.sortBy, writeSortTerm)
      .scalar("currentTrackId", state.currentTrackId.raw())
      .scalar("anchorIndex", state.anchorIndex)
      .scalar("positionMs", state.positionMs)
      .scalar("shuffleMode", static_cast<std::int32_t>(state.shuffleMode))
      .scalar("repeatMode", static_cast<std::int32_t>(state.repeatMode))
      .scalar("volume", state.volume)
      .scalar("muted", state.muted);
    return std::move(writer).finish();
  }

  Result<PlaybackSessionState> PlaybackSessionYamlSchema::deserialize(ryml::ConstNodeRef node,
                                                                      PlaybackSessionState const& /*seed*/) const
  {
    constexpr auto kContext = std::string_view{"playback session"};

    if (auto const result = yaml::requireMap(node, kContext); !result)
    {
      return std::unexpected{result.error()};
    }

    auto schemaVersion = yaml::requireScalar<std::uint32_t>(node, "schemaVersion", kContext);

    if (!schemaVersion)
    {
      return std::unexpected{schemaVersion.error()};
    }

    if (*schemaVersion != kPlaybackSessionSchemaVersion)
    {
      return makeError(
        Error::Code::NotSupported, std::format("Unsupported playback session schema version {}", *schemaVersion));
    }

    constexpr auto kKeys = std::to_array<std::string_view>({"schemaVersion",
                                                            "sourceListId",
                                                            "quickFilterExpression",
                                                            "sortBy",
                                                            "currentTrackId",
                                                            "anchorIndex",
                                                            "positionMs",
                                                            "shuffleMode",
                                                            "repeatMode",
                                                            "volume",
                                                            "muted"});

    std::uint32_t sourceListId = 0;
    std::uint32_t currentTrackId = 0;
    std::int32_t shuffleMode = 0;
    std::int32_t repeatMode = 0;
    auto state = PlaybackSessionState{.schemaVersion = *schemaVersion};
    auto reader = yaml::MapReader{node, kKeys, kContext};
    reader.requiredScalar("sourceListId", sourceListId)
      .requiredScalar("quickFilterExpression", state.quickFilterExpression)
      .requiredSequence("sortBy", state.sortBy, readSortTerm)
      .requiredScalar("currentTrackId", currentTrackId)
      .requiredScalar("anchorIndex", state.anchorIndex)
      .requiredScalar("positionMs", state.positionMs)
      .requiredScalar("shuffleMode", shuffleMode)
      .requiredScalar("repeatMode", repeatMode)
      .requiredScalar("volume", state.volume)
      .requiredScalar("muted", state.muted);

    if (!reader.result())
    {
      return std::unexpected{reader.result().error()};
    }

    state.sourceListId = ListId{sourceListId};
    state.currentTrackId = TrackId{currentTrackId};
    state.shuffleMode = static_cast<ShuffleMode>(shuffleMode);
    state.repeatMode = static_cast<RepeatMode>(repeatMode);

    if (auto const result = validatePlaybackSessionState(state); !result)
    {
      return std::unexpected{result.error()};
    }

    return state;
  }
} // namespace ao::rt
