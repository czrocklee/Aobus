// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingFieldLabel.h"

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/Type.h>
#include <ao/audio/Types.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace ao::gtk::test
{
  namespace
  {
    rt::PlaybackService::PlaybackRequest playbackRequest(TrackId trackId, std::string title, std::string artist = {})
    {
      return rt::PlaybackService::PlaybackRequest{
        .trackId = trackId,
        .input = audio::PlaybackInput{.duration = std::chrono::seconds{1}},
        .title = std::move(title),
        .artist = std::move(artist),
      };
    }
  } // namespace

  TEST_CASE("NowPlayingFieldLabel - renders now playing fields", "[gtk][playback][viewmodel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime.playback());
    drainGtkEvents();
    auto& playback = runtime.playback();

    SECTION("title label renders idle and playing title text")
    {
      auto titleLabel = NowPlayingFieldLabel{runtime, rt::TrackField::Title};
      auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&titleLabel.widget());
      REQUIRE(gtkLabel);

      CHECK(gtkLabel->get_text() == "Not Playing");
      CHECK(gtkLabel->has_css_class("ao-playback-title"));
      CHECK_FALSE(gtkLabel->has_css_class("ao-clickable"));
      CHECK_FALSE(hasController<Gtk::GestureClick>(*gtkLabel));

      auto desc = playbackRequest(TrackId{1}, "Test Song", "Test Artist");

      playback.play(desc, ListId{7});
      drainGtkEvents();

      CHECK(gtkLabel->get_text() == "Test Song");
    }

    SECTION("artist label renders artist text and css")
    {
      auto artistLabel = NowPlayingFieldLabel{runtime, rt::TrackField::Artist};
      auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&artistLabel.widget());
      REQUIRE(gtkLabel);

      auto desc = playbackRequest(TrackId{2}, "Another Song", "Known Artist");

      playback.play(desc, ListId{8});
      drainGtkEvents();

      CHECK(gtkLabel->get_text() == "Known Artist");
      CHECK(gtkLabel->has_css_class("ao-playback-artist"));
      CHECK_FALSE(gtkLabel->has_css_class("ao-playback-title"));
    }

    SECTION("unsupported field renders empty text without title or artist css")
    {
      auto yearLabel = NowPlayingFieldLabel{runtime, rt::TrackField::Year};
      auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&yearLabel.widget());
      REQUIRE(gtkLabel);

      auto desc = playbackRequest(TrackId{3}, "Dated Song");

      playback.play(desc, ListId{9});
      drainGtkEvents();

      CHECK(gtkLabel->get_text().empty());
      CHECK_FALSE(gtkLabel->has_css_class("ao-playback-title"));
      CHECK_FALSE(gtkLabel->has_css_class("ao-playback-artist"));
    }
  }

  TEST_CASE("NowPlayingFieldLabel - clickable actions route through runtime services", "[gtk][playback][viewmodel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime.playback());
    drainGtkEvents();

    SECTION("filter action navigates to the now playing field query")
    {
      auto titleLabel =
        NowPlayingFieldLabel{runtime, rt::TrackField::Title, uimodel::playback::NowPlayingFieldAction::FilterByField};
      auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&titleLabel.widget());
      REQUIRE(gtkLabel);

      runtime.playback().play(playbackRequest(TrackId{11}, "Filtered Song", "Filter Artist"), ListId{12});
      drainGtkEvents();

      CHECK(gtkLabel->has_css_class("ao-clickable"));
      REQUIRE(emitGesturePressed(*gtkLabel, 1, 2.0, 3.0, Gtk::PropagationPhase::BUBBLE));
      drainGtkEvents();

      auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
      CHECK(state.listId == rt::kAllTracksListId);
      CHECK(state.filterExpression == "$title = \"Filtered Song\"");
    }

    SECTION("reveal action emits a reveal request for the current track")
    {
      auto titleLabel =
        NowPlayingFieldLabel{runtime, rt::TrackField::Title, uimodel::playback::NowPlayingFieldAction::Reveal};
      auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&titleLabel.widget());
      REQUIRE(gtkLabel);

      auto optRequest = std::optional<rt::PlaybackService::RevealTrackRequested>{};
      auto sub = runtime.playback().onRevealTrackRequested([&](auto const& ev) { optRequest = ev; });

      runtime.playback().play(playbackRequest(TrackId{21}, "Reveal Song", "Reveal Artist"), ListId{22});
      drainGtkEvents();

      REQUIRE(emitGesturePressed(*gtkLabel, 1, 2.0, 3.0, Gtk::PropagationPhase::BUBBLE));
      drainGtkEvents();

      REQUIRE(optRequest);
      CHECK(optRequest->trackId == TrackId{21});
      CHECK(optRequest->preferredListId == ListId{22});
    }

    SECTION("play-pause action resumes when transport is not playing")
    {
      auto titleLabel =
        NowPlayingFieldLabel{runtime, rt::TrackField::Title, uimodel::playback::NowPlayingFieldAction::PlayPause};
      auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&titleLabel.widget());
      REQUIRE(gtkLabel);

      runtime.playback().play(playbackRequest(TrackId{31}, "Toggle Song", "Toggle Artist"), ListId{32});
      drainGtkEvents();

      bool started = false;
      auto startSub = runtime.playback().onStarted([&] { started = true; });
      REQUIRE(emitGesturePressed(*gtkLabel, 1, 2.0, 3.0, Gtk::PropagationPhase::BUBBLE));
      drainGtkEvents();
      CHECK(started);
    }
  }
} // namespace ao::gtk::test
