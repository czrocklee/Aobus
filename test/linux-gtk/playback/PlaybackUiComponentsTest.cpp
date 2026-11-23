// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SeekControlWidget.h"
#include "playback/TimeLabel.h"
#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkNativeInputTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/linux-gtk/GtkWidgetTestSupport.h"
#include "test/media/wav/TestWav.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <gtk/gtk.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>
#include <gtkmm/window.h>
#include <sigc++/scoped_connection.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Include Xlib after gtkmm to avoid macro collisions.
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <gdk/x11/gdkx.h>

namespace ao::gtk::test
{
  namespace
  {
    void startPlayback(rt::AppRuntime& runtime, TrackId const trackId)
    {
      runtime.sources().reloadAllTracks();
      auto const viewRes = runtime.workspace().navigate({.target = rt::kAllTracksListId});
      REQUIRE(viewRes);
      REQUIRE(runtime.playback().commands().startFromView(*viewRes, trackId));
      REQUIRE(tryWaitForPlaybackSettlement(runtime, trackId));
      drainGtkEvents();
    }

    void startPlayback(rt::AppRuntime& runtime)
    {
      auto const trackId = addRuntimeTrack(
        runtime,
        library::test::TrackSpec{.title = "Tick Test",
                                 .artist = "Artist",
                                 .uri = audio::test::requireAudioFixture("basic_metadata.flac").string(),
                                 .duration = std::chrono::seconds{5}});
      startPlayback(runtime, trackId);
    }

    void preparePausedLoopingPlayback(rt::AppRuntime& runtime)
    {
      rt::test::addReadyAudioProvider(runtime);
      drainGtkEvents();
      startPlayback(runtime);
      runtime.playback().commands().setRepeatMode(rt::RepeatMode::All);
      runtime.playback().commands().pause();
    }

    std::vector<std::uint8_t> seekPcmWave(std::size_t const seconds)
    {
      constexpr std::uint32_t kSampleRate = 48000;
      return ao::test::wav::makeWav(
        {.sampleRate = kSampleRate,
         .audioData = std::vector<std::uint8_t>(std::size_t{kSampleRate} * seconds * 2U, 0)});
    }

    void checkFrameAlignedElapsed(std::chrono::milliseconds const actualElapsed,
                                  std::chrono::milliseconds const requestedElapsed)
    {
      // The decoder's frame-aligned position is truncated to whole milliseconds.
      CHECK(actualElapsed <= requestedElapsed);
      CHECK(actualElapsed >= requestedElapsed - std::chrono::milliseconds{1});
    }

    ::Display* focusScale(Gtk::Scale& scale)
    {
      auto* const widget = GTK_WIDGET(scale.gobj());
      auto* const native = ::gtk_widget_get_native(widget);
      REQUIRE(native != nullptr);
      auto* const surface = ::gtk_native_get_surface(native);
      REQUIRE(GDK_IS_X11_SURFACE(surface));
      // The portal-owned Xvfb has no window manager to focus the toplevel.
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      auto* const display = ::gdk_x11_display_get_xdisplay(::gtk_widget_get_display(widget));
      auto const window = ::gdk_x11_surface_get_xid(surface);
      G_GNUC_END_IGNORE_DEPRECATIONS
      REQUIRE(display != nullptr);
      std::ignore = ::XSetInputFocus(display, window, RevertToParent, CurrentTime);
      std::ignore = ::XSync(display, False);
      REQUIRE(scale.grab_focus());
      REQUIRE(tryPumpGtkEventsUntil([&] { return scale.has_focus(); }));
      return display;
    }

    bool tryTapScaleKey(::Display* display, ::KeySym const key)
    {
      auto const code = ::XKeysymToKeycode(display, key);

      if (code == 0)
      {
        return false;
      }

      bool const pressed = ::XTestFakeKeyEvent(display, code, True, CurrentTime) != False;
      bool const released = ::XTestFakeKeyEvent(display, code, False, CurrentTime) != False;
      std::ignore = ::XSync(display, False);
      return pressed && released;
    }

