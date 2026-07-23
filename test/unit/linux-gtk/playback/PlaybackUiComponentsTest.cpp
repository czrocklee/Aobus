// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../GtkTestSupport.h"
#include "playback/SeekControlWidget.h"
#include "playback/TimeLabel.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/playback/PlaybackService.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>

#include <chrono>
#include <cstdint>

namespace ao::gtk::test
{
  namespace
  {
    struct PlaybackUiComponentsFixture final
    {
      ao::test::TempDir tempDir{};
      rt::AppRuntime runtime;

      PlaybackUiComponentsFixture()
        : runtime{makeRuntime(tempDir)}
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
      auto const view = runtime.views().createView({.listId = rt::kAllTracksListId});
      REQUIRE(view);
      REQUIRE(runtime.playback().commands().startFromView(*view, trackId));
      REQUIRE(waitForPlaybackSettlement(runtime, trackId));
      drainGtkEvents();
    }
  } // namespace

  TEST_CASE("PlaybackUiComponents - render initial GTK bindings", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = PlaybackUiComponentsFixture{};
    auto& playback = env.runtime.playback();
    rt::test::addReadyAudioProvider(env.runtime);
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
      auto timeLabel = TimeLabel{playback, TimeLabel::Mode::Default};

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
      auto timeLabel = TimeLabel{playback, TimeLabel::Mode::Default};
      CHECK_FALSE(timeLabel.isTickActive());

      startPlayback(env.runtime);
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

      startPlayback(env.runtime);
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
} // namespace ao::gtk::test
