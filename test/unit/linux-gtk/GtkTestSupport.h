// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/StateTypes.h>
#include <test/unit/lmdb/TestUtils.h>

#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <gtkmm/application.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  /**
   * ImmediateExecutor - Standalone test double for IControlExecutor.
   * Executes tasks immediately on the caller's thread.
   */
  class ImmediateExecutor final : public rt::IControlExecutor
  {
  public:
    bool isCurrent() const noexcept override { return true; }
    void dispatch(std::move_only_function<void()> task) override { task(); }
    void defer(std::move_only_function<void()> task) override { task(); }
  };

  /**
   * ensureGtkApplication - Ensures a Gtk::Application instance exists for the test.
   */
  inline Glib::RefPtr<Gtk::Application> ensureGtkApplication()
  {
    static bool logHandlerInstalled = false;

    if (!logHandlerInstalled)
    {
      ::g_log_set_writer_func(
        [](GLogLevelFlags logLevel, GLogField const* fields, gsize nFields, gpointer) -> GLogWriterOutput
        {
          for (gsize i = 0; i < nFields; i++)
          {
            if (std::string_view{fields[i].key} == "MESSAGE")
            {
              auto msg = std::string_view{};

              if (fields[i].length < 0)
              {
                msg = static_cast<char const*>(fields[i].value);
              }
              else
              {
                msg = std::string_view{
                  static_cast<char const*>(fields[i].value), static_cast<std::size_t>(fields[i].length)};
              }

              if (msg.find("Finalizing ") != std::string_view::npos &&
                  msg.find("has children left") != std::string_view::npos)
              {
                return G_LOG_WRITER_HANDLED;
              }
            }
          }

          return ::g_log_writer_default(logLevel, fields, nFields, nullptr);
        },
        nullptr,
        nullptr);
      logHandlerInstalled = true;
    }

    if (auto gioApp = Gio::Application::get_default())
    {
      auto gtkApp = std::dynamic_pointer_cast<Gtk::Application>(gioApp);

      if (gtkApp)
      {
        return gtkApp;
      }
    }

    return Gtk::Application::create("io.github.aobus.test", Gio::Application::Flags::NON_UNIQUE);
  }

  /**
   * drainGtkEvents - Drains the GTK event loop.
   */
  inline void drainGtkEvents()
  {
    auto context = Glib::MainContext::get_default();

    while (context->pending())
    {
      context->iteration(false);
    }
  }

  /**
   * GtkRuntimeFixture - RAII fixture for AppRuntime with temporary storage.
   */
  class GtkRuntimeFixture
  {
  public:
    GtkRuntimeFixture()
    {
      auto const musicRoot = std::filesystem::path{_tempDir.path()} / "music";
      auto const databasePath = std::filesystem::path{_tempDir.path()} / "db";
      auto const configPath = std::filesystem::path{_tempDir.path()} / "config.yaml";

      std::filesystem::create_directories(musicRoot);
      std::filesystem::create_directories(databasePath);

      auto configStore = std::make_shared<rt::ConfigStore>(configPath);

      _runtime = std::make_unique<rt::AppRuntime>(rt::AppRuntimeDependencies{
        .executor = std::make_unique<ImmediateExecutor>(),
        .musicRoot = musicRoot,
        .databasePath = databasePath,
        .workspaceConfigStore = std::move(configStore),
      });
    }

    rt::AppRuntime& runtime() { return *_runtime; }
    lmdb::test::TempDir& tempDir() { return _tempDir; }

  private:
    lmdb::test::TempDir _tempDir;
    std::unique_ptr<rt::AppRuntime> _runtime;
  };

  /**
   * ManualTrackDetailMock - Manual mock for ITrackDetailProjection.
   */
  class ManualTrackDetailMock final : public rt::ITrackDetailProjection
  {
  public:
    rt::TrackDetailSnapshot snapshot() const override { return _snap; }

    rt::Subscription subscribe(std::move_only_function<void(rt::TrackDetailSnapshot const&)> handler) override
    {
      _handler = std::move(handler);
      return rt::Subscription{};
    }

    void emit(rt::TrackDetailSnapshot const& snap)
    {
      if (_handler)
      {
        _handler(snap);
      }
    }

  private:
    rt::TrackDetailSnapshot _snap;
    std::move_only_function<void(rt::TrackDetailSnapshot const&)> _handler;
  };

  /**
   * RenderLog - A simple sink for asserting on rendered view states.
   */
  template<typename TState>
  struct RenderLog
  {
    std::vector<TState> states;

    void render(TState state) { states.push_back(std::move(state)); }

    bool empty() const noexcept { return states.empty(); }
    TState const& last() const { return states.back(); }
    void clear() { states.clear(); }
  };

  /**
   * FakePlaybackEvents - A fake event source for testing binder subscription wiring.
   */
  class FakePlaybackEvents
  {
  public:
    rt::Subscription onStarted(std::move_only_function<void()> handler)
    {
      _started = std::move(handler);
      return rt::Subscription{[this] { _started = nullptr; }};
    }

    rt::Subscription onPaused(std::move_only_function<void()> handler)
    {
      _paused = std::move(handler);
      return rt::Subscription{[this] { _paused = nullptr; }};
    }

    rt::Subscription onStopped(std::move_only_function<void()> handler)
    {
      _stopped = std::move(handler);
      return rt::Subscription{[this] { _stopped = nullptr; }};
    }

    rt::Subscription onIdle(std::move_only_function<void()> handler)
    {
      _idle = std::move(handler);
      return rt::Subscription{[this] { _idle = nullptr; }};
    }

    rt::Subscription onPreparing(std::move_only_function<void()> handler)
    {
      _preparing = std::move(handler);
      return rt::Subscription{[this] { _preparing = nullptr; }};
    }

    rt::Subscription onSeekUpdate(std::move_only_function<void(rt::PlaybackService::SeekUpdate const&)> handler)
    {
      _seekUpdate = std::move(handler);
      return rt::Subscription{[this] { _seekUpdate = nullptr; }};
    }

    rt::Subscription onOutputChanged(std::move_only_function<void(rt::OutputSelection const&)> handler)
    {
      _outputChanged = std::move(handler);
      return rt::Subscription{[this] { _outputChanged = nullptr; }};
    }

    rt::Subscription onQualityChanged(std::move_only_function<void(rt::PlaybackService::QualityChanged const&)> handler)
    {
      _qualityChanged = std::move(handler);
      return rt::Subscription{[this] { _qualityChanged = nullptr; }};
    }

    rt::Subscription onShuffleModeChanged(
      std::move_only_function<void(rt::PlaybackService::ShuffleModeChanged const&)> handler)
    {
      _shuffleModeChanged = std::move(handler);
      return rt::Subscription{[this] { _shuffleModeChanged = nullptr; }};
    }

    rt::Subscription onRepeatModeChanged(
      std::move_only_function<void(rt::PlaybackService::RepeatModeChanged const&)> handler)
    {
      _repeatModeChanged = std::move(handler);
      return rt::Subscription{[this] { _repeatModeChanged = nullptr; }};
    }

    rt::Subscription onVolumeChanged(std::move_only_function<void(float)> handler)
    {
      _volumeChanged = std::move(handler);
      return rt::Subscription{[this] { _volumeChanged = nullptr; }};
    }

    void emitStarted()
    {
      if (_started)
      {
        _started();
      }
    }

    void emitPaused()
    {
      if (_paused)
      {
        _paused();
      }
    }

    void emitStopped()
    {
      if (_stopped)
      {
        _stopped();
      }
    }

    void emitIdle()
    {
      if (_idle)
      {
        _idle();
      }
    }

    void emitPreparing()
    {
      if (_preparing)
      {
        _preparing();
      }
    }

    void emitSeekUpdate(rt::PlaybackService::SeekUpdate const& update)
    {
      if (_seekUpdate)
      {
        _seekUpdate(update);
      }
    }

    void emitOutputChanged(rt::OutputSelection const& sel)
    {
      if (_outputChanged)
      {
        _outputChanged(sel);
      }
    }

    void emitQualityChanged(rt::PlaybackService::QualityChanged const& q)
    {
      if (_qualityChanged)
      {
        _qualityChanged(q);
      }
    }

    void emitShuffleModeChanged(rt::PlaybackService::ShuffleModeChanged const& m)
    {
      if (_shuffleModeChanged)
      {
        _shuffleModeChanged(m);
      }
    }

    void emitRepeatModeChanged(rt::PlaybackService::RepeatModeChanged const& m)
    {
      if (_repeatModeChanged)
      {
        _repeatModeChanged(m);
      }
    }

    void emitVolumeChanged(float v)
    {
      if (_volumeChanged)
      {
        _volumeChanged(v);
      }
    }

  private:
    rt::PlaybackState _state;
    std::move_only_function<void()> _started;
    std::move_only_function<void()> _paused;
    std::move_only_function<void()> _stopped;
    std::move_only_function<void()> _idle;
    std::move_only_function<void()> _preparing;
    std::move_only_function<void(rt::PlaybackService::SeekUpdate const&)> _seekUpdate;
    std::move_only_function<void(rt::OutputSelection const&)> _outputChanged;
    std::move_only_function<void(rt::PlaybackService::QualityChanged const&)> _qualityChanged;
    std::move_only_function<void(rt::PlaybackService::ShuffleModeChanged const&)> _shuffleModeChanged;
    std::move_only_function<void(rt::PlaybackService::RepeatModeChanged const&)> _repeatModeChanged;
    std::move_only_function<void(float)> _volumeChanged;
  };
} // namespace ao::gtk::test
