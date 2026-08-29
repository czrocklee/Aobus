// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "GtkApplicationTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <type_traits>
#include <utility>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::library::test
{
  struct TrackSpec;
}

namespace ao::test
{
  class TempDir;
}

namespace ao::rt
{
  class AppRuntime;
  class TextOrderingPolicy;
}

namespace ao::gtk::test
{
  template<typename T>
  async::Task<T> runGtkTaskOnCallback(async::Runtime* runtime, async::Task<T> task)
  {
    co_await runtime->resumeOnCallbackExecutor();

    if constexpr (std::is_void_v<T>)
    {
      co_await std::move(task);
      co_return;
    }
    else
    {
      auto result = co_await std::move(task);
      co_return std::move(result);
    }
  }

  template<typename T>
  T runGtkTask(rt::AppRuntime& runtime, async::Task<T> task)
  {
    auto pump = [] { drainGtkEvents(); };
    return rt::test::runRuntimeTask(runtime, runGtkTaskOnCallback(&runtime.async(), std::move(task)), pump);
  }

  TrackId addRuntimeTrack(rt::AppRuntime& runtime, library::test::TrackSpec const& spec);
  void updateRuntimeTrack(rt::AppRuntime& runtime,
                          TrackId trackId,
                          compat::MoveOnlyFunction<void(library::test::TrackSpec&)> updater);

  class GtkRuntimeFixture final
  {
  public:
    explicit GtkRuntimeFixture(compat::MoveOnlyFunction<void(library::MusicLibrary&)> initializeLibrary = {},
                               rt::TextOrderingPolicy const* textOrderingPolicy = nullptr);
    ~GtkRuntimeFixture();

    GtkRuntimeFixture(GtkRuntimeFixture const&) = delete;
    GtkRuntimeFixture& operator=(GtkRuntimeFixture const&) = delete;
    GtkRuntimeFixture(GtkRuntimeFixture&&) = delete;
    GtkRuntimeFixture& operator=(GtkRuntimeFixture&&) = delete;

    rt::AppRuntime& runtime();
    ao::test::TempDir& tempDir();

    /// Where this fixture's runtime keeps its derived caches, including the
    /// cover cache a resource request reads content through.
    std::filesystem::path cacheDirectory() const;

  private:
    struct State;
    std::unique_ptr<State> _statePtr;
  };

  /// The cache directory a fixture or `makeRuntime` composition supplies for a
  /// runtime rooted at @p tempPath.
  std::filesystem::path runtimeCacheDirectory(std::filesystem::path const& tempPath);

  std::unique_ptr<rt::AppRuntime> makeRuntime(
    ao::test::TempDir const& tempDir,
    compat::MoveOnlyFunction<void(library::MusicLibrary&)> initializeLibrary = {},
    rt::TextOrderingPolicy const* textOrderingPolicy = nullptr);

  bool waitForPlaybackSettlement(rt::AppRuntime& runtime,
                                 TrackId trackId,
                                 std::chrono::milliseconds timeout = std::chrono::seconds{2});
} // namespace ao::gtk::test
