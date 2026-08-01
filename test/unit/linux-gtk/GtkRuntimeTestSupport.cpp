// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkRuntimeTestSupport.h"

#include "GtkApplicationTestSupport.h"
#include "linux-gtk/app/GtkMainContextExecutor.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/playback/PlaybackService.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>

namespace ao::gtk::test
{
  TrackId addRuntimeTrack(rt::AppRuntime& runtime, library::test::TrackSpec const& spec)
  {
    return rt::test::addRuntimeTrack(runtime, spec, [] { drainGtkEvents(); });
  }

  void updateRuntimeTrack(rt::AppRuntime& runtime,
                          TrackId const trackId,
                          std::move_only_function<void(library::test::TrackSpec&)> updater)
  {
    rt::test::updateRuntimeTrack(runtime, trackId, std::move(updater));
    drainGtkEvents();
  }

  struct GtkRuntimeFixture::State final
  {
    explicit State(std::move_only_function<void(library::MusicLibrary&)> initializeLibrary)
    {
      auto const musicRoot = tempDir.path() / "music";
      auto const databasePath = tempDir.path() / "db";
      auto const configPath = tempDir.path() / "config.yaml";

      std::filesystem::create_directories(musicRoot);
      std::filesystem::create_directories(databasePath);

      if (initializeLibrary)
      {
        auto musicLibrary = library::test::makeTestMusicLibrary(musicRoot, databasePath);
        initializeLibrary(musicLibrary);
      }

      auto configStorePtr = std::make_unique<rt::ConfigStore>(configPath);
      auto executorPtr = std::make_unique<GtkMainContextExecutor>();
      runtimePtr = ao::test::requireValue(rt::AppRuntime::create(rt::AppRuntimeDependencies{
        .executorPtr = std::move(executorPtr),
        .musicRoot = musicRoot,
        .databasePath = databasePath,
        .musicLibraryMapSize = library::test::kTestMusicLibraryMapSize,
        .workspaceConfigStorePtr = std::move(configStorePtr),
      }));
    }

    ao::test::TempDir tempDir;
    std::unique_ptr<rt::AppRuntime> runtimePtr;
  };

  GtkRuntimeFixture::GtkRuntimeFixture(std::move_only_function<void(library::MusicLibrary&)> initializeLibrary)
    : _statePtr{std::make_unique<State>(std::move(initializeLibrary))}
  {
  }

  GtkRuntimeFixture::~GtkRuntimeFixture() = default;

  rt::AppRuntime& GtkRuntimeFixture::runtime()
  {
    return *_statePtr->runtimePtr;
  }

  ao::test::TempDir& GtkRuntimeFixture::tempDir()
  {
    return _statePtr->tempDir;
  }

  std::unique_ptr<rt::AppRuntime> makeRuntime(ao::test::TempDir const& tempDir,
                                              std::move_only_function<void(library::MusicLibrary&)> initializeLibrary)
  {
    auto const databasePath = rt::LibraryPaths{tempDir.path()}.databasePath();

    if (initializeLibrary)
    {
      auto musicLibrary = library::test::makeTestMusicLibrary(tempDir.path(), databasePath);
      initializeLibrary(musicLibrary);
    }

    auto executorPtr = std::make_unique<GtkMainContextExecutor>();
    return ao::test::requireValue(rt::AppRuntime::create(rt::AppRuntimeDependencies{
      .executorPtr = std::move(executorPtr),
      .musicRoot = tempDir.path(),
      .databasePath = databasePath,
      .musicLibraryMapSize = library::test::kTestMusicLibraryMapSize,
      .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(tempDir.path() / "config.yaml"),
    }));
  }

  bool waitForPlaybackSettlement(rt::AppRuntime& runtime,
                                 TrackId const trackId,
                                 std::chrono::milliseconds const timeout)
  {
    return pumpGtkEventsUntil(
      [&] { return runtime.playback().snapshot().transport.nowPlaying.trackId == trackId; }, timeout);
  }
} // namespace ao::gtk::test
