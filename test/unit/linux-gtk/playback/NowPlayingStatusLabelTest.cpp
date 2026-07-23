// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingStatusLabel.h"

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/label.h>

#include <optional>

namespace ao::gtk::test
{
  TEST_CASE("NowPlayingStatusLabel - binds status text and reveals the playing track", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto& playback = fixture.runtime().playback();
    rt::test::addReadyAudioProvider(fixture.runtime());
    drainGtkEvents();

    auto statusLabel = NowPlayingStatusLabel{playback};
    auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&statusLabel.widget());
    REQUIRE(gtkLabel);
    CHECK(gtkLabel->get_text().empty());
    CHECK(gtkLabel->has_css_class("ao-nowplaying"));
    CHECK(gtkLabel->has_css_class("ao-clickable"));
    CHECK(gtkLabel->get_tooltip_text() == "Click to show playing list");

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = addRuntimeTrack(fixture.runtime(), {.title = "Song", .artist = "Artist", .uri = fixturePath});
    fixture.runtime().reloadAllTracks();
    auto const view = fixture.runtime().views().createView({.listId = rt::kAllTracksListId});
    REQUIRE(view);
    REQUIRE(playback.commands().startFromView(*view, trackId));
    REQUIRE(waitForPlaybackSettlement(fixture.runtime(), trackId));
    drainGtkEvents();
    CHECK_FALSE(gtkLabel->get_text().empty());

    auto optRequest = std::optional<rt::PlaybackRevealTrackRequest>{};
    auto sub = playback.events().onRevealTrackRequested([&](auto const& ev) { optRequest = ev; });

    REQUIRE(emitGesturePressed(*gtkLabel));
    drainGtkEvents();

    REQUIRE(optRequest);
    CHECK(optRequest->trackId == trackId);
    CHECK(optRequest->preferredListId == rt::kAllTracksListId);
  }
} // namespace ao::gtk::test
