// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SeekControlWidget.h"
#include "playback/TimeLabel.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>

#include <catch2/catch_test_macros.hpp>
#include <glibmm/refptr.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>

#include <chrono>
#include <cstdint>
#include <memory>

namespace ao::gtk::test
{
  namespace
  {
    struct PlaybackUiComponentsFixture final
    {
      ao::test::TempDir tempDir{};
      std::unique_ptr<rt::AppRuntime> runtimePtr;

      PlaybackUiComponentsFixture()
        : runtimePtr{makeRuntime(tempDir)}
      {
      }
    };

    void startPlayback(rt::AppRuntime& runtime)
    {
      auto const trackId = addRuntimeTrack(
        runtime,
        library::test::TrackSpec{.title = "Tick Test",
                                 .artist = "Artist",
                                 .uri = audio::test::requireAudioFixture("basic_metadata.flac").string(),
                                 .duration = std::chrono::seconds{5}});
      runtime.reloadAllTracks();
      auto const viewRes = runtime.workspace().navigate({.target = rt::kAllTracksListId});
      REQUIRE(viewRes);
      REQUIRE(runtime.playback().commands().startFromView(*viewRes, trackId));
      REQUIRE(waitForPlaybackSettlement(runtime, trackId));
      drainGtkEvents();
    }
  } // namespace

  TEST_CASE("PlaybackUiComponents - render initial GTK bindings", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = PlaybackUiComponentsFixture{};
    auto& playback = env.runtimePtr->playback();
    rt::test::addReadyAudioProvider(*env.runtimePtr);
    drainGtkEvents();

    SECTION("SeekControlWidget renders a disabled seek scale before playback starts")
    {
      auto seekControl = SeekControlWidget{playback};

      auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
      REQUIRE(scale != nullptr);
      CHECK(scale->has_css_class("ao-seekbar"));
    }

    SECTION("TimeLabel renders the playback time template before playback starts")
    {
      auto timeLabel = TimeLabel{playback, TimeLabel::Mode::Combined};

      auto* const label = dynamic_cast<Gtk::Label*>(&timeLabel.widget());
      REQUIRE(label != nullptr);
      CHECK(label->has_css_class("ao-time-label"));

      std::int32_t widthRequest = 0;
      std::int32_t heightRequest = 0;
      label->get_size_request(widthRequest, heightRequest);
      CHECK(widthRequest > 0);
    }

    SECTION("TimeLabel tick follows mapped playing state")
    {
      auto timeLabel = TimeLabel{playback, TimeLabel::Mode::Combined};
      CHECK_FALSE(timeLabel.isTickActive());

      startPlayback(*env.runtimePtr);
      CHECK_FALSE(timeLabel.isTickActive());

      auto windowFixture = GtkWindowFixture{};
      windowFixture.mount(timeLabel.widget());
      windowFixture.present();
      CHECK(timeLabel.isTickActive());

      playback.commands().pause();
      drainGtkEvents();
      CHECK_FALSE(timeLabel.isTickActive());
    }

    SECTION("SeekControlWidget tick follows mapped playing state")
    {
      auto seekControl = SeekControlWidget{playback};
      CHECK_FALSE(seekControl.isTickActive());

      startPlayback(*env.runtimePtr);
      CHECK_FALSE(seekControl.isTickActive());

      auto windowFixture = GtkWindowFixture{};
      windowFixture.mount(seekControl.widget());
      windowFixture.present();
      CHECK(seekControl.isTickActive());

      playback.commands().pause();
      drainGtkEvents();
      CHECK_FALSE(seekControl.isTickActive());
    }
  }

  // Construction delivers the current playback state synchronously through the
  // view model. The constructor body must not overwrite it with template/reset
  // values, or a layout rebuilt during playback stays blank until the next
  // transport change.
  TEST_CASE("PlaybackUiComponents - construction during playback keeps live state", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = PlaybackUiComponentsFixture{};
    auto& playback = env.runtimePtr->playback();
    rt::test::addReadyAudioProvider(*env.runtimePtr);
    drainGtkEvents();

    startPlayback(*env.runtimePtr);

    auto const transport = playback.snapshot().transport;
    REQUIRE(transport.duration > std::chrono::milliseconds{0});

    SECTION("TimeLabel shows live time instead of the template")
    {
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

    SECTION("SeekControlWidget shows the live range instead of a zeroed scale")
    {
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
  }

  TEST_CASE("PlaybackUiComponents - construction without a track renders idle state", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = PlaybackUiComponentsFixture{};
    auto& playback = env.runtimePtr->playback();
    rt::test::addReadyAudioProvider(*env.runtimePtr);
    drainGtkEvents();

    SECTION("TimeLabel renders the template")
    {
      auto timeLabel = TimeLabel{playback, TimeLabel::Mode::Combined};

      auto* const label = dynamic_cast<Gtk::Label*>(&timeLabel.widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == uimodel::describeTimeTemplate(uimodel::PlaybackTimeMode::Combined));
    }

    SECTION("SeekControlWidget renders a disabled zeroed scale")
    {
      auto seekControl = SeekControlWidget{playback};

      auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
      REQUIRE(scale != nullptr);
      CHECK(scale->get_value() == 0.0);
      CHECK_FALSE(scale->get_sensitive());
    }
  }
} // namespace ao::gtk::test
