// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TrackPropertiesDialog.h"

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/rt/library/Library.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("TrackPropertiesDialog - renders metadata fields for the selected tracks", "[gtk][unit][tag][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId1 = kInvalidTrackId;
    auto trackId2 = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                                     {
                                       trackId1 = library::test::addTrack(musicLibrary,
                                                                          {.title = "Track 1",
                                                                           .artist = "Artist 1",
                                                                           .album = "Album 1",
                                                                           .albumArtist = "AA",
                                                                           .genre = "Rock",
                                                                           .uri = "music/track1.flac",
                                                                           .year = 2023,
                                                                           .discNumber = 1,
                                                                           .trackNumber = 1,
                                                                           .duration = std::chrono::seconds{1},
                                                                           .bitrate = Bitrate{320},
                                                                           .sampleRate = SampleRate{44100},
                                                                           .channels = Channels{2},
                                                                           .bitDepth = BitDepth{16},
                                                                           .codec = AudioCodec::Flac});
                                       trackId2 = library::test::addTrack(musicLibrary,
                                                                          {.title = "Track 2",
                                                                           .artist = "Artist 2",
                                                                           .album = "Album 1",
                                                                           .albumArtist = "AA",
                                                                           .genre = "Rock",
                                                                           .uri = "music/track2.flac",
                                                                           .year = 2023,
                                                                           .discNumber = 1,
                                                                           .trackNumber = 2,
                                                                           .duration = std::chrono::seconds{2},
                                                                           .bitrate = Bitrate{320},
                                                                           .sampleRate = SampleRate{48000},
                                                                           .channels = Channels{2},
                                                                           .bitDepth = BitDepth{24},
                                                                           .codec = AudioCodec::Flac});
                                     }};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library()};
    auto window = Gtk::Window{};

    REQUIRE(trackId1 != kInvalidTrackId);
    REQUIRE(trackId2 != kInvalidTrackId);

    SECTION("dialog creation and data loading")
    {
      auto dialog = TrackPropertiesDialog{window, runtime.library(), runtime.completion(), cache, {trackId1}};
      drainGtkEvents();

      auto const entries = collectAll<Gtk::Entry>(dialog);
      CHECK_FALSE(entries.empty());

      auto* const saveButton = findButtonByLabel(dialog, "Save");
      REQUIRE(saveButton != nullptr);
      CHECK_FALSE(saveButton->get_sensitive());

      auto const titleEntryIter =
        std::ranges::find_if(entries, [](Gtk::Entry const* entry) { return entry->get_text().raw() == "Track 1"; });
      REQUIRE(titleEntryIter != entries.end());

      (*titleEntryIter)->set_text("Renamed Track");
      drainGtkEvents();
      CHECK(saveButton->get_sensitive());
    }

    SECTION("multi-track selection marks differing fields as mixed")
    {
      auto dialog = TrackPropertiesDialog{window, runtime.library(), runtime.completion(), cache, {trackId1, trackId2}};
      drainGtkEvents();

      // Title and artist differ across the two tracks. UIModel owns the mixed-state decision; this
      // adapter test only asserts that the dialog reflects that row view in GTK widgets.
      auto const entries = collectAll<Gtk::Entry>(dialog);
      auto const mixedCount =
        std::ranges::count_if(entries,
                              [](Gtk::Entry const* entry)
                              { return entry->get_placeholder_text().raw() == uimodel::kMultipleTrackValuesText; });
      CHECK(mixedCount >= 1);
    }

    SECTION("missing authoring targets show why editing is unavailable")
    {
      auto dialog = TrackPropertiesDialog{window, runtime.library(), runtime.completion(), cache, {TrackId{999999}}};

      auto const labels = collectAll<Gtk::Label>(dialog);
      auto const errorLabelIter = std::ranges::find_if(
        labels, [](Gtk::Label const* label) { return label->has_css_class("ao-properties-session-error"); });
      REQUIRE(errorLabelIter != labels.end());
      CHECK((*errorLabelIter)->get_visible());
      CHECK((*errorLabelIter)->get_text().raw().contains("Track authoring target not found"));

      auto* const saveButton = findButtonByLabel(dialog, "Save");
      REQUIRE(saveButton != nullptr);
      CHECK_FALSE(saveButton->get_sensitive());
    }
  }
} // namespace ao::gtk::test
