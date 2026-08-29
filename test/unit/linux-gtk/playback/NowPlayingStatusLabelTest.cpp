// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingStatusLabel.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/source/TrackSourceCache.h>

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

    auto statusLabel = NowPlayingStatusLabel{playback, ao::test::englishMessageCatalog()};
    auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&statusLabel.widget());
    REQUIRE(gtkLabel);
    CHECK(gtkLabel->get_text().empty());
    CHECK(gtkLabel->has_css_class("ao-nowplaying"));
    CHECK(gtkLabel->has_css_class("ao-clickable"));
    CHECK(gtkLabel->get_tooltip_text() == "Click to show playing list");

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = addRuntimeTrack(
      fixture.runtime(), library::test::TrackSpec{.title = "Song", .artist = "Artist", .uri = fixturePath});
    fixture.runtime().sources().reloadAllTracks();
    auto const viewRes = fixture.runtime().workspace().navigate({.target = rt::kAllTracksListId});
    REQUIRE(viewRes);
    REQUIRE(playback.commands().startFromView(*viewRes, trackId));
    REQUIRE(waitForPlaybackSettlement(fixture.runtime(), trackId));
    drainGtkEvents();
    CHECK_FALSE(gtkLabel->get_text().empty());

    auto optRequest = std::optional<rt::PlaybackRevealTrackRequest>{};
    auto sub = playback.events().onRevealTrackRequested([&](auto const& ev) noexcept { optRequest = ev; });

    REQUIRE(emitGesturePressed(*gtkLabel));
    drainGtkEvents();

    REQUIRE(optRequest);
    CHECK(optRequest->trackId == trackId);
    CHECK(optRequest->preferredListId == rt::kAllTracksListId);
  }
} // namespace ao::gtk::test