    // Scale-specific pointer choreography over the shared native input
    // fixture: it owns only the window host, the thumb targeting, and the
    // seek-scale arbitration observations. Coordinate translation, XTest
    // injection, delivery observation, and held-button teardown live in
    // GtkNativeInputTestSupport.
    class NativeScaleMouseFixture final
    {
    public:
      explicit NativeScaleMouseFixture(Gtk::Scale& scale,
                                       std::uint32_t const button = 1U,
                                       Gtk::Widget* const companion = nullptr)
        : _scale{scale}
      {
        _scale.set_size_request(640, 70);
        _scale.set_hexpand(true);
        _ancestor.append(_scale);

        if (companion != nullptr)
        {
          _ancestor.append(*companion);
        }

        _window.window().set_default_size(720, 100);
        _window.mount(_ancestor);
        _window.present();
        _optPointer.emplace(_scale, button);
      }

      ~NativeScaleMouseFixture()
      {
        _optPointer.reset();
        _window.unmount();
        _ancestor.remove(_scale);
      }

      NativeScaleMouseFixture(NativeScaleMouseFixture const&) = delete;
      NativeScaleMouseFixture& operator=(NativeScaleMouseFixture const&) = delete;
      NativeScaleMouseFixture(NativeScaleMouseFixture&&) = delete;
      NativeScaleMouseFixture& operator=(NativeScaleMouseFixture&&) = delete;

      void pressThumb()
      {
        auto const [x, y] = thumbPoint();
        _optPointer->movePointerTo(x, y);
        _optPointer->press();
      }

      void moveHeld(double const fraction)
      {
        REQUIRE(_optPointer->isButtonDown());
        auto const deliveredBefore = _optPointer->motionCount();
        auto const width = ::gtk_widget_get_width(GTK_WIDGET(_scale.gobj()));
        auto const height = ::gtk_widget_get_height(GTK_WIDGET(_scale.gobj()));
        REQUIRE(width > 4);
        REQUIRE(height > 0);
        auto const x = std::clamp(fraction * static_cast<double>(width), 2.0, static_cast<double>(width - 2));
        _optPointer->movePointerTo(x, static_cast<double>(height) / 2.0);
        REQUIRE(tryPumpGtkEventsUntil([&] { return _optPointer->motionCount() > deliveredBefore; }));
      }

      std::chrono::milliseconds moveHeldToPosition(double const fraction)
      {
        auto const previousValue = _scale.get_value();
        moveHeld(fraction);
        REQUIRE(tryPumpGtkEventsUntil([&] { return _scale.get_value() != previousValue; }));
        return scaleElapsed();
      }

      void release() { _optPointer->release(); }

      void releaseOutsideWidget()
      {
        auto const deliveredBefore = _optPointer->releaseCount();
        _optPointer->release(false);
        CHECK(_optPointer->releaseCount() == deliveredBefore);
      }

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

      std::chrono::milliseconds scaleElapsed() const
      {
        return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(std::llround(_scale.get_value()))};
      }

    private:
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

