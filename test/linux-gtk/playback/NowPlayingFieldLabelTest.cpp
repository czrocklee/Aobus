// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingFieldLabel.h"

#include "layout/runtime/LayoutComponent.h"
#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/linux-gtk/GtkWidgetTestSupport.h"
#include "test/linux-gtk/layout/LayoutTestSupport.h"
#include "test/media/wav/TestWav.h"
#include "test/runtime/AppRuntimeTestSupport.h"
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
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    TrackId addPlayableTrack(rt::AppRuntime& runtime, std::string title, std::string artist = {})
    {
      constexpr std::uint32_t kSampleRate = 48000;
      constexpr std::uint16_t kChannels = 2;
      auto const media = ao::test::TempFile{
        ao::test::wav::makeWav({.channels = kChannels,
                                .sampleRate = kSampleRate,
                                .audioData = std::vector<std::uint8_t>(std::size_t{kSampleRate} * kChannels * 2U, 0)}),
        ".wav"};
      return addRuntimeTrack(
        runtime,
        library::test::TrackSpec{.title = std::move(title), .artist = std::move(artist), .uri = media.path.string()});
    }

    void startPlayback(rt::AppRuntime& runtime, TrackId const trackId)
    {
      runtime.sources().reloadAllTracks();
      auto const viewRes = runtime.workspace().navigate({.target = rt::kAllTracksListId});
      REQUIRE(viewRes);
      REQUIRE(runtime.playback().commands().startFromView(*viewRes, trackId));
      REQUIRE(tryWaitForPlaybackSettlement(runtime, trackId));
    }
  } // namespace

  TEST_CASE("NowPlayingFieldLabel - binds model field text and css")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime);
    drainGtkEvents();

    SECTION("title label binds idle and playing title text")
    {
      auto titleLabel = NowPlayingFieldLabel{
        runtime.playback(), runtime.workspace(), nullptr, ao::test::englishMessageCatalog(), rt::TrackField::Title};
      auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&titleLabel.widget());
      REQUIRE(gtkLabel);
      CHECK(gtkLabel->get_text() == "Not Playing");

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
      auto artistLabel = NowPlayingFieldLabel{
        runtime.playback(), runtime.workspace(), nullptr, ao::test::englishMessageCatalog(), rt::TrackField::Artist};
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
      auto yearLabel = NowPlayingFieldLabel{
        runtime.playback(), runtime.workspace(), nullptr, ao::test::englishMessageCatalog(), rt::TrackField::Year};
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

  TEST_CASE("NowPlayingFieldLabel - filter action routes the exact title query")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime);
    drainGtkEvents();

    auto titleLabel = NowPlayingFieldLabel{runtime.playback(),
                                           runtime.workspace(),
                                           nullptr,
                                           ao::test::englishMessageCatalog(),
                                           rt::TrackField::Title,
                                           uimodel::NowPlayingFieldAction::FilterByField};
    auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&titleLabel.widget());
    REQUIRE(gtkLabel);

    auto const trackId = addPlayableTrack(runtime, "Filtered Song", "Filter Artist");
    startPlayback(runtime, trackId);
    drainGtkEvents();

    CHECK(gtkLabel->has_css_class("ao-clickable"));
    REQUIRE(runtime.views().trackListState(runtime.workspace().snapshot().activeViewId).filterExpression.empty());
    REQUIRE(tryEmitGesturePressed(*gtkLabel, 1, 2.0, 3.0, Gtk::PropagationPhase::BUBBLE));
    drainGtkEvents();

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == rt::kAllTracksListId);
    CHECK(state.filterExpression == R"($title = "Filtered Song")");
  }

  TEST_CASE("NowPlayingFieldLabel - reveal action publishes the current track and list")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime);
    drainGtkEvents();

    auto titleLabel = NowPlayingFieldLabel{runtime.playback(),
                                           runtime.workspace(),
                                           nullptr,
                                           ao::test::englishMessageCatalog(),
                                           rt::TrackField::Title,
                                           uimodel::NowPlayingFieldAction::Reveal};
    auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&titleLabel.widget());
    REQUIRE(gtkLabel);

    auto optRequest = std::optional<rt::PlaybackRevealTrackRequest>{};
    auto sub = runtime.playback().events().onRevealTrackRequested([&](auto const& ev) noexcept { optRequest = ev; });

    auto const trackId = addPlayableTrack(runtime, "Reveal Song", "Reveal Artist");
    startPlayback(runtime, trackId);
    drainGtkEvents();

    REQUIRE(tryEmitGesturePressed(*gtkLabel, 1, 2.0, 3.0, Gtk::PropagationPhase::BUBBLE));
    drainGtkEvents();

    REQUIRE(optRequest);
    CHECK(optRequest->trackId == trackId);
    CHECK(optRequest->preferredListId == rt::kAllTracksListId);
  }

  TEST_CASE("NowPlayingFieldLabel - configured play-pause starts a selection or toggles the current subject")
  {
    auto fixture = layout::test::LayoutRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    auto const selectedId = addPlayableTrack(runtime, "Selected Song");
    runtime.sources().reloadAllTracks();
    auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    REQUIRE(runtime.views().setSelection(viewId, {selectedId}));
    auto const componentPtr = fixture.create(uimodel::LayoutNode{
      .type = "playback.currentTitleLabel", .props = {{"action", uimodel::LayoutValue{"playPause"}}}});
    REQUIRE(componentPtr);
    auto* const label = dynamic_cast<Gtk::Label*>(&componentPtr->widget());
    REQUIRE(label);
    auto const click = [&] { REQUIRE(tryEmitGesturePressed(*label, 1, 2.0, 3.0, Gtk::PropagationPhase::BUBBLE)); };

    SECTION("Idle starts the selected row without a current subject")
    {
      rt::test::addReadyAudioProvider(runtime);
      drainGtkEvents();
      REQUIRE(playback.snapshot().transport.ready);
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Idle);
      REQUIRE(playback.snapshot().transport.nowPlaying.trackId == kInvalidTrackId);
      CHECK(label->get_text() == "Not Playing");

      click();

      REQUIRE(tryWaitForPlaybackSettlement(runtime, selectedId));
      CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
      CHECK(label->get_text() == "Selected Song");
    }

    SECTION("Restored Idle resumes the current subject rather than starting the selection")
    {
      rt::test::addReadyAudioProvider(runtime);
      auto const currentId = addPlayableTrack(runtime, "Restored Song");
      startPlayback(runtime, currentId);
      REQUIRE(runtime.savePlaybackSession());
      playback.commands().stop();
      REQUIRE(ao::test::requireValue(runtime.restorePlaybackSession()).restored);
      REQUIRE(runtime.views().setSelection(viewId, {selectedId}));
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Idle);
      REQUIRE(playback.snapshot().transport.nowPlaying.trackId == currentId);
      CHECK(label->get_text() == "Restored Song");

      click();

      CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
      CHECK(playback.snapshot().transport.nowPlaying.trackId == currentId);
    }

    SECTION("Playing pauses and Paused resumes the current subject")
    {
      rt::test::addReadyAudioProvider(runtime);
      auto const currentId = addPlayableTrack(runtime, "Current Song");
      startPlayback(runtime, currentId);
      REQUIRE(runtime.views().setSelection(viewId, {selectedId}));
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Playing);

      click();

      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Paused);
      CHECK(label->get_text() == "Current Song");

      click();

      CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
      CHECK(playback.snapshot().transport.nowPlaying.trackId == currentId);
    }

    SECTION("Unready Idle does not start the selected row or hide the placeholder")
    {
      REQUIRE_FALSE(playback.snapshot().transport.ready);
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Idle);

      click();
      drainGtkEvents();

      CHECK(playback.snapshot().transport.transport == audio::Transport::Idle);
      CHECK(playback.snapshot().transport.nowPlaying.trackId == kInvalidTrackId);
      CHECK(label->get_visible());
      CHECK(label->get_text() == "Not Playing");
    }
  }
} // namespace ao::gtk::test
