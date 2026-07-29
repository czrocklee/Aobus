// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/PlaybackTestSupport.h"

#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/async/Executor.h>
#include <ao/async/Runtime.h>
#include <ao/audio/Player.h>
#include <ao/audio/RenderTarget.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/NotificationService.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>

namespace ao::rt::test
{
  PlaybackTransport makePlaybackTransport(async::Executor& executor,
                                          library::MusicLibrary& library,
                                          NotificationService& notifications)
  {
    return PlaybackTransport{executor, library, notifications, std::make_unique<audio::Player>(executor)};
  }

  PlaybackTransport makePlaybackTransport(async::Runtime& runtime,
                                          library::MusicLibrary& library,
                                          NotificationService& notifications)
  {
    return PlaybackTransport{
      runtime.callbackExecutor(), library, notifications, std::make_unique<audio::Player>(runtime)};
  }

  bool driveRenderUntilTaskQueued(audio::RenderTarget& renderTarget,
                                  QueuedExecutor& executor,
                                  std::span<std::byte> output,
                                  std::chrono::milliseconds const timeout)
  {
    auto const deadline = std::chrono::steady_clock::now() + timeout;

    while (executor.queuedCount() == 0)
    {
      renderTarget.renderPcm(output);
      auto const now = std::chrono::steady_clock::now();

      if (now >= deadline)
      {
        return false;
      }

      auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      auto const pollInterval = std::min(remaining, std::chrono::milliseconds{1});

      if (executor.waitUntilQueued(pollInterval))
      {
        return true;
      }
    }

    return true;
  }
} // namespace ao::rt::test
