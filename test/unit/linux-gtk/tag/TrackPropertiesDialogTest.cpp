// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TrackPropertiesDialog.h"

#include "../../TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/AudioCodec.h>
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/entry.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("TrackPropertiesDialog renders metadata fields for the selected tracks", "[gtk][unit][tag][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& library = runtime.musicLibrary();
    auto cache = TrackRowCache{runtime.library()};
    auto window = Gtk::Window{};

    auto trackId1 = TrackId{kInvalidTrackId};
    auto trackId2 = TrackId{kInvalidTrackId};
    {
      auto txn = library.writeTransaction();
      auto writer = library.tracks().writer(txn);

      auto builder1 = library::TrackBuilder::createNew();
      builder1.metadata()
        .title("Track 1")
        .artist("Artist 1")
        .album("Album 1")
        .albumArtist("AA")
        .genre("Rock")
        .year(2023)
        .trackNumber(1)
        .discNumber(1);
      builder1.property()
        .uri("/music/track1.flac")
        .duration(std::chrono::seconds{1})
        .sampleRate(SampleRate{44100})
        .bitrate(Bitrate{320})
        .channels(Channels{2})
        .bitDepth(BitDepth{16})
        .codec(AudioCodec::Flac);
      auto serializeResult1 = builder1.serialize(txn, library.dictionary(), library.resources());
      REQUIRE(serializeResult1);
      auto const [hot1, cold1] = *serializeResult1;
      trackId1 = ao::test::requireValue(writer.createHotCold(hot1, cold1)).first;

      auto builder2 = library::TrackBuilder::createNew();
      builder2.metadata()
        .title("Track 2")
        .artist("Artist 2")
        .album("Album 1")
        .albumArtist("AA")
        .genre("Rock")
        .year(2023)
        .trackNumber(2)
        .discNumber(1);
      builder2.property()
        .uri("/music/track2.flac")
        .duration(std::chrono::seconds{2})
        .sampleRate(SampleRate{48000})
        .bitrate(Bitrate{320})
        .channels(Channels{2})
        .bitDepth(BitDepth{24})
        .codec(AudioCodec::Flac);
      auto serializeResult2 = builder2.serialize(txn, library.dictionary(), library.resources());
      REQUIRE(serializeResult2);
      auto const [hot2, cold2] = *serializeResult2;
      trackId2 = ao::test::requireValue(writer.createHotCold(hot2, cold2)).first;

      REQUIRE(txn.commit());
    }

    REQUIRE(trackId1 != kInvalidTrackId);
    REQUIRE(trackId2 != kInvalidTrackId);

    SECTION("dialog creation and data loading")
    {
      auto dialog = TrackPropertiesDialog{
        window, runtime.library(), runtime.library().writer(), runtime.completion(), cache, {trackId1}};
      drainGtkEvents();

      auto const entries = collectAll<Gtk::Entry>(dialog);
      auto const hasTrackTitle =
        std::ranges::any_of(entries, [](Gtk::Entry const* entry) { return entry->get_text() == "Track 1"; });
      auto const hasTrackArtist =
        std::ranges::any_of(entries, [](Gtk::Entry const* entry) { return entry->get_text() == "Artist 1"; });
      CHECK(hasTrackTitle);
      CHECK(hasTrackArtist);
    }

    SECTION("multi-track selection marks differing fields as mixed")
    {
      auto dialog = TrackPropertiesDialog{
        window, runtime.library(), runtime.library().writer(), runtime.completion(), cache, {trackId1, trackId2}};
      drainGtkEvents();

      // Title and artist differ across the two tracks, so their editors surface the mixed-value
      // placeholder, while album/albumArtist/genre/year are shared and stay plain. The field-merge
      // logic itself belongs to the dialog's data path; here we assert the mixed marker reaches a
      // widget so the multi-track branch is observably exercised, not just constructed.
      auto const entries = collectAll<Gtk::Entry>(dialog);
      auto const mixedCount = std::ranges::count_if(
        entries, [](Gtk::Entry const* entry) { return entry->get_placeholder_text() == "<Multiple Values>"; });
      CHECK(mixedCount >= 1);
    }
  }
} // namespace ao::gtk::test
