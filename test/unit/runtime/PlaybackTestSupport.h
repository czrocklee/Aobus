// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/RenderTarget.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <span>
#include <tuple>
#include <utility>

namespace ao::async
{
  class Executor;
  class Runtime;
}

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class NotificationService;
}

namespace ao::rt::test
{
  PlaybackTransport makePlaybackTransport(async::Executor& executor,
                                          library::MusicLibrary& library,
                                          NotificationService& notifications);

  PlaybackTransport makePlaybackTransport(async::Runtime& runtime,
                                          library::MusicLibrary& library,
                                          NotificationService& notifications);

  template<typename ExecutorT, typename PositionRevisionFn>
  bool waitForPlaybackSettlement(ExecutorT& executor,
                                 PlaybackPositionRevision const previousRevision,
                                 PositionRevisionFn positionRevision,
                                 std::chrono::milliseconds const timeout = std::chrono::seconds{2})
  {
    return executor.drainUntil([&] { return positionRevision() != previousRevision; }, timeout);
  }

  template<typename ExecutorT, typename AdmissionFn, typename PositionRevisionFn>
  Result<> admitPlaybackAndWait(ExecutorT& executor,
                                AdmissionFn admit,
                                PositionRevisionFn positionRevision,
                                std::chrono::milliseconds const timeout = std::chrono::seconds{2})
  {
    auto const previousRevision = positionRevision();

    if (auto admitted = admit(); !admitted)
    {
      return admitted;
    }

    if (!waitForPlaybackSettlement(executor, previousRevision, std::move(positionRevision), timeout))
    {
      return makeError(Error::Code::InvalidState, "Timed out waiting for playback settlement");
    }

    return {};
  }

  bool driveRenderUntilTaskQueued(audio::RenderTarget& renderTarget,
                                  QueuedExecutor& executor,
                                  std::span<std::byte> output,
                                  std::chrono::milliseconds timeout = std::chrono::seconds{5});

  template<typename Predicate>
  bool driveRenderUntil(audio::RenderTarget& renderTarget,
                        QueuedExecutor& executor,
                        std::span<std::byte> output,
                        Predicate predicate,
                        std::chrono::milliseconds timeout = std::chrono::seconds{5})
  {
    auto const deadline = std::chrono::steady_clock::now() + timeout;

    while (!predicate())
    {
      executor.drain();

      if (predicate())
      {
        return true;
      }

      renderTarget.renderPcm(output);
      auto const now = std::chrono::steady_clock::now();

      if (now >= deadline)
      {
        return predicate();
      }

      auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      auto const pollInterval = std::min(remaining, std::chrono::milliseconds{1});
      std::ignore = executor.waitUntilQueued(pollInterval);
    }

    return true;
  }
} // namespace ao::rt::test
