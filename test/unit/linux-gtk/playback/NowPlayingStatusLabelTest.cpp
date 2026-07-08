// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingStatusLabel.h"

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/audio/PlaybackInput.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/label.h>

#include <chrono>
#include <optional>

namespace ao::gtk::test
{
  TEST_CASE("NowPlayingStatusLabel - binds status text and reveals the playing track", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto& playback = fixture.runtime().playback();
    rt::test::addReadyAudioProvider(playback);
    drainGtkEvents();

    auto statusLabel = NowPlayingStatusLabel{playback};
    auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&statusLabel.widget());
    REQUIRE(gtkLabel);
    CHECK(gtkLabel->get_text().empty());
    CHECK(gtkLabel->has_css_class("ao-nowplaying"));
    CHECK(gtkLabel->has_css_class("ao-clickable"));
    CHECK(gtkLabel->get_tooltip_text() == "Click to show playing list");

    auto desc = rt::PlaybackService::PlaybackRequest{
      .item = rt::NowPlayingInfo{.trackId = TrackId{1}, .title = "Song", .artist = "Artist"},
      .input = audio::PlaybackInput{.duration = std::chrono::seconds{1}},
    };

    REQUIRE(playback.play(desc, ListId{1}));
    drainGtkEvents();
    CHECK_FALSE(gtkLabel->get_text().empty());

    auto optRequest = std::optional<rt::PlaybackService::RevealTrackRequested>{};
    auto sub = playback.onRevealTrackRequested([&](auto const& ev) { optRequest = ev; });

    REQUIRE(emitGesturePressed(*gtkLabel));
    drainGtkEvents();

    REQUIRE(optRequest);
    CHECK(optRequest->trackId == TrackId{1});
    CHECK(optRequest->preferredListId == ListId{1});
  }
} // namespace ao::gtk::test
