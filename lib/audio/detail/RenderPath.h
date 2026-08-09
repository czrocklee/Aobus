// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "RenderTimeline.h"
#include <ao/audio/PcmFormat.h>
#include <ao/audio/RenderTarget.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::audio::detail
{
  struct RenderProgress final
  {
    std::size_t bytesWritten = 0;
    std::size_t positionStartBytes = 0;
    std::size_t positionBytes = 0;

    void advance(std::size_t bytes) noexcept;

    void startNewPositionSegment() noexcept;

    RenderPcmResult result(std::atomic<std::uint32_t> const& engineFrameBytes, bool drained = false) const noexcept;
  };

  template<typename IsActive, typename EnqueueSpliced>
  bool trySplicePreparedNext(RenderTimeline& timeline,
                             std::atomic<bool>& spliceHandoffInProgress,
                             std::uint64_t generation,
                             IsActive isActive,
                             std::atomic<std::uint64_t>& accumulatedFrames,
                             std::atomic<std::uint32_t>& engineSampleRate,
                             std::atomic<std::uint32_t>& engineFrameBytes,
                             std::atomic<std::uint32_t>& underrunCount,
                             std::atomic<bool>& playbackDrainPending,
                             EnqueueSpliced enqueueSpliced) noexcept
  {
    spliceHandoffInProgress.store(true, std::memory_order_release);
    auto* session = timeline.consumeLookaheadForRender();

    if (session == nullptr)
    {
      spliceHandoffInProgress.store(false, std::memory_order_release);
      return false;
    }

    bool const active = isActive(generation);

    if (active)
    {
      timeline.publishActive(session);
      accumulatedFrames.store(0, std::memory_order_relaxed);
      engineSampleRate.store(session->info.outputFormat.sampleRate, std::memory_order_relaxed);
      engineFrameBytes.store(
        static_cast<std::uint32_t>(frameBytes(session->info.outputFormat)), std::memory_order_relaxed);
      underrunCount.store(0, std::memory_order_relaxed);
      playbackDrainPending.store(false, std::memory_order_relaxed);
    }

    // The event/control consumer promotes the lookahead node to current when
    // `active`, or discards it otherwise. Signal publication is mandatory: a
    // violated ring-capacity plan aborts on this thread and never continues with
    // an unreported timeline owner.
    enqueueSpliced(generation, session);
    spliceHandoffInProgress.store(false, std::memory_order_release);
    return active;
  }

  template<typename IsActive, typename TrySplice>
  RenderPcmResult renderPcm(RenderTimeline& timeline,
                            std::atomic<std::uint32_t> const& engineFrameBytes,
                            std::atomic<bool>& playbackDrainPending,
                            std::uint64_t generation,
                            std::span<std::byte> output,
                            IsActive isActive,
                            TrySplice trySplice) noexcept
  {
    if (!isActive(generation))
    {
      return {.drained = true};
    }

    auto progress = RenderProgress{};
    std::size_t spliceCount = 0;
    // This bounds sequential work in one render call, not simultaneous RT-ring
    // occupancy. Each splice consumes the sole armed lookahead; another splice
    // requires control-side promotion and re-arm first.
    constexpr std::size_t kMaxSplicesPerRender = 8;

    while (progress.bytesWritten < output.size())
    {
      auto* const source = timeline.activeSource();

      if (source == nullptr)
      {
        return progress.result(engineFrameBytes, progress.bytesWritten == 0);
      }

      auto tail = output.subspan(progress.bytesWritten);
      auto const bytesRead = source->read(tail);
      progress.advance(bytesRead);

      if (progress.bytesWritten == output.size())
      {
        return progress.result(engineFrameBytes);
      }

      if (!source->isDrained())
      {
        return progress.result(engineFrameBytes);
      }

      if (spliceCount++ < kMaxSplicesPerRender && trySplice(generation))
      {
        progress.startNewPositionSegment();
        continue;
      }

      playbackDrainPending.store(true, std::memory_order_release);
      return progress.result(engineFrameBytes, progress.bytesWritten == 0);
    }

    return progress.result(engineFrameBytes);
  }
} // namespace ao::audio::detail
