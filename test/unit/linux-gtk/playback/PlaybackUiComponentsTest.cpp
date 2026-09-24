// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SeekControlWidget.h"
#include "playback/TimeLabel.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <gdk/gdk.h>
#include <gdk/x11/gdkx.h>
#include <glib-object.h>
#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <graphene.h>
#include <gtk/gtk.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>
#include <gtkmm/window.h>
#include <sigc++/scoped_connection.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    void startPlayback(rt::AppRuntime& runtime)
    {
      auto const trackId = addRuntimeTrack(
        runtime,
        library::test::TrackSpec{.title = "Tick Test",
                                 .artist = "Artist",
                                 .uri = audio::test::requireAudioFixture("basic_metadata.flac").string(),
                                 .duration = std::chrono::seconds{5}});
      runtime.sources().reloadAllTracks();
      auto const viewRes = runtime.workspace().navigate({.target = rt::kAllTracksListId});
      REQUIRE(viewRes);
      REQUIRE(runtime.playback().commands().startFromView(*viewRes, trackId));
      REQUIRE(tryWaitForPlaybackSettlement(runtime, trackId));
      drainGtkEvents();
    }

    void preparePausedLoopingPlayback(rt::AppRuntime& runtime)
    {
      rt::test::addReadyAudioProvider(runtime);
      drainGtkEvents();
      startPlayback(runtime);
      runtime.playback().commands().setRepeatMode(rt::RepeatMode::All);
      runtime.playback().commands().pause();
    }

    void checkFrameAlignedElapsed(std::chrono::milliseconds const actualElapsed,
                                  std::chrono::milliseconds const requestedElapsed)
    {
      // The decoder's frame-aligned position is truncated to whole milliseconds.
      CHECK(actualElapsed <= requestedElapsed);
      CHECK(actualElapsed >= requestedElapsed - std::chrono::milliseconds{1});
    }

    void requireOwnedGtkDisplay()
    {
      auto const* const marker = std::getenv("AOBUS_OWNED_GTK_DISPLAY");

      if (marker == nullptr || std::string_view{marker} != "1")
      {
        SKIP("native GTK input requires the portal-owned Xvfb display");
      }
    }

    class NativeScaleMouseFixture final
    {
    public:
      explicit NativeScaleMouseFixture(Gtk::Scale& scale, std::uint32_t const button = 1U)
        : _scale{scale}, _button{button}
      {
        _scale.set_size_request(640, 70);
        _scale.set_hexpand(true);
        _ancestor.append(_scale);
        _window.window().set_default_size(720, 100);
        _window.mount(_ancestor);
        _window.present();

        auto* const gdkDisplay = ::gtk_widget_get_display(GTK_WIDGET(_scale.gobj()));
        REQUIRE(GDK_IS_X11_DISPLAY(gdkDisplay));
        // This fixture deliberately targets the portal's isolated Xvfb backend.
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        _display = ::gdk_x11_display_get_xdisplay(gdkDisplay);
        G_GNUC_END_IGNORE_DEPRECATIONS
        REQUIRE(_display != nullptr);

        int eventBase = 0;
        int errorBase = 0;
        int majorVersion = 0;
        int minorVersion = 0;
        REQUIRE(::XTestQueryExtension(_display, &eventBase, &errorBase, &majorVersion, &minorVersion) != False);

        _observer = ::gtk_event_controller_legacy_new();
        ::gtk_event_controller_set_propagation_phase(_observer, GTK_PHASE_CAPTURE);
        ::g_signal_connect_data(
          _observer, "event", G_CALLBACK(&NativeScaleMouseFixture::observeEvent), this, nullptr, G_CONNECT_DEFAULT);
        ::gtk_widget_add_controller(GTK_WIDGET(_scale.gobj()), _observer);
      }

      ~NativeScaleMouseFixture()
      {
        if (_buttonDown && _display != nullptr)
        {
          std::ignore = ::XTestFakeButtonEvent(_display, _button, False, CurrentTime);
          std::ignore = ::XSync(_display, False);
        }

        if (_observer != nullptr)
        {
          ::g_signal_handlers_disconnect_matched(_observer, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
          ::gtk_widget_remove_controller(GTK_WIDGET(_scale.gobj()), _observer);
        }

        _window.unmount();
        _ancestor.remove(_scale);
      }

      NativeScaleMouseFixture(NativeScaleMouseFixture const&) = delete;
      NativeScaleMouseFixture& operator=(NativeScaleMouseFixture const&) = delete;
      NativeScaleMouseFixture(NativeScaleMouseFixture&&) = delete;
      NativeScaleMouseFixture& operator=(NativeScaleMouseFixture&&) = delete;

      void pressThumb()
      {
        REQUIRE_FALSE(_buttonDown);
        auto const [x, y] = thumbPoint();
        movePointer(x, y);
        auto const deliveredBefore = _pressCount;
        REQUIRE(::XTestFakeButtonEvent(_display, _button, True, CurrentTime) != False);
        std::ignore = ::XSync(_display, False);
        _buttonDown = true;
        REQUIRE(tryPumpGtkEventsUntil([&] { return _pressCount > deliveredBefore; }));
      }

      std::chrono::milliseconds moveHeldToPosition(double const fraction)
      {
        REQUIRE(_buttonDown);
        auto const previousValue = _scale.get_value();
        auto const deliveredBefore = _motionCount;
        auto const width = ::gtk_widget_get_width(GTK_WIDGET(_scale.gobj()));
        auto const height = ::gtk_widget_get_height(GTK_WIDGET(_scale.gobj()));
        REQUIRE(width > 4);
        REQUIRE(height > 0);
        auto const x = std::clamp(fraction * static_cast<double>(width), 2.0, static_cast<double>(width - 2));
        movePointer(x, static_cast<double>(height) / 2.0);
        REQUIRE(
          tryPumpGtkEventsUntil([&] { return _motionCount > deliveredBefore && _scale.get_value() != previousValue; }));
        return scaleElapsed();
      }

      void release() { release(true); }

      void releaseOutsideWidget() { release(false); }

      void unmap() { _window.unmount(); }

      void remap()
      {
        _window.mount(_ancestor);
        _window.present();
      }

      void setAncestorSensitive(bool const sensitive)
      {
        _ancestor.set_sensitive(sensitive);
        drainGtkEvents();
      }

    private:
      static gboolean observeEvent([[maybe_unused]] GtkEventController* controller, GdkEvent* event, gpointer data)
      {
        switch (auto& fixture = *static_cast<NativeScaleMouseFixture*>(data); ::gdk_event_get_event_type(event))
        {
          case GDK_BUTTON_PRESS: ++fixture._pressCount; break;
          case GDK_MOTION_NOTIFY: ++fixture._motionCount; break;
          case GDK_BUTTON_RELEASE: ++fixture._releaseCount; break;
          default: break;
        }

        return FALSE;
      }

      std::pair<double, double> thumbPoint() const
      {
        int sliderStart = 0;
        int sliderEnd = 0;
        ::gtk_range_get_slider_range(GTK_RANGE(_scale.gobj()), &sliderStart, &sliderEnd);
        REQUIRE(sliderEnd > sliderStart);
        auto const height = ::gtk_widget_get_height(GTK_WIDGET(_scale.gobj()));
        REQUIRE(height > 0);
        return {static_cast<double>(sliderStart + sliderEnd) / 2.0, static_cast<double>(height) / 2.0};
      }

      std::pair<std::int32_t, std::int32_t> rootPoint(double const widgetX, double const widgetY) const
      {
        auto* const native = ::gtk_widget_get_native(GTK_WIDGET(_scale.gobj()));
        REQUIRE(native != nullptr);
        auto const widgetPoint = graphene_point_t{static_cast<float>(widgetX), static_cast<float>(widgetY)};
        auto nativePoint = graphene_point_t{};
        REQUIRE(::gtk_widget_compute_point(GTK_WIDGET(_scale.gobj()), GTK_WIDGET(native), &widgetPoint, &nativePoint) !=
                FALSE);
        auto* const surface = ::gtk_native_get_surface(native);
        REQUIRE(surface != nullptr);
        REQUIRE(GDK_IS_X11_SURFACE(surface));
        // GTK deprecates X11 access, but native Xvfb input requires its public ID.
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        auto const surfaceId = ::gdk_x11_surface_get_xid(surface);
        G_GNUC_END_IGNORE_DEPRECATIONS
        double surfaceX = 0.0;
        double surfaceY = 0.0;
        ::gtk_native_get_surface_transform(native, &surfaceX, &surfaceY);

        int rootX = 0;
        int rootY = 0;
        ::Window child = None;
        REQUIRE(::XTranslateCoordinates(_display,
                                        surfaceId,
                                        DefaultRootWindow(_display),
                                        static_cast<std::int32_t>(std::lround(nativePoint.x - surfaceX)),
                                        static_cast<std::int32_t>(std::lround(nativePoint.y - surfaceY)),
                                        &rootX,
                                        &rootY,
                                        &child) != False);
        return {rootX, rootY};
      }

      void movePointer(double const widgetX, double const widgetY)
      {
        auto const [rootX, rootY] = rootPoint(widgetX, widgetY);
        REQUIRE(::XTestFakeMotionEvent(_display, DefaultScreen(_display), rootX, rootY, CurrentTime) != False);
        std::ignore = ::XSync(_display, False);
      }

      void release(bool const expectDelivery)
      {
        REQUIRE(_buttonDown);
        auto const deliveredBefore = _releaseCount;
        REQUIRE(::XTestFakeButtonEvent(_display, _button, False, CurrentTime) != False);
        std::ignore = ::XSync(_display, False);
        _buttonDown = false;

        if (expectDelivery)
        {
          REQUIRE(tryPumpGtkEventsUntil([&] { return _releaseCount > deliveredBefore; }));
        }
        else
        {
          drainGtkEventsFor(std::chrono::milliseconds{20});
          CHECK(_releaseCount == deliveredBefore);
        }
      }

      std::chrono::milliseconds scaleElapsed() const
      {
        return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(std::llround(_scale.get_value()))};
      }

      Gtk::Scale& _scale;
      std::uint32_t _button;
      Gtk::Box _ancestor{Gtk::Orientation::HORIZONTAL};
      GtkWindowFixture _window;
      ::Display* _display = nullptr;
      GtkEventController* _observer = nullptr;
      std::int32_t _pressCount = 0;
      std::int32_t _motionCount = 0;
      std::int32_t _releaseCount = 0;
      bool _buttonDown = false;
    };
  } // namespace

  TEST_CASE("SeekControlWidget - idle state has a disabled zeroed styled scale", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();

    auto seekControl = SeekControlWidget{playback};

    auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
    REQUIRE(scale != nullptr);
    CHECK(scale->has_css_class("ao-seekbar"));
    CHECK(scale->get_value() == 0.0);
    CHECK_FALSE(scale->get_sensitive());
  }

  TEST_CASE("TimeLabel - idle state has template text and style", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();

    auto timeLabel = TimeLabel{playback, TimeLabel::Mode::Combined};

    auto* const label = dynamic_cast<Gtk::Label*>(&timeLabel.widget());
    REQUIRE(label != nullptr);
    CHECK(label->has_css_class("ao-time-label"));

    CHECK(label->get_text() == uimodel::describeTimeTemplate(uimodel::PlaybackTimeMode::Combined));

    std::int32_t widthRequest = 0;
    std::int32_t heightRequest = 0;
    label->get_size_request(widthRequest, heightRequest);
    CHECK(widthRequest > 0);
  }

  TEST_CASE("TimeLabel - ticks only while mapped and playing", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();

    auto timeLabel = TimeLabel{playback, TimeLabel::Mode::Combined};
    CHECK_FALSE(timeLabel.isTickActive());

    startPlayback(env.runtime());
    CHECK_FALSE(timeLabel.isTickActive());

    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(timeLabel.widget());
    windowFixture.present();
    CHECK(timeLabel.isTickActive());

    windowFixture.unmount();
    CHECK_FALSE(timeLabel.isTickActive());
    windowFixture.mount(timeLabel.widget());
    windowFixture.present();
    CHECK(timeLabel.isTickActive());

    playback.commands().pause();
    drainGtkEvents();
    CHECK_FALSE(timeLabel.isTickActive());
  }

  TEST_CASE("SeekControlWidget - ticks only while mapped and playing", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();

    auto seekControl = SeekControlWidget{playback};
    CHECK_FALSE(seekControl.isTickActive());

    startPlayback(env.runtime());
    CHECK_FALSE(seekControl.isTickActive());

    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(seekControl.widget());
    windowFixture.present();
    CHECK(seekControl.isTickActive());

    windowFixture.unmount();
    CHECK_FALSE(seekControl.isTickActive());
    windowFixture.mount(seekControl.widget());
    windowFixture.present();
    CHECK(seekControl.isTickActive());

    playback.commands().pause();
    drainGtkEvents();
    CHECK_FALSE(seekControl.isTickActive());
  }

  // Construction delivers the current playback state synchronously through the
  // view model. The constructor body must not overwrite it with template/reset
  // values, or a layout rebuilt during playback stays blank until the next
  // transport change.
  TEST_CASE("TimeLabel - construction during playback keeps live state", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();

    startPlayback(env.runtime());

    auto const transport = playback.snapshot().transport;
    REQUIRE(transport.duration > std::chrono::milliseconds{0});

    auto timeLabel = TimeLabel{playback, TimeLabel::Mode::Combined};

    auto* const label = dynamic_cast<Gtk::Label*>(&timeLabel.widget());
    REQUIRE(label != nullptr);
    CHECK(label->get_text() ==
          uimodel::formatPlaybackTime(uimodel::PlaybackTimeMode::Combined, transport.elapsed, transport.duration));

    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(timeLabel.widget());
    windowFixture.present();
    CHECK(timeLabel.isTickActive());
  }

  TEST_CASE("SeekControlWidget - construction during playback keeps live state", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();

    startPlayback(env.runtime());

    auto const transport = playback.snapshot().transport;
    REQUIRE(transport.duration > std::chrono::milliseconds{0});

    auto seekControl = SeekControlWidget{playback};

    auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
    REQUIRE(scale != nullptr);
    Glib::RefPtr<Gtk::Adjustment> const adjustmentPtr = scale->get_adjustment();
    CHECK(adjustmentPtr->get_upper() == static_cast<double>(transport.duration.count()));
    CHECK(scale->get_sensitive());

    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(seekControl.widget());
    windowFixture.present();
    CHECK(seekControl.isTickActive());
  }

  TEST_CASE("SeekControlWidget - debounce commits the captured value instead of the live scale",
            "[gtk][integration][playback][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();
    startPlayback(env.runtime());
    playback.commands().pause();
    auto seekControl = SeekControlWidget{playback};
    auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
    REQUIRE(scale != nullptr);

    auto const duration = playback.snapshot().transport.duration;
    auto const requestedElapsed = duration / 4;
    auto const laterElapsed = duration / 2;
    REQUIRE(requestedElapsed > std::chrono::milliseconds{0});

    scale->set_value(static_cast<double>(requestedElapsed.count()));
    playback.commands().seek(laterElapsed);
    REQUIRE(scale->get_value() == static_cast<double>(requestedElapsed.count()));

    auto const competingRevision = playback.snapshot().transport.finalSeekRevision;
    REQUIRE(
      tryPumpGtkEventsUntil([&] { return playback.snapshot().transport.finalSeekRevision != competingRevision; }));
    CHECK(playback.snapshot().transport.elapsed == requestedElapsed);
  }

  TEST_CASE("SeekControlWidget - destruction retires a pending final seek", "[gtk][integration][playback][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();
    startPlayback(env.runtime());
    playback.commands().pause();
    auto const before = playback.snapshot().transport;
    auto const requestedElapsed = before.duration / 4;
    REQUIRE(requestedElapsed > std::chrono::milliseconds{0});

    {
      auto seekControl = SeekControlWidget{playback};
      auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
      REQUIRE(scale != nullptr);
      scale->set_value(static_cast<double>(requestedElapsed.count()));
      REQUIRE(scale->get_value() == static_cast<double>(requestedElapsed.count()));
      REQUIRE(playback.snapshot().transport.finalSeekRevision == before.finalSeekRevision);
    }

    // A later timeout proves the main context dispatches beyond the debounce window.
    bool controlExpired = false;
    auto controlConnection = sigc::scoped_connection{Glib::signal_timeout().connect(
      [&]
      {
        controlExpired = true;
        return false;
      },
      75)};
    REQUIRE(tryPumpGtkEventsUntil([&] { return controlExpired; }));
    CHECK(playback.snapshot().transport.finalSeekRevision == before.finalSeekRevision);
    CHECK(playback.snapshot().transport.elapsed == before.elapsed);
  }

  TEST_CASE("SeekControlWidget - pending final keeps the playing thumb stable without a frame tick",
            "[gtk][unit][playback][async]")
  {
    requireOwnedGtkDisplay();
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();
    startPlayback(env.runtime());
    auto seekControl = SeekControlWidget{playback};
    auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
    REQUIRE(scale != nullptr);
    auto mouse = NativeScaleMouseFixture{*scale};
    REQUIRE(seekControl.isTickActive());

    auto const before = playback.snapshot().transport;
    REQUIRE(before.duration > std::chrono::milliseconds{0});
    mouse.pressThumb();
    auto const pendingElapsed = mouse.moveHeldToPosition(0.68);
    mouse.release();
    auto const pendingScaleValue = scale->get_value();

    CHECK_FALSE(seekControl.isTickActive());

    auto const competingElapsed = before.duration / 5;
    REQUIRE(competingElapsed != pendingElapsed);
    playback.commands().seek(competingElapsed);
    auto const competing = playback.snapshot().transport;
    REQUIRE(competing.occurrenceId == before.occurrenceId);
    REQUIRE(competing.positionRevision != before.positionRevision);
    CHECK(scale->get_value() == pendingScaleValue);
    CHECK_FALSE(seekControl.isTickActive());
  }

  TEST_CASE("SeekControlWidget - replacing a pending seek leaves a fresh native gesture usable",
            "[gtk][integration][playback][async]")
  {
    requireOwnedGtkDisplay();
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    preparePausedLoopingPlayback(env.runtime());
    auto seekControl = SeekControlWidget{playback};
    auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
    REQUIRE(scale != nullptr);
    auto mouse = NativeScaleMouseFixture{*scale};

    auto const first = playback.snapshot().transport;
    scale->set_value(static_cast<double>((first.duration / 4).count()));

    // Next synchronously replays this one-track sequence. The new occurrence
    // is published in the same turn, before the pending debounce is dispatched.
    playback.commands().next();
    playback.commands().pause();
    auto const replacement = playback.snapshot().transport;
    REQUIRE(replacement.occurrenceId != first.occurrenceId);
    REQUIRE(replacement.nowPlaying.trackId == first.nowPlaying.trackId);

    auto previews = std::vector<std::chrono::milliseconds>{};
    auto const previewSub =
      playback.events().onSeekPreview([&](std::chrono::milliseconds const elapsed) { previews.push_back(elapsed); });
    mouse.pressThumb();
    auto const arbitrationElapsed = mouse.moveHeldToPosition(0.43);
    CHECK(previews == std::vector<std::chrono::milliseconds>{arbitrationElapsed});
    auto const targetElapsed = mouse.moveHeldToPosition(0.67);
    CHECK(previews == std::vector<std::chrono::milliseconds>{arbitrationElapsed, targetElapsed});
    mouse.release();
    REQUIRE(tryPumpGtkEventsUntil(
      [&] { return playback.snapshot().transport.finalSeekRevision != replacement.finalSeekRevision; }));
    drainGtkEventsFor(std::chrono::milliseconds{75});
    CHECK(playback.snapshot().transport.finalSeekRevision.value == replacement.finalSeekRevision.value + 1);
    CHECK(playback.snapshot().transport.occurrenceId == replacement.occurrenceId);
    checkFrameAlignedElapsed(playback.snapshot().transport.elapsed, targetElapsed);
  }

  TEST_CASE("SeekControlWidget - native range arbitration does not retarget a held seek",
            "[gtk][integration][playback][async]")
  {
    requireOwnedGtkDisplay();
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    preparePausedLoopingPlayback(env.runtime());
    auto seekControl = SeekControlWidget{playback};
    auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
    REQUIRE(scale != nullptr);
    auto const button = GENERATE(1U, 2U, 3U);
    CAPTURE(button);
    auto mouse = NativeScaleMouseFixture{*scale, button};

    auto previews = std::vector<std::chrono::milliseconds>{};
    auto const previewSub =
      playback.events().onSeekPreview([&](std::chrono::milliseconds const elapsed) { previews.push_back(elapsed); });
    auto const first = playback.snapshot().transport;
    mouse.pressThumb();
    auto const firstTargetElapsed = mouse.moveHeldToPosition(0.28);
    CHECK(previews == std::vector<std::chrono::milliseconds>{firstTargetElapsed});

    playback.commands().next();
    playback.commands().pause();
    auto const replacement = playback.snapshot().transport;
    REQUIRE(replacement.occurrenceId != first.occurrenceId);
    REQUIRE(replacement.nowPlaying.trackId == first.nowPlaying.trackId);
    std::ignore = mouse.moveHeldToPosition(0.72);
    mouse.release();
    drainGtkEventsFor(std::chrono::milliseconds{75});
    CHECK(playback.snapshot().transport.finalSeekRevision == replacement.finalSeekRevision);
    CHECK(playback.snapshot().transport.elapsed == replacement.elapsed);
    CHECK(scale->get_value() == static_cast<double>(replacement.elapsed.count()));

    mouse.pressThumb();
    auto const targetElapsed = mouse.moveHeldToPosition(0.41);
    CHECK(previews == std::vector<std::chrono::milliseconds>{firstTargetElapsed, targetElapsed});
    mouse.release();
    REQUIRE(tryPumpGtkEventsUntil(
      [&] { return playback.snapshot().transport.finalSeekRevision != replacement.finalSeekRevision; }));
    drainGtkEventsFor(std::chrono::milliseconds{75});
    CHECK(playback.snapshot().transport.finalSeekRevision.value == replacement.finalSeekRevision.value + 1);
    CHECK(playback.snapshot().transport.occurrenceId == replacement.occurrenceId);
    checkFrameAlignedElapsed(playback.snapshot().transport.elapsed, targetElapsed);
  }

  TEST_CASE("SeekControlWidget - native interruption permits a fresh gesture without release",
            "[gtk][integration][playback][async]")
  {
    requireOwnedGtkDisplay();
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    preparePausedLoopingPlayback(env.runtime());
    auto seekControl = SeekControlWidget{playback};
    auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
    REQUIRE(scale != nullptr);
    auto mouse = NativeScaleMouseFixture{*scale};

    auto previews = std::vector<std::chrono::milliseconds>{};
    auto const previewSub =
      playback.events().onSeekPreview([&](std::chrono::milliseconds const elapsed) { previews.push_back(elapsed); });
    auto const first = playback.snapshot().transport;
    mouse.pressThumb();
    auto const firstTargetElapsed = mouse.moveHeldToPosition(0.27);
    CHECK(previews == std::vector<std::chrono::milliseconds>{firstTargetElapsed});
    playback.commands().next();
    playback.commands().pause();
    auto const replacement = playback.snapshot().transport;
    REQUIRE(replacement.occurrenceId != first.occurrenceId);

    SECTION("unmapping the mounted scale terminates its native gesture")
    {
      mouse.unmap();
      REQUIRE_FALSE(::gtk_widget_get_mapped(GTK_WIDGET(scale->gobj())));
      mouse.releaseOutsideWidget();
      mouse.remap();
      REQUIRE(::gtk_widget_get_mapped(GTK_WIDGET(scale->gobj())) != FALSE);
    }

    SECTION("effective ancestor insensitivity terminates the native gesture")
    {
      mouse.setAncestorSensitive(false);
      REQUIRE(scale->get_sensitive());
      REQUIRE_FALSE(::gtk_widget_is_sensitive(GTK_WIDGET(scale->gobj())));
      mouse.releaseOutsideWidget();
      mouse.setAncestorSensitive(true);
      REQUIRE(::gtk_widget_is_sensitive(GTK_WIDGET(scale->gobj())) != FALSE);
    }

    drainGtkEventsFor(std::chrono::milliseconds{75});
    CHECK(playback.snapshot().transport.finalSeekRevision == replacement.finalSeekRevision);
    CHECK(playback.snapshot().transport.elapsed == replacement.elapsed);
    CHECK(scale->get_value() == static_cast<double>(replacement.elapsed.count()));

    mouse.pressThumb();
    auto const targetElapsed = mouse.moveHeldToPosition(0.63);
    CHECK(previews == std::vector<std::chrono::milliseconds>{firstTargetElapsed, targetElapsed});
    mouse.release();
    REQUIRE(tryPumpGtkEventsUntil(
      [&] { return playback.snapshot().transport.finalSeekRevision != replacement.finalSeekRevision; }));
    drainGtkEventsFor(std::chrono::milliseconds{75});
    CHECK(playback.snapshot().transport.finalSeekRevision.value == replacement.finalSeekRevision.value + 1);
    CHECK(playback.snapshot().transport.occurrenceId == replacement.occurrenceId);
    checkFrameAlignedElapsed(playback.snapshot().transport.elapsed, targetElapsed);
  }
} // namespace ao::gtk::test
