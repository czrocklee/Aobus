// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceEditScript.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <type_traits>
#include <variant>

namespace ao::rt
{
  TrackSourceBatchKind classifyTrackSourceBatch(TrackSourceDeltaBatch const& batch) noexcept
  {
    if (batch.deltas.empty())
    {
      return TrackSourceBatchKind::Invalid;
    }

    if (std::holds_alternative<SourceReset>(batch.deltas.front()))
    {
      return batch.deltas.size() == 1 ? TrackSourceBatchKind::Reset : TrackSourceBatchKind::Invalid;
    }

    if (std::holds_alternative<SourceInvalidated>(batch.deltas.front()))
    {
      return batch.deltas.size() == 1 ? TrackSourceBatchKind::Invalidated : TrackSourceBatchKind::Invalid;
    }

    for (auto const& edit : batch.deltas)
    {
      if (std::holds_alternative<SourceReset>(edit) || std::holds_alternative<SourceInvalidated>(edit))
      {
        return TrackSourceBatchKind::Invalid;
      }
    }

    return TrackSourceBatchKind::Regular;
  }

  Result<delta::RegularTrackEditScript> regularTrackEditScriptOf(TrackSourceDeltaBatch const& batch)
  {
    if (classifyTrackSourceBatch(batch) != TrackSourceBatchKind::Regular)
    {
      return std::unexpected{makeError(Error::Code::InvalidInput, "Track source batch is not a regular edit script")};
    }

    auto script = delta::RegularTrackEditScript{};
    script.edits.reserve(batch.deltas.size());

    for (auto const& edit : batch.deltas)
    {
      std::visit(
        [&script](auto const& range)
        {
          using Range = std::remove_cvref_t<decltype(range)>;

          if constexpr (std::same_as<Range, SourceInsertRange>)
          {
            script.edits.emplace_back(delta::InsertRange{.start = range.start, .trackIds = range.trackIds});
          }
          else if constexpr (std::same_as<Range, SourceRemoveRange>)
          {
            script.edits.emplace_back(delta::RemoveRange{.start = range.start, .trackIds = range.trackIds});
          }
          else if constexpr (std::same_as<Range, SourceUpdateRange>)
          {
            script.edits.emplace_back(delta::UpdateRange{.start = range.start, .trackIds = range.trackIds});
          }
        },
        edit);
    }

    return script;
  }

  TrackSourceDeltaBatch sourceBatchOf(delta::RegularTrackEditScript const& script, std::uint64_t revision)
  {
    auto batch = TrackSourceDeltaBatch{.revision = revision};
    batch.deltas.reserve(script.edits.size());

    for (auto const& edit : script.edits)
    {
      std::visit(
        [&batch](auto const& range)
        {
          using Range = std::remove_cvref_t<decltype(range)>;

          if constexpr (std::same_as<Range, delta::InsertRange>)
          {
            batch.deltas.push_back(SourceInsertRange{.start = range.start, .trackIds = range.trackIds});
          }
          else if constexpr (std::same_as<Range, delta::RemoveRange>)
          {
            batch.deltas.push_back(SourceRemoveRange{.start = range.start, .trackIds = range.trackIds});
          }
          else
          {
            batch.deltas.push_back(SourceUpdateRange{.start = range.start, .trackIds = range.trackIds});
          }
        },
        edit);
    }

    return batch;
  }

  bool validateTrackSourceDeltaBatch(TrackSourceDeltaBatch const& batch, std::size_t initialSize)
  {
    auto const kind = classifyTrackSourceBatch(batch);

    if (kind == TrackSourceBatchKind::Reset || kind == TrackSourceBatchKind::Invalidated)
    {
      return true;
    }

    if (kind != TrackSourceBatchKind::Regular)
    {
      return false;
    }

    auto const script = regularTrackEditScriptOf(batch);
    return script && delta::validate(*script, initialSize);
  }
} // namespace ao::rt
