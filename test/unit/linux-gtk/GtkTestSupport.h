// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "linux-gtk/app/GtkMainContextExecutor.h"
#include "test/unit/TestUtils.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/projection/ProjectionTypes.h>

#include <glib-object.h>
#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  /**
   * collectAll - Recursively collects all descendant widgets (including root) of type T.
   */
  template<typename T>
  std::vector<T*> collectAll(Gtk::Widget& root)
  {
    auto result = std::vector<T*>{};

    if (auto* const match = dynamic_cast<T*>(&root); match != nullptr)
    {
      result.push_back(match);
    }

    for (auto* child = root.get_first_child(); child != nullptr; child = child->get_next_sibling())
    {
      auto nested = collectAll<T>(*child);
      result.insert(result.end(), nested.begin(), nested.end());
    }

    return result;
  }

  /**
   * collectScrolledWindows - Recursively collects all Gtk::ScrolledWindow widgets.
   */
  inline void collectScrolledWindows(Gtk::Widget& widget, std::vector<Gtk::ScrolledWindow*>& result)
  {
    auto found = collectAll<Gtk::ScrolledWindow>(widget);
    result.insert(result.end(), found.begin(), found.end());
  }

  /**
   * findLabelByText - Returns the first descendant Gtk::Label whose text matches, or nullptr.
   */
  inline Gtk::Label* findLabelByText(Gtk::Widget& root, std::string const& text)
  {
    for (auto* const label : collectAll<Gtk::Label>(root))
    {
      if (label->get_text() == text)
      {
        return label;
      }
    }

    return nullptr;
  }

  /**
   * findButtonByLabel - Returns the first descendant Gtk::Button with the given label, or nullptr.
   */
  inline Gtk::Button* findButtonByLabel(Gtk::Widget& root, std::string const& labelText)
  {
    for (auto* const button : collectAll<Gtk::Button>(root))
    {
      if (button->get_label() == labelText)
      {
        return button;
      }
    }

    return nullptr;
  }

  /**
   * hasCssClass - True if the widget carries the given CSS class.
   */
  inline bool hasCssClass(Gtk::Widget const& widget, std::string_view cssClass)
  {
    auto const classes = widget.get_css_classes();
    return std::ranges::any_of(classes, [&](auto const& name) { return std::string_view{name.raw()} == cssClass; });
  }

  /**
   * emitClicked - Emits the GTK "clicked" signal on a button.
   */
  inline void emitClicked(Gtk::Button& button)
  {
    ::g_signal_emit_by_name(button.gobj(), "clicked");
  }

  /**
   * emitActivate - Emits the GTK "activate" signal on an entry.
   */
  inline void emitActivate(Gtk::Entry& entry)
  {
    ::g_signal_emit_by_name(entry.gobj(), "activate");
  }

  /**
   * emitClosed - Emits the GTK "closed" signal on a popover.
   */
  inline void emitClosed(Gtk::Popover& popover)
  {
    ::g_signal_emit_by_name(popover.gobj(), "closed");
  }

  /**
   * emitShow - Emits the GTK "show" signal on a widget.
   */
  inline void emitShow(Gtk::Widget& widget)
  {
    ::g_signal_emit_by_name(widget.gobj(), "show");
  }

  /**
   * findController - Returns the first controller of type T installed on a widget.
   */
  template<typename T>
  Glib::RefPtr<T> findController(Gtk::Widget& widget)
  {
    auto const controllersPtr = widget.observe_controllers();

    if (!controllersPtr)
    {
      return {};
    }

    auto const count = controllersPtr->get_n_items();

    for (std::uint32_t i = 0U; i < count; ++i)
    {
      if (auto const controllerPtr = std::dynamic_pointer_cast<T>(controllersPtr->get_object(i)); controllerPtr)
      {
        return controllerPtr;
      }
    }

    return {};
  }

  /**
   * findControllerIf - Returns the first controller of type T matching a predicate.
   */
  template<typename T, typename Predicate>
  Glib::RefPtr<T> findControllerIf(Gtk::Widget& widget, Predicate const& predicate)
  {
    auto const controllersPtr = widget.observe_controllers();

    if (!controllersPtr)
    {
      return {};
    }

    auto const count = controllersPtr->get_n_items();

    for (std::uint32_t i = 0U; i < count; ++i)
    {
      auto const controllerPtr = std::dynamic_pointer_cast<T>(controllersPtr->get_object(i));

      if (controllerPtr && predicate(*controllerPtr))
      {
        return controllerPtr;
      }
    }

    return {};
  }

  /**
   * hasController - True if the widget has a controller of type T.
   */
  template<typename T>
  bool hasController(Gtk::Widget& widget)
  {
    return findController<T>(widget) != nullptr;
  }

  /**
   * emitFocusEnter - Emits the focus controller "enter" signal when present.
   */
  inline bool emitFocusEnter(Gtk::Widget& widget)
  {
    auto const focusControllerPtr = findController<Gtk::EventControllerFocus>(widget);

    if (!focusControllerPtr)
    {
      return false;
    }

    ::g_signal_emit_by_name(focusControllerPtr->gobj(), "enter");
    return true;
  }

  /**
   * emitFocusLeave - Emits the focus controller "leave" signal when present.
   */
  inline bool emitFocusLeave(Gtk::Widget& widget)
  {
    auto const focusControllerPtr = findController<Gtk::EventControllerFocus>(widget);

    if (!focusControllerPtr)
    {
      return false;
    }

    ::g_signal_emit_by_name(focusControllerPtr->gobj(), "leave");
    return true;
  }

  /**
   * emitGesturePressed - Emits "pressed" on the first matching GestureClick controller.
   */
  inline bool emitGesturePressed(Gtk::Widget& widget,
                                 int const nPress = 1,
                                 double const x = 1.0,
                                 double const y = 1.0,
                                 std::optional<Gtk::PropagationPhase> const optPhase = std::nullopt)
  {
    auto const gesturePtr =
      findControllerIf<Gtk::GestureClick>(widget,
                                          [optPhase](Gtk::GestureClick const& gesture)
                                          { return !optPhase || gesture.get_propagation_phase() == *optPhase; });

    if (!gesturePtr)
    {
      return false;
    }

    ::g_signal_emit_by_name(gesturePtr->gobj(), "pressed", nPress, x, y);
    return true;
  }

  /**
   * emitGestureReleased - Emits "released" on the first matching GestureClick controller.
   */
  inline bool emitGestureReleased(Gtk::Widget& widget,
                                  int const nPress = 1,
                                  double const x = 1.0,
                                  double const y = 1.0,
                                  std::optional<Gtk::PropagationPhase> const optPhase = std::nullopt)
  {
    auto const gesturePtr =
      findControllerIf<Gtk::GestureClick>(widget,
                                          [optPhase](Gtk::GestureClick const& gesture)
                                          { return !optPhase || gesture.get_propagation_phase() == *optPhase; });

    if (!gesturePtr)
    {
      return false;
    }

    ::g_signal_emit_by_name(gesturePtr->gobj(), "released", nPress, x, y);
    return true;
  }

  /**
   * walkWidgets - Visits root and every descendant in pre-order.
   */
  void walkWidgets(Gtk::Widget& root, auto const& visit)
  {
    visit(root);

    for (auto* child = root.get_first_child(); child != nullptr; child = child->get_next_sibling())
    {
      walkWidgets(*child, visit);
    }
  }

  /**
   * findWidget - Returns the first descendant (including root) of type T in pre-order, or nullptr.
   */
  template<typename T>
  T* findWidget(Gtk::Widget& root)
  {
    T* result = nullptr;

    walkWidgets(root,
                [&result](Gtk::Widget& widget)
                {
                  if (result == nullptr)
                  {
                    result = dynamic_cast<T*>(&widget);
                  }
                });

    return result;
  }

  /**
   * findWidgetByClass - Returns the first descendant of type T carrying the CSS class, or nullptr.
   */
  template<typename T>
  T* findWidgetByClass(Gtk::Widget& root, std::string_view cssClass)
  {
    T* result = nullptr;

    walkWidgets(root,
                [&result, cssClass](Gtk::Widget& widget)
                {
                  if (result == nullptr && hasCssClass(widget, cssClass))
                  {
                    result = dynamic_cast<T*>(&widget);
                  }
                });

    return result;
  }

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

              if (msg.find(
                    "New application windows must be added after the GApplication::startup signal has been emitted") !=
                  std::string_view::npos)
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

    if (auto gioAppPtr = Gio::Application::get_default(); gioAppPtr)
    {
      auto gtkAppPtr = std::dynamic_pointer_cast<Gtk::Application>(gioAppPtr);

      if (gtkAppPtr)
      {
        return gtkAppPtr;
      }
    }

    return Gtk::Application::create("io.github.aobus.test", Gio::Application::Flags::NON_UNIQUE);
  }

  /**
   * drainGtkEvents - Drains the GTK event loop.
   */
  inline void drainGtkEvents()
  {
    auto contextPtr = Glib::MainContext::get_default();

    while (contextPtr->pending())
    {
      contextPtr->iteration(false);
    }
  }

  /**
   * GtkWindowFixture - Owns GTK application/window plumbing for widget tests.
   */
  class GtkWindowFixture final
  {
  public:
    GtkWindowFixture()
      : _appPtr{ensureGtkApplication()}
    {
    }

    GtkWindowFixture(GtkWindowFixture const&) = delete;
    GtkWindowFixture& operator=(GtkWindowFixture const&) = delete;
    GtkWindowFixture(GtkWindowFixture&&) = delete;
    GtkWindowFixture& operator=(GtkWindowFixture&&) = delete;

    ~GtkWindowFixture()
    {
      if (_mounted)
      {
        _window.unset_child();
        drainGtkEvents();
      }
    }

    Gtk::Window& window() { return _window; }

    void mount(Gtk::Widget& widget)
    {
      _window.set_child(widget);
      _mounted = true;
    }

    void present()
    {
      _window.present();
      drain();
    }

    void unmount()
    {
      if (_mounted)
      {
        _window.unset_child();
        _mounted = false;
      }

      drain();
    }

    void drain() { drainGtkEvents(); }

  private:
    Glib::RefPtr<Gtk::Application> _appPtr;
    Gtk::Window _window;
    bool _mounted = false;
  };

  /**
   * GtkRuntimeFixture - RAII fixture for AppRuntime with temporary storage.
   */
  class GtkRuntimeFixture
  {
  public:
    GtkRuntimeFixture()
    {
      auto const musicRoot = _tempDir.path() / "music";
      auto const databasePath = _tempDir.path() / "db";
      auto const configPath = _tempDir.path() / "config.yaml";

      std::filesystem::create_directories(musicRoot);
      std::filesystem::create_directories(databasePath);

      auto configStorePtr = std::make_unique<rt::ConfigStore>(configPath);

      _runtimePtr = std::make_unique<rt::AppRuntime>(rt::AppRuntimeDependencies{
        .executorPtr = std::make_unique<GtkMainContextExecutor>(),
        .musicRoot = musicRoot,
        .databasePath = databasePath,
        .workspaceConfigStorePtr = std::move(configStorePtr),
      });
    }

    rt::AppRuntime& runtime() { return *_runtimePtr; }
    ao::test::TempDir& tempDir() { return _tempDir; }

  private:
    ao::test::TempDir _tempDir;
    std::unique_ptr<rt::AppRuntime> _runtimePtr;
  };

  /**
   * @brief Creates an AppRuntime backed by a temporary directory with a GtkMainContextExecutor.
   */
  inline auto makeRuntime(ao::test::TempDir const& tempDir)
  {
    return rt::AppRuntime{rt::AppRuntimeDependencies{
      .executorPtr = std::make_unique<GtkMainContextExecutor>(),
      .musicRoot = tempDir.path(),
      .databasePath = tempDir.path() / ".aobus" / "library",
      .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(tempDir.path() / "config.yaml"),
    }};
  }

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

  /**
   * WidgetMeasure - Holds layout sizes for widget measurements.
   */
  struct WidgetMeasure final
  {
    std::int32_t minimum = 0;
    std::int32_t natural = 0;
    std::int32_t minimumBaseline = -1;
    std::int32_t naturalBaseline = -1;
  };

  /**
   * measureWidget - Helper to measure widget sizes.
   */
  inline WidgetMeasure measureWidget(Gtk::Widget& widget, Gtk::Orientation orientation, std::int32_t forSize = -1)
  {
    auto result = WidgetMeasure{};
    widget.measure(
      orientation, forSize, result.minimum, result.natural, result.minimumBaseline, result.naturalBaseline);
    return result;
  }

  /**
   * drainGtkEventsFor - Iterates default MainContext for a duration.
   */
  inline void drainGtkEventsFor(std::chrono::milliseconds duration)
  {
    auto const deadline = std::chrono::steady_clock::now() + duration;
    auto contextPtr = Glib::MainContext::get_default();

    while (std::chrono::steady_clock::now() < deadline)
    {
      while (contextPtr->pending())
      {
        contextPtr->iteration(false);
      }

      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    while (contextPtr->pending())
    {
      contextPtr->iteration(false);
    }
  }

  template<typename Predicate>
  bool pumpGtkEventsUntil(Predicate const& predicate,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds{500})
  {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    auto contextPtr = Glib::MainContext::get_default();

    while (std::chrono::steady_clock::now() < deadline)
    {
      while (contextPtr->pending())
      {
        contextPtr->iteration(false);
      }

      if (predicate())
      {
        return true;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    while (contextPtr->pending())
    {
      contextPtr->iteration(false);
    }

    return predicate();
  }
} // namespace ao::gtk::test
