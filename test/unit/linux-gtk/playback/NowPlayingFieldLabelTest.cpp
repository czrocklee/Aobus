// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingFieldLabel.h"

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>

#include <optional>
#include <string>
#include <utility>

namespace ao::gtk::test
{
  namespace
  {
    TrackId addPlayableTrack(rt::AppRuntime& runtime, std::string title, std::string artist = {})
    {
      auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
      return addRuntimeTrack(runtime, {.title = std::move(title), .artist = std::move(artist), .uri = fixturePath});
    }

    void startPlayback(rt::AppRuntime& runtime, TrackId const trackId)
    {
      runtime.reloadAllTracks();
      auto const view = runtime.views().createView({.listId = rt::kAllTracksListId}, true);
      REQUIRE(view);
      REQUIRE(runtime.playback().commands().startFromView(view->viewId, trackId));
    }
  } // namespace

  TEST_CASE("NowPlayingFieldLabel - binds model field text and css", "[gtk][unit][playback][field-label]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime);
    drainGtkEvents();

    SECTION("title label binds idle and playing title text")
    {
      auto titleLabel = NowPlayingFieldLabel{runtime, rt::TrackField::Title};
      auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&titleLabel.widget());
      REQUIRE(gtkLabel);

      CHECK(gtkLabel->has_css_class("ao-playback-title"));
      CHECK_FALSE(gtkLabel->has_css_class("ao-clickable"));
      CHECK_FALSE(hasController<Gtk::GestureClick>(*gtkLabel));

      auto const trackId = addPlayableTrack(runtime, "Test Song", "Test Artist");
      startPlayback(runtime, trackId);
      drainGtkEvents();

      CHECK(gtkLabel->get_text() == "Test Song");
    }

    SECTION("artist label binds artist text and css")
    {
      auto artistLabel = NowPlayingFieldLabel{runtime, rt::TrackField::Artist};
      auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&artistLabel.widget());
      REQUIRE(gtkLabel);

      auto const trackId = addPlayableTrack(runtime, "Another Song", "Known Artist");
      startPlayback(runtime, trackId);
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

      auto const trackId = addPlayableTrack(runtime, "Dated Song");
      startPlayback(runtime, trackId);
      drainGtkEvents();

      CHECK(gtkLabel->get_text().empty());
      CHECK_FALSE(gtkLabel->has_css_class("ao-playback-title"));
      CHECK_FALSE(gtkLabel->has_css_class("ao-playback-artist"));
    }
  }

  TEST_CASE("NowPlayingFieldLabel - clickable actions route through runtime services",
            "[gtk][unit][playback][field-label]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime);
    drainGtkEvents();

    SECTION("filter action navigates to the now playing field query")
    {
      auto titleLabel =
        NowPlayingFieldLabel{runtime, rt::TrackField::Title, uimodel::NowPlayingFieldAction::FilterByField};
      auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&titleLabel.widget());
      REQUIRE(gtkLabel);

      auto const trackId = addPlayableTrack(runtime, "Filtered Song", "Filter Artist");
      startPlayback(runtime, trackId);
      drainGtkEvents();

      CHECK(gtkLabel->has_css_class("ao-clickable"));
      REQUIRE(emitGesturePressed(*gtkLabel, 1, 2.0, 3.0, Gtk::PropagationPhase::BUBBLE));
      drainGtkEvents();

      auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
      CHECK(state.listId == rt::kAllTracksListId);
      CHECK_FALSE(state.filterExpression.empty());
    }

    SECTION("reveal action emits a reveal request for the current track")
    {
      auto titleLabel = NowPlayingFieldLabel{runtime, rt::TrackField::Title, uimodel::NowPlayingFieldAction::Reveal};
      auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&titleLabel.widget());
      REQUIRE(gtkLabel);

      auto optRequest = std::optional<rt::PlaybackRevealTrackRequest>{};
      auto sub = runtime.playback().events().onRevealTrackRequested([&](auto const& ev) { optRequest = ev; });

      auto const trackId = addPlayableTrack(runtime, "Reveal Song", "Reveal Artist");
      startPlayback(runtime, trackId);
      drainGtkEvents();

      REQUIRE(emitGesturePressed(*gtkLabel, 1, 2.0, 3.0, Gtk::PropagationPhase::BUBBLE));
      drainGtkEvents();

      REQUIRE(optRequest);
      CHECK(optRequest->trackId == trackId);
      CHECK(optRequest->preferredListId == rt::kAllTracksListId);
    }

    SECTION("play-pause action resumes when transport is not playing")
    {
      auto titleLabel = NowPlayingFieldLabel{runtime, rt::TrackField::Title, uimodel::NowPlayingFieldAction::PlayPause};
      auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&titleLabel.widget());
      REQUIRE(gtkLabel);

      auto const trackId = addPlayableTrack(runtime, "Toggle Song", "Toggle Artist");
      startPlayback(runtime, trackId);
      drainGtkEvents();
      runtime.playback().commands().pause();
      drainGtkEvents();

      bool started = false;
      auto startSub = runtime.playback().events().onSnapshot(
        [&](rt::PlaybackSnapshot const& snapshot)
        { started = snapshot.transport.transport == audio::Transport::Playing; });
      REQUIRE(emitGesturePressed(*gtkLabel, 1, 2.0, 3.0, Gtk::PropagationPhase::BUBBLE));
      drainGtkEvents();
      CHECK(started);
    }
  }
} // namespace ao::gtk::test
