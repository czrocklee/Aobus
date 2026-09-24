// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/PlaybackDetailsWidget.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/audio/Quality.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>

#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("PlaybackDetailsWidget - renders idle stream status", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    REQUIRE_FALSE(fixture.runtime().playback().snapshot().transport.ready);
    auto widget = PlaybackDetailsWidget{fixture.runtime().playback(), ao::test::englishMessageCatalog()};

    auto* const root = dynamic_cast<Gtk::Box*>(&widget.widget());
    REQUIRE(root != nullptr);
    CHECK(root->has_css_class("ao-playback-details"));

    auto const labels = collectAll<Gtk::Label>(*root);
    REQUIRE(labels.size() == 1);
    CHECK(labels[0]->get_text() == "Connecting to audio engine...");
    CHECK(labels[0]->has_css_class("dim-label"));

    auto const icons = collectAll<Gtk::Image>(*root);
    REQUIRE(icons.size() == 1);
    CHECK(icons[0]->get_icon_name() == "media-record-symbolic");
    CHECK_FALSE(icons[0]->get_visible());
  }

  TEST_CASE("PlaybackDetailsWidget - preserves active quality when constructed during playback",
            "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = addRuntimeTrack(
      runtime, library::test::TrackSpec{.title = "Active details", .artist = "Artist", .uri = fixturePath});
    runtime.sources().reloadAllTracks();
    auto const viewRes = runtime.workspace().navigate({.target = rt::kAllTracksListId});
    REQUIRE(viewRes);
    REQUIRE(runtime.playback().commands().startFromView(*viewRes, trackId));
    REQUIRE(tryWaitForPlaybackSettlement(runtime, trackId));
    REQUIRE(tryPumpGtkEventsUntil(
      [&] { return runtime.playback().snapshot().transport.quality.overall != audio::Quality::Unknown; }));

    auto widget = PlaybackDetailsWidget{runtime.playback(), ao::test::englishMessageCatalog()};
    auto const icons = collectAll<Gtk::Image>(widget.widget());
    REQUIRE(icons.size() == 1);
    CHECK(icons[0]->get_icon_name() == "media-record-symbolic");
    CHECK(icons[0]->get_visible());
  }
} // namespace ao::gtk::test
