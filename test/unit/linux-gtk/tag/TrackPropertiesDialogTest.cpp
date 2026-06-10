// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TrackPropertiesDialog.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/Type.h>
#include <ao/library/AudioCodec.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("TrackPropertiesDialog - lifecycle", "[gtk][tag][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& library = runtime.musicLibrary();
    auto cache = TrackRowCache{library};
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
        .durationMs(1000)
        .sampleRate(44100)
        .bitrate(320)
        .channels(2)
        .bitDepth(16)
        .codec(library::AudioCodec::Flac);
      auto const [hot1, cold1] = builder1.serialize(txn, library.dictionary(), library.resources());
      trackId1 = writer.createHotCold(hot1, cold1).first;

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
        .durationMs(2000)
        .sampleRate(48000)
        .bitrate(320)
        .channels(2)
        .bitDepth(24)
        .codec(library::AudioCodec::Flac);
      auto const [hot2, cold2] = builder2.serialize(txn, library.dictionary(), library.resources());
      trackId2 = writer.createHotCold(hot2, cold2).first;

      txn.commit();
    }

    REQUIRE(trackId1 != kInvalidTrackId);
    REQUIRE(trackId2 != kInvalidTrackId);

    SECTION("dialog creation and data loading")
    {
      auto dialog = TrackPropertiesDialog{window, library, runtime.mutation(), cache, {trackId1}};
      drainGtkEvents();

      auto width = 0;
      auto height = 0;
      dialog.get_default_size(width, height);

      CHECK(width == -1);
      CHECK(height == -1);

      auto scrolledWindows = std::vector<Gtk::ScrolledWindow*>{};
      auto* const child = dialog.get_child();
      REQUIRE(child != nullptr);
      collectScrolledWindows(*child, scrolledWindows);

      auto configuredScrollCount = 0;

      for (auto* const scrolledWindow : scrolledWindows)
      {
        if (scrolledWindow->get_min_content_width() == 480)
        {
          ++configuredScrollCount;
          CHECK(scrolledWindow->get_propagate_natural_width());
          CHECK(scrolledWindow->get_propagate_natural_height());
          CHECK(scrolledWindow->get_max_content_width() == 640);
          CHECK(scrolledWindow->get_max_content_height() == 520);
        }
      }

      CHECK(configuredScrollCount == 2);
    }

    SECTION("multi-track selection")
    {
      auto const dialog = TrackPropertiesDialog{window, library, runtime.mutation(), cache, {trackId1, trackId2}};
      drainGtkEvents();
    }
  }
} // namespace ao::gtk::test
