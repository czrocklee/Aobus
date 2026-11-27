// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkRuntimeTestSupport.h"

#include "GtkApplicationTestSupport.h"
#include "linux-gtk/app/GtkMainContextExecutor.h"
#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/library/LibraryDurability.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/library/LibraryEditing.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>

namespace ao::gtk::test
{
  TrackId addRuntimeTrack(rt::AppRuntimeOwner& owner, library::test::TrackSpec const& spec)
  {
    return rt::test::addRuntimeTrack(owner, spec, [] { drainGtkEvents(); });
  }

  void updateRuntimeTrack(rt::AppRuntimeOwner& owner,
                          TrackId const trackId,
                          compat::MoveOnlyFunction<void(library::test::TrackSpec&)> updater)
  {
    rt::test::updateRuntimeTrack(owner, trackId, std::move(updater), [] { drainGtkEvents(); });
  }

  std::filesystem::path runtimeCacheDirectory(std::filesystem::path const& tempPath)
  {
    return tempPath / "cache";
  }

  struct GtkRuntimeFixture::State final
  {
    explicit State(compat::MoveOnlyFunction<void(library::MusicLibrary&)> initializeLibrary,
                   rt::TextOrderingPolicy const* const textOrderingPolicy)
      : runtimeOwner{openRuntime(tempDir, initializeLibrary, textOrderingPolicy)}
    {
    }

    static rt::AppRuntimeOwner openRuntime(ao::test::TempDir const& tempDir,
                                           compat::MoveOnlyFunction<void(library::MusicLibrary&)>& initializeLibrary,
                                           rt::TextOrderingPolicy const* textOrderingPolicy)
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
      return ao::test::requireValue(rt::AppRuntimeOwner::open(rt::AppRuntimeDependencies{
        .executorPtr = std::move(executorPtr),
        .musicRoot = musicRoot,
        .databasePath = databasePath,
        .cacheDirectory = runtimeCacheDirectory(tempDir.path()),
        .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
        .musicLibraryDurability = rt::LibraryDurability::Unflushed,
        .workspaceConfigStorePtr = std::move(configStorePtr),
        .textOrderingPolicy = textOrderingPolicy,
      }));
    }

    ao::test::TempDir tempDir;
    rt::AppRuntimeOwner runtimeOwner;
  };

  GtkRuntimeFixture::GtkRuntimeFixture(compat::MoveOnlyFunction<void(library::MusicLibrary&)> initializeLibrary,
                                       rt::TextOrderingPolicy const* const textOrderingPolicy)
    : _statePtr{std::make_unique<State>(std::move(initializeLibrary), textOrderingPolicy)}
  {
  }

  GtkRuntimeFixture::~GtkRuntimeFixture() = default;

  rt::AppRuntimeOwner& GtkRuntimeFixture::owner()
  {
    return _statePtr->runtimeOwner;
  }

  rt::AppRuntime& GtkRuntimeFixture::runtime()
  {
    return owner().runtime();
  }

  uimodel::LibraryEditing GtkRuntimeFixture::editing()
  {
    return uimodel::LibraryEditing{owner().libraryCommands()};
  }

  ao::test::TempDir& GtkRuntimeFixture::tempDir()
  {
    return _statePtr->tempDir;
  }

  std::filesystem::path GtkRuntimeFixture::cacheDirectory() const
  {
    return runtimeCacheDirectory(_statePtr->tempDir.path());
  }

  std::unique_ptr<rt::AppRuntimeOwner> makeRuntime(
    ao::test::TempDir const& tempDir,
    compat::MoveOnlyFunction<void(library::MusicLibrary&)> initializeLibrary,
    rt::TextOrderingPolicy const* const textOrderingPolicy)
  {
    auto const databasePath = rt::LibraryPaths{tempDir.path()}.databasePath();

    if (initializeLibrary)
    {
      auto musicLibrary = library::test::makeTestMusicLibrary(tempDir.path(), databasePath);
      initializeLibrary(musicLibrary);
    }

    auto executorPtr = std::make_unique<GtkMainContextExecutor>();
    return std::make_unique<rt::AppRuntimeOwner>(
      ao::test::requireValue(rt::AppRuntimeOwner::open(rt::AppRuntimeDependencies{
        .executorPtr = std::move(executorPtr),
        .musicRoot = tempDir.path(),
        .databasePath = databasePath,
        .cacheDirectory = runtimeCacheDirectory(tempDir.path()),
        .musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
        .musicLibraryDurability = rt::LibraryDurability::Unflushed,
        .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(tempDir.path() / "config.yaml"),
        .textOrderingPolicy = textOrderingPolicy,
      })));
  }

  bool tryWaitForPlaybackSettlement(rt::AppRuntime& runtime,
                                    TrackId const trackId,
                                    std::chrono::milliseconds const timeout)
  {
    return tryPumpGtkEventsUntil(
      [&] { return runtime.playback().snapshot().transport.nowPlaying.trackId == trackId; }, timeout);
  }
} // namespace ao::gtk::test
