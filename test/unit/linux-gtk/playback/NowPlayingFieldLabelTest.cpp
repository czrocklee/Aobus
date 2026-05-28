// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingFieldLabel.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/audio/Types.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/label.h>

namespace ao::gtk::test
{
  TEST_CASE("NowPlayingFieldLabel - smoke test", "[gtk][playback][viewmodel]")
  {
    [[maybe_unused]] auto const app = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();

    auto titleLabel = NowPlayingFieldLabel{runtime, rt::TrackField::Title};
    auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&titleLabel.widget());
    REQUIRE(gtkLabel);

    // Just verify it wires up and doesn't crash
    auto desc = audio::TrackPlaybackDescriptor{.trackId = TrackId{1}, .title = "Test Song", .durationMs = 1000};

    playback.play(desc, ListId{1});
    drainGtkEvents();
  }
} // namespace ao::gtk::test
