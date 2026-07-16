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
#include <ao/audio/PlaybackInput.h>
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
      auto const trackId =
        addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Tick Test", .duration = std::chrono::seconds{5}});

      auto const request = rt::PlaybackService::PlaybackRequest{
        .item = rt::NowPlayingInfo{.trackId = trackId, .title = "Tick Test", .artist = "Artist"},
        .input = audio::PlaybackInput{.filePath = audio::test::requireAudioFixture("basic_metadata.flac"),
                                      .duration = std::chrono::seconds{5}},
      };

      REQUIRE(runtime.playback().play(request, kInvalidListId));
      drainGtkEvents();
    }
  } // namespace

  TEST_CASE("PlaybackUiComponents - render initial GTK bindings", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto env = PlaybackUiComponentsFixture{};
    auto& playback = env.runtime.playback();
    rt::test::addReadyAudioProvider(playback);
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

      playback.pause();
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

      playback.pause();
      drainGtkEvents();
      CHECK_FALSE(seekControl.isTickActive());
    }
  }
} // namespace ao::gtk::test
