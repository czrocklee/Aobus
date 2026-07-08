// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../GtkTestSupport.h"
#include "playback/SeekControl.h"
#include "playback/TimeLabel.h"
#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/adjustment.h>  // NOLINT(misc-include-cleaner)
#include <gtkmm/application.h> // NOLINT(misc-include-cleaner)
#include <gtkmm/label.h>
#include <gtkmm/scale.h>

#include <chrono>
#include <cstdint>

namespace ao::gtk::test
{
  namespace
  {
    struct TestEnvironment final
    {
      ao::test::TempDir tempDir{};
      rt::AppRuntime runtime;

      TestEnvironment()
        : runtime{makeRuntime(tempDir)}
      {
      }
    };

    void startPlayback(rt::PlaybackService& playback, library::MusicLibrary& library)
    {
      auto const trackId = library::test::addTrack(
        library, library::test::TrackSpec{.title = "Tick Test", .duration = std::chrono::seconds{5}});

      auto const request = rt::PlaybackService::PlaybackRequest{
        .item = rt::NowPlayingInfo{.trackId = trackId, .title = "Tick Test", .artist = "Artist"},
        .input = audio::PlaybackInput{.filePath = audio::test::requireAudioFixture("basic_metadata.flac"),
                                      .duration = std::chrono::seconds{5}},
      };

      REQUIRE(playback.play(request, kInvalidListId));
      drainGtkEvents();
    }
  } // namespace

  TEST_CASE("PlaybackUiComponents - render initial GTK bindings", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = TestEnvironment{};
    auto& playback = env.runtime.playback();
    rt::test::addReadyAudioProvider(playback);
    drainGtkEvents();

    SECTION("SeekControl renders a disabled seek scale before playback starts")
    {
      auto seekControl = SeekControl{playback};

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
      CHECK_FALSE(timeLabel.isTickActiveForTest());

      startPlayback(playback, env.runtime.musicLibrary());
      CHECK_FALSE(timeLabel.isTickActiveForTest());

      auto windowFixture = GtkWindowFixture{};
      windowFixture.mount(timeLabel.widget());
      windowFixture.present();
      CHECK(timeLabel.isTickActiveForTest());

      playback.pause();
      drainGtkEvents();
      CHECK_FALSE(timeLabel.isTickActiveForTest());
    }

    SECTION("SeekControl tick follows mapped playing state")
    {
      auto seekControl = SeekControl{playback};
      CHECK_FALSE(seekControl.isTickActiveForTest());

      startPlayback(playback, env.runtime.musicLibrary());
      CHECK_FALSE(seekControl.isTickActiveForTest());

      auto windowFixture = GtkWindowFixture{};
      windowFixture.mount(seekControl.widget());
      windowFixture.present();
      CHECK(seekControl.isTickActiveForTest());

      playback.pause();
      drainGtkEvents();
      CHECK_FALSE(seekControl.isTickActiveForTest());
    }
  }
} // namespace ao::gtk::test
