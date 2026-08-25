// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/async/Runtime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <memory>

namespace ao::rt::test
{
  /**
   * @brief Composes PlaybackService over the real transport and succession owners for
   *        consumer and view-model tests.
   *
   * InlineExecutor suits state-only view-model tests. Tests that play real
   * audio through the succession owner must use QueuedExecutor and drain
   * explicitly to avoid synchronous re-entry into audio::Player.
   */
  template<typename ExecutorT = InlineExecutor>
  struct ApplicationPlaybackFixtureT final
  {
    ApplicationPlaybackFixtureT() = default;

    ApplicationPlaybackFixtureT(ApplicationPlaybackFixtureT const&) = delete;
    ApplicationPlaybackFixtureT& operator=(ApplicationPlaybackFixtureT const&) = delete;
    ApplicationPlaybackFixtureT(ApplicationPlaybackFixtureT&&) = delete;
    ApplicationPlaybackFixtureT& operator=(ApplicationPlaybackFixtureT&&) = delete;
    ~ApplicationPlaybackFixtureT() = default;

    void addReadyProvider() { playbackBootstrap.addProvider(makeReadyAudioProvider()); }

    PlaybackCommands& commands() { return playback.commands(); }
    LibraryWriter& writer() { return writerFixture.writer(); }

    MusicLibraryFixture libraryFixture;
    ControlledSleeper sleeper;
    ExecutorT executor;
    async::Runtime asyncRuntime{executor, 1, &sleeper};
    LibraryChanges changes{executor, 0, "test-library"};
    LibraryWriterFixture writerFixture{libraryFixture.library(), changes, executor};
    TrackSourceCache sources{libraryFixture.library(), changes};
    ViewService views{executor, libraryFixture.library(), sources, changes};
    WorkspaceService workspace{executor, views, changes};
    NotificationService notifications{asyncRuntime};
    PlaybackTransport playbackTransport{makePlaybackTransport(asyncRuntime, libraryFixture.library(), notifications)};
    PlaybackSuccession
      succession{executor, views, sources, libraryFixture.library(), playbackTransport, notifications, asyncRuntime};
    PlaybackBootstrap playbackBootstrap{playbackTransport};
    std::unique_ptr<PlaybackService> playbackPtr{playbackBootstrap.createPlaybackService(executor, succession)};
    PlaybackService& playback{*playbackPtr};
  };

  using ApplicationPlaybackFixture = ApplicationPlaybackFixtureT<>;
} // namespace ao::rt::test
