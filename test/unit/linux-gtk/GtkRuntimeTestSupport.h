// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <chrono>
#include <functional>
#include <memory>

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
}

namespace ao::gtk::test
{
  TrackId addRuntimeTrack(rt::AppRuntime& runtime, library::test::TrackSpec const& spec);
  void updateRuntimeTrack(rt::AppRuntime& runtime,
                          TrackId trackId,
                          std::move_only_function<void(library::test::TrackSpec&)> updater);

  class GtkRuntimeFixture final
  {
  public:
    explicit GtkRuntimeFixture(std::move_only_function<void(library::MusicLibrary&)> initializeLibrary = {});
    ~GtkRuntimeFixture();

    GtkRuntimeFixture(GtkRuntimeFixture const&) = delete;
    GtkRuntimeFixture& operator=(GtkRuntimeFixture const&) = delete;
    GtkRuntimeFixture(GtkRuntimeFixture&&) = delete;
    GtkRuntimeFixture& operator=(GtkRuntimeFixture&&) = delete;

    rt::AppRuntime& runtime();
    ao::test::TempDir& tempDir();

  private:
    struct State;
    std::unique_ptr<State> _statePtr;
  };

  std::unique_ptr<rt::AppRuntime> makeRuntime(
    ao::test::TempDir const& tempDir,
    std::move_only_function<void(library::MusicLibrary&)> initializeLibrary = {});

  bool waitForPlaybackSettlement(rt::AppRuntime& runtime,
                                 TrackId trackId,
                                 std::chrono::milliseconds timeout = std::chrono::seconds{2});
} // namespace ao::gtk::test