      Gtk::Scale& _scale;
      Gtk::Box _ancestor{Gtk::Orientation::HORIZONTAL};
      GtkWindowFixture _window;
      std::optional<GtkNativePointerFixture> _optPointer;
    };
  } // namespace

  TEST_CASE("SeekControlWidget - idle state has a disabled zeroed styled scale")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();

    auto seekControl = SeekControlWidget{playback, ao::test::englishMessageCatalog()};

    auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
    REQUIRE(scale != nullptr);
    CHECK(scale->has_css_class("ao-seekbar"));
    CHECK(scale->get_value() == 0.0);
    CHECK_FALSE(scale->get_sensitive());
  }

  TEST_CASE("SeekControlWidget - exposes an accessible label for the seek scale")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();

    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto seekControl = SeekControlWidget{playback, textCatalog};
    auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
    REQUIRE(scale != nullptr);

    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(seekControl.widget());
    windowFixture.present();
    windowFixture.drain();

    // Compare through the catalog id, not the English copy, so the oracle
    // follows the message the widget actually renders.
    auto const expectedLabel = std::string{i18n::requiredText(textCatalog, i18n::MessageId::GtkPlaybackPosition)};
    CHECK(scale->get_tooltip_text() == Glib::ustring{expectedLabel});
    CHECK(hasAccessibleLabel(*scale, expectedLabel));
  }

  TEST_CASE("TimeLabel - idle state has template text and style")
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

  TEST_CASE("TimeLabel - ticks only while mapped and playing")
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

  TEST_CASE("SeekControlWidget - ticks only while mapped and playing")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();

    auto seekControl = SeekControlWidget{playback, ao::test::englishMessageCatalog()};
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
  TEST_CASE("TimeLabel - construction during playback keeps live state")
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

  TEST_CASE("SeekControlWidget - construction during playback keeps live state")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();

    startPlayback(env.runtime());

    auto const transport = playback.snapshot().transport;
    REQUIRE(transport.duration > std::chrono::milliseconds{0});

    auto seekControl = SeekControlWidget{playback, ao::test::englishMessageCatalog()};

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

  TEST_CASE("SeekControlWidget - debounce commits the captured value instead of the live scale")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();
    startPlayback(env.runtime());
    playback.commands().pause();
    auto seekControl = SeekControlWidget{playback, ao::test::englishMessageCatalog()};
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

  TEST_CASE("SeekControlWidget - consecutive non-pointer changes settle only the latest target")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    preparePausedLoopingPlayback(env.runtime());
    playback.commands().seek(std::chrono::milliseconds{0});
    auto const before = playback.snapshot().transport;
    auto const firstPosition = before.duration / 4;
    auto const lastPosition = before.duration / 2;
    REQUIRE(firstPosition > before.elapsed);
    REQUIRE(lastPosition > firstPosition);
    auto seekControl = SeekControlWidget{playback, ao::test::englishMessageCatalog()};
    auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
    REQUIRE(scale != nullptr);
    bool previewed = false;
    auto const previewSub = playback.events().onSeekPreview([&](auto) { previewed = true; });

    // Both value changes are delivered in one turn, before GLib can dispatch a timer.
    scale->set_value(static_cast<double>(firstPosition.count()));
    scale->set_value(static_cast<double>(lastPosition.count()));
    REQUIRE(playback.snapshot().transport.finalSeekRevision == before.finalSeekRevision);

    SECTION("the same occurrence accepts only the last target")
    {
      REQUIRE(tryPumpGtkEventsUntil(
        [&] { return playback.snapshot().transport.finalSeekRevision != before.finalSeekRevision; }));
      drainGtkEventsFor(std::chrono::milliseconds{75});
      auto const after = playback.snapshot().transport;
      CHECK(after.finalSeekRevision.value == before.finalSeekRevision.value + 1);
      CHECK(after.occurrenceId == before.occurrenceId);
      checkFrameAlignedElapsed(after.elapsed, lastPosition);
    }

    SECTION("replaying the same track drops both targets before the next turn")
    {
      playback.commands().next();
      playback.commands().pause();
      auto const replacement = playback.snapshot().transport;
      REQUIRE(replacement.nowPlaying.trackId == before.nowPlaying.trackId);
      REQUIRE(replacement.occurrenceId != before.occurrenceId);
      REQUIRE(replacement.elapsed != lastPosition);
      CHECK(scale->get_value() == static_cast<double>(replacement.elapsed.count()));
      drainGtkEventsFor(std::chrono::milliseconds{75});
      CHECK(playback.snapshot().transport.finalSeekRevision == before.finalSeekRevision);
      CHECK(playback.snapshot().transport.elapsed == replacement.elapsed);
    }

    CHECK_FALSE(previewed);
  }

  TEST_CASE("SeekControlWidget - focused native arrows seek by five seconds and clamp")
  {
    requireOwnedGtkDisplay();
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& runtime = env.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(runtime);
    drainGtkEvents();

    auto const media = ao::test::TempFile{seekPcmWave(21), ".wav"};
    auto const trackId =
      addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Seek Keys", .uri = media.path.string()});
    startPlayback(runtime, trackId);
    playback.commands().pause();
    playback.commands().seek(std::chrono::seconds{12});
    auto const before = playback.snapshot().transport;
    REQUIRE(before.duration == std::chrono::seconds{21});
    REQUIRE(before.elapsed == std::chrono::seconds{12});

    auto seekControl = SeekControlWidget{playback, ao::test::englishMessageCatalog()};
    auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
    REQUIRE(scale != nullptr);
    auto host = GtkWindowFixture{};
    host.mount(*scale);
    host.present();

    bool previewed = false;
    auto const previewSub = playback.events().onSeekPreview([&](auto) { previewed = true; });

    // Observe real delivery before the scale handles the press, independently of seeks.
    ::KeySym receivedKey = NoSymbol;
    auto keyObserverPtr = Gtk::EventControllerKey::create();
    keyObserverPtr->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    auto const keySub = sigc::scoped_connection{keyObserverPtr->signal_key_pressed().connect(
      [&](auto const keyval, auto, auto)
      {
        receivedKey = keyval;
        return false;
      },
      false)};
    host.window().add_controller(keyObserverPtr);

    bool rightIncreases = true;

    SECTION("left-to-right")
    {
      scale->set_direction(Gtk::TextDirection::LTR);
    }

    SECTION("right-to-left")
    {
      scale->set_direction(Gtk::TextDirection::RTL);
      rightIncreases = false;
    }

    SECTION("inversion cancels the right-to-left flip")
    {
      scale->set_direction(Gtk::TextDirection::RTL);
      scale->set_inverted(true);
    }

    auto* const display = focusScale(*scale);
    // Native events may run in separate main-context turns. Test each target;
    // the synchronous non-pointer test above owns the coalescing assertion.
    auto const tapAndSettle = [&](::KeySym const key, std::chrono::milliseconds const expectedElapsed)
    {
      CAPTURE(key, expectedElapsed.count());
      receivedKey = NoSymbol;
      auto const revision = playback.snapshot().transport.finalSeekRevision;
      REQUIRE(tryTapScaleKey(display, key));
      REQUIRE(tryPumpGtkEventsUntil([&] { return receivedKey != NoSymbol; }));
      REQUIRE(receivedKey == key);
      REQUIRE(scale->get_value() == static_cast<double>(expectedElapsed.count()));
      REQUIRE(tryPumpGtkEventsUntil([&] { return playback.snapshot().transport.finalSeekRevision != revision; }));
      auto const after = playback.snapshot().transport;
      CHECK(after.occurrenceId == before.occurrenceId);
      checkFrameAlignedElapsed(after.elapsed, expectedElapsed);
    };

    tapAndSettle(rightIncreases ? XK_Right : XK_Left, std::chrono::seconds{17});
    tapAndSettle(rightIncreases ? XK_KP_Left : XK_KP_Right, std::chrono::seconds{12});
    playback.commands().seek(std::chrono::seconds{2});
    tapAndSettle(rightIncreases ? XK_Left : XK_Right, std::chrono::seconds{0});
    playback.commands().seek(std::chrono::seconds{19});
    tapAndSettle(rightIncreases ? XK_KP_Right : XK_KP_Left, std::chrono::seconds{21});
    CHECK_FALSE(previewed);
  }

  TEST_CASE("SeekControlWidget - focused Right presses accumulate while the first Final is delayed")
  {
    requireOwnedGtkDisplay();
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& runtime = env.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(runtime);
    drainGtkEvents();

    auto const media = ao::test::TempFile{seekPcmWave(45), ".wav"};
    auto const trackId =
      addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Delayed Seek Keys", .uri = media.path.string()});
    startPlayback(runtime, trackId);

    auto seekControl = SeekControlWidget{playback, ao::test::englishMessageCatalog()};
    auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
    REQUIRE(scale != nullptr);
    scale->set_direction(Gtk::TextDirection::LTR);
    auto host = GtkWindowFixture{};
    host.mount(*scale);
    host.present();
    auto* const display = focusScale(*scale);

    playback.commands().seek(std::chrono::seconds{12});
    auto const before = playback.snapshot().transport;
    REQUIRE(before.transport == audio::Transport::Playing);
    REQUIRE(before.elapsed == std::chrono::seconds{12});
    REQUIRE(before.duration == std::chrono::seconds{45});

    auto keyBases = std::vector<double>{};
    auto keyObserverPtr = Gtk::EventControllerKey::create();
    keyObserverPtr->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    auto const keySub = sigc::scoped_connection{keyObserverPtr->signal_key_pressed().connect(
      [&](auto const keyval, auto, auto)
      {
        if (keyval == XK_Right && scale->has_focus())
        {
          keyBases.push_back(scale->get_value());
        }

        return false;
      },
      false)};
    host.window().add_controller(keyObserverPtr);

    constexpr auto kTimeout = std::chrono::seconds{2};
    constexpr float kGateVolume = 0.42F;
    auto landings = std::vector<std::chrono::milliseconds>{};
    auto lastRevision = before.finalSeekRevision;
    auto revisionWhileHeld = before.finalSeekRevision;
    bool entered = false;
    bool firstDelivered = false;
    bool sawFrameAfterDebounce = false;
    bool secondDelivered = false;
    auto const publicationSub = playback.events().onSnapshot(
      [&](auto const& snapshot)
      {
        auto const& state = snapshot.transport;

        if (state.finalSeekRevision != lastRevision)
        {
          lastRevision = state.finalSeekRevision;
          landings.push_back(state.elapsed);
        }

        if (entered || state.volume.level != kGateVolume)
        {
          return;
        }

        // Nested GTK dispatch lets the first seek reach the runtime, which
        // cannot commit it until this volume publication has returned.
        entered = true;
        firstDelivered =
          tryTapScaleKey(display, XK_Right) && tryPumpGtkEventsUntil([&] { return !keyBases.empty(); }, kTimeout);

        if (!firstDelivered)
        {
          return;
        }

        // Give the 50 ms debounce and then a frame a dispatch opportunity.
        // Observe our own tick, not whether the seek thumb snapped back.
        bool passedDebounce = false;
        auto const checkpoint = sigc::scoped_connection{Glib::signal_timeout().connect(
          [&]
          {
            passedDebounce = true;
            return false;
          },
          75)};
        auto const tickId = scale->add_tick_callback(
          [&](auto const&)
          {
            sawFrameAfterDebounce = passedDebounce;
            return true;
          });
        bool const reachedFrame = tryPumpGtkEventsUntil([&] { return sawFrameAfterDebounce; }, kTimeout);
        scale->remove_tick_callback(tickId);

        if (!reachedFrame)
        {
          return;
        }

        secondDelivered =
          tryTapScaleKey(display, XK_Right) && tryPumpGtkEventsUntil([&] { return keyBases.size() >= 2; }, kTimeout);
        revisionWhileHeld = playback.snapshot().transport.finalSeekRevision;
      });
    playback.commands().setVolume(kGateVolume);

    REQUIRE(entered);
    REQUIRE(firstDelivered);
    REQUIRE(sawFrameAfterDebounce);
    REQUIRE(secondDelivered);
    REQUIRE(revisionWhileHeld == before.finalSeekRevision);
    REQUIRE(tryPumpGtkEventsUntil([&] { return landings.size() >= 2; }, kTimeout));

    auto const after = playback.snapshot().transport;
    auto const firstPosition = std::chrono::milliseconds{std::llround(keyBases[0])} + std::chrono::seconds{5};
    CAPTURE(keyBases[0], keyBases[1], landings[0].count(), after.elapsed.count());
    CHECK(after.occurrenceId == before.occurrenceId);
    CHECK(std::llround(keyBases[1]) == firstPosition.count());
    checkFrameAlignedElapsed(landings[0], firstPosition);
    checkFrameAlignedElapsed(after.elapsed, firstPosition + std::chrono::seconds{5});
  }

  TEST_CASE("SeekControlWidget - destruction retires a pending final seek")
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
      auto const& textCatalog = ao::test::englishMessageCatalog();
      auto seekControl = SeekControlWidget{playback, textCatalog};
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

  TEST_CASE("SeekControlWidget - pending final keeps the playing thumb stable without a frame tick")
  {
    requireOwnedGtkDisplay();
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    rt::test::addReadyAudioProvider(env.runtime());
    drainGtkEvents();
    startPlayback(env.runtime());
    auto seekControl = SeekControlWidget{playback, ao::test::englishMessageCatalog()};
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

  TEST_CASE("SeekControlWidget - replacing a pending seek leaves a fresh native gesture usable")
  {
    requireOwnedGtkDisplay();
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    preparePausedLoopingPlayback(env.runtime());
    auto seekControl = SeekControlWidget{playback, ao::test::englishMessageCatalog()};
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

  TEST_CASE("SeekControlWidget - native range arbitration does not retarget a held seek")
  {
    requireOwnedGtkDisplay();
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& runtime = env.runtime();
    auto& playback = runtime.playback();
    enum class Replacement : std::uint8_t
    {
      Replay,
      PausedTrack,
      PlayingTrack,
    };
    auto const [button, kind] = GENERATE(Catch::Generators::table<std::uint32_t, Replacement>({
      {1U, Replacement::Replay},
      {2U, Replacement::Replay},
      {3U, Replacement::Replay},
      {1U, Replacement::PausedTrack},
      {1U, Replacement::PlayingTrack},
    }));
    CAPTURE(button, kind);

    rt::test::addReadyAudioProvider(runtime);
    drainGtkEvents();
    auto const firstMedia = ao::test::TempFile{seekPcmWave(12), ".wav"};
    auto const firstId =
      addRuntimeTrack(runtime, library::test::TrackSpec{.title = "First", .uri = firstMedia.path.string()});
    auto optReplacementMedia = std::optional<ao::test::TempFile>{};
    auto replacementId = firstId;

    if (kind != Replacement::Replay)
    {
      optReplacementMedia.emplace(seekPcmWave(16), ".wav");
      replacementId = addRuntimeTrack(
        runtime, library::test::TrackSpec{.title = "Replacement", .uri = optReplacementMedia->path.string()});
    }

    startPlayback(runtime, firstId);
    playback.commands().setRepeatMode(rt::RepeatMode::All);
    playback.commands().pause();
    auto seekControl = SeekControlWidget{playback, ao::test::englishMessageCatalog()};
    auto timeLabel = TimeLabel{playback, TimeLabel::Mode::Combined};
    auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
    auto* const label = dynamic_cast<Gtk::Label*>(&timeLabel.widget());
    REQUIRE(scale != nullptr);
    REQUIRE(label != nullptr);
    scale->set_direction(Gtk::TextDirection::LTR);
    auto mouse = NativeScaleMouseFixture{*scale, button, label};
    auto const timeText = [](std::chrono::milliseconds const elapsed, std::chrono::milliseconds const duration)
    { return uimodel::formatPlaybackTime(uimodel::PlaybackTimeMode::Combined, elapsed, duration); };

    auto previews = std::vector<std::chrono::milliseconds>{};
    auto const previewSub =
      playback.events().onSeekPreview([&](std::chrono::milliseconds const elapsed) { previews.push_back(elapsed); });
    auto const first = playback.snapshot().transport;
    mouse.pressThumb();
    auto const firstTargetElapsed = mouse.moveHeldToPosition(0.28);
    CHECK(previews == std::vector<std::chrono::milliseconds>{firstTargetElapsed});
    auto const firstPreview = timeText(firstTargetElapsed, first.duration);
    REQUIRE(label->get_text() == firstPreview);

    if (kind == Replacement::Replay)
    {
      playback.commands().next();
    }
    else
    {
      startPlayback(runtime, replacementId);
    }

    bool const playing = kind == Replacement::PlayingTrack;

    if (!playing)
    {
      playback.commands().pause();
    }

    auto replacement = playback.snapshot().transport;
    REQUIRE(replacement.occurrenceId != first.occurrenceId);
    REQUIRE(replacement.nowPlaying.trackId == replacementId);
    REQUIRE(replacement.transport == (playing ? audio::Transport::Playing : audio::Transport::Paused));
    REQUIRE(timeText(replacement.elapsed, replacement.duration) != firstPreview);
    auto const checkDisplay = [&]
    {
      CHECK(scale->get_value() == static_cast<double>(replacement.elapsed.count()));
      CHECK(label->get_text() == timeText(replacement.elapsed, replacement.duration));
    };

    if (playing)
    {
      // startPlayback drains GTK, so the shared window frame clock may already
      // have advanced the widgets beyond the replacement snapshot.
      CHECK(scale->get_adjustment()->get_upper() == static_cast<double>(replacement.duration.count()));
      CHECK(tryPumpGtkEventsUntil(
        [&] { return label->get_text() == timeText(mouse.scaleElapsed(), replacement.duration); }));
    }
    else
    {
      checkDisplay();
    }

    if (kind != Replacement::Replay)
    {
      // This position cannot be displayed by a thumb still clamped to the old duration.
      auto const targetPosition = first.duration + std::chrono::seconds{1};
      REQUIRE(targetPosition < replacement.duration);
      auto const newOccurrence = replacement.occurrenceId;
      REQUIRE(playback.commands().trySeek(newOccurrence, targetPosition));
      replacement = playback.snapshot().transport;
      REQUIRE(replacement.occurrenceId == newOccurrence);
      REQUIRE(replacement.elapsed > first.duration);
      checkDisplay();
    }

    if (playing)
    {
      // From this late anchor playback only moves forward, but the old drag tries
      // to move toward the start. Record every value so a tick cannot hide a jump.
      auto minThumbPosition = mouse.scaleElapsed();
      auto const thumbSub = sigc::scoped_connection{scale->signal_value_changed().connect(
        [&] { minThumbPosition = std::min(minThumbPosition, mouse.scaleElapsed()); })};
      mouse.moveHeld(0.05);

      // Both widgets interpolate from the seek on the same window frame clock.
      // Read them after dispatch, accepting any later displayed second.
      auto const anchorElapsed = std::chrono::floor<std::chrono::seconds>(replacement.elapsed);
      CHECK(tryPumpGtkEventsUntil(
        [&]
        {
          auto const thumbElapsed = mouse.scaleElapsed();
          return std::chrono::floor<std::chrono::seconds>(thumbElapsed) > anchorElapsed &&
                 label->get_text() == timeText(thumbElapsed, replacement.duration);
        }));
      CHECK(minThumbPosition == replacement.elapsed);
    }
    else
    {
      mouse.moveHeld(0.72);
      checkDisplay();
    }

    mouse.release();
    drainGtkEventsFor(std::chrono::milliseconds{75});
    auto const afterRelease = playback.snapshot().transport;
    CHECK(afterRelease.occurrenceId == replacement.occurrenceId);
    CHECK(afterRelease.positionRevision == replacement.positionRevision);
    CHECK(afterRelease.finalSeekRevision == replacement.finalSeekRevision);
    CHECK(previews == std::vector<std::chrono::milliseconds>{firstTargetElapsed});
    // Only the widgets interpolate; the silent test backend never advances.
    CHECK(afterRelease.elapsed == replacement.elapsed);
    CHECK(playback.elapsed() == replacement.elapsed);

    if (playing)
    {
      playback.commands().pause();
      drainGtkEvents();
    }

    REQUIRE(playback.snapshot().transport.transport == audio::Transport::Paused);
    checkDisplay();

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

  TEST_CASE("SeekControlWidget - native interruption permits a fresh gesture without release")
  {
    requireOwnedGtkDisplay();
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = GtkRuntimeFixture{};
    auto& playback = env.runtime().playback();
    preparePausedLoopingPlayback(env.runtime());
    auto seekControl = SeekControlWidget{playback, ao::test::englishMessageCatalog()};
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
    auto const afterInterruption = playback.snapshot().transport;
    CHECK(afterInterruption.occurrenceId == replacement.occurrenceId);
    CHECK(afterInterruption.positionRevision == replacement.positionRevision);
    CHECK(afterInterruption.finalSeekRevision == replacement.finalSeekRevision);
    CHECK(afterInterruption.elapsed == replacement.elapsed);
    CHECK(playback.elapsed() == replacement.elapsed);
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
