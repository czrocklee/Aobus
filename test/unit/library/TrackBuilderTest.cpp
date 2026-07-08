// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/library/TrackBuilder.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::library::test
{
  TEST_CASE("TrackBuilder - makeEmpty returns an empty builder", "[library][unit][track][builder]")
  {
    auto builder = TrackBuilder::makeEmpty();

    CHECK(builder.metadata().title().empty());
    CHECK(builder.metadata().artist().empty());
    CHECK(builder.metadata().album().empty());
    CHECK(builder.metadata().albumArtist().empty());
    CHECK(builder.metadata().composer().empty());
    CHECK(builder.metadata().conductor().empty());
    CHECK(builder.metadata().ensemble().empty());
    CHECK(builder.metadata().genre().empty());
    CHECK(builder.metadata().soloist().empty());
    CHECK(builder.property().uri().empty());
    CHECK(builder.property().bitDepth() == 0);
    CHECK(builder.property().duration() == std::chrono::milliseconds{0});
    CHECK(builder.tags().names().empty());
    CHECK(builder.coverArt().entries().empty());
    CHECK(builder.customMetadata().pairs().empty());
  }

  TEST_CASE("TrackBuilder - metadata builder fluent setters update fields", "[library][unit][track][builder]")
  {
    auto builder = TrackBuilder::makeEmpty();
    builder.metadata()
      .title("Test Title")
      .artist("Test Artist")
      .album("Test Album")
      .albumArtist("Test Album Artist")
      .composer("Test Composer")
      .genre("Rock")
      .work("Symphony No. 9")
      .movement("I. Allegro ma non troppo")
      .conductor("Carlos Kleiber")
      .ensemble("Vienna Philharmonic")
      .soloist("Yo-Yo Ma")
      .year(2024)
      .trackNumber(5)
      .trackTotal(10)
      .discNumber(2)
      .discTotal(3)
      .movementNumber(1)
      .movementTotal(4);

    CHECK(builder.metadata().title() == "Test Title");
    CHECK(builder.metadata().artist() == "Test Artist");
    CHECK(builder.metadata().album() == "Test Album");
    CHECK(builder.metadata().albumArtist() == "Test Album Artist");
    CHECK(builder.metadata().composer() == "Test Composer");
    CHECK(builder.metadata().genre() == "Rock");
    CHECK(builder.metadata().work() == "Symphony No. 9");
    CHECK(builder.metadata().movement() == "I. Allegro ma non troppo");
    CHECK(builder.metadata().conductor() == "Carlos Kleiber");
    CHECK(builder.metadata().ensemble() == "Vienna Philharmonic");
    CHECK(builder.metadata().soloist() == "Yo-Yo Ma");
    CHECK(builder.metadata().year() == 2024);
    CHECK(builder.metadata().trackNumber() == 5);
    CHECK(builder.metadata().trackTotal() == 10);
    CHECK(builder.metadata().discNumber() == 2);
    CHECK(builder.metadata().discTotal() == 3);
    CHECK(builder.metadata().movementNumber() == 1);
    CHECK(builder.metadata().movementTotal() == 4);
  }

  TEST_CASE("TrackBuilder - property builder fluent setters update fields", "[library][unit][track][builder]")
  {
    auto builder = TrackBuilder::makeEmpty();
    builder.property()
      .uri("file:///home/user/music/test.flac")
      .duration(std::chrono::minutes{3} + std::chrono::milliseconds{500})
      .bitrate(Bitrate{320000})
      .sampleRate(SampleRate{44100})
      .codec(AudioCodec::Alac)
      .channels(Channels{2})
      .bitDepth(BitDepth{16});

    CHECK(builder.property().uri() == "file:///home/user/music/test.flac");
    CHECK(builder.property().duration() == std::chrono::minutes{3} + std::chrono::milliseconds{500});
    CHECK(builder.property().bitrate() == 320000);
    CHECK(builder.property().sampleRate() == 44100);
    CHECK(builder.property().codec() == AudioCodec::Alac);
    CHECK(builder.property().channels() == 2);
    CHECK(builder.property().bitDepth() == 16);
  }

  TEST_CASE("TrackBuilder - custom metadata builder adds removes and clears pairs", "[library][unit][track][builder]")
  {
    auto builder = TrackBuilder::makeEmpty();

    builder.customMetadata().add("replaygain_track_gain_db", "-6.5");
    builder.customMetadata().add("isrc", "USSM19999999");

    CHECK(builder.customMetadata().pairs().size() == 2);

    builder.customMetadata().remove("isrc");
    CHECK(builder.customMetadata().pairs().size() == 1);
    CHECK(builder.customMetadata().pairs()[0].first == "replaygain_track_gain_db");
    CHECK(builder.customMetadata().pairs()[0].second == "-6.5");

    builder.customMetadata().clear();
    CHECK(builder.customMetadata().pairs().empty());
  }

  TEST_CASE("TrackBuilder - chained API updates builder state", "[library][unit][track][builder]")
  {
    auto builder = TrackBuilder::makeEmpty();

    builder.metadata().title("Song").artist("Artist").album("Album");
    builder.property().bitDepth(BitDepth{16});
    builder.tags().add("rock").add("jazz");
    builder.customMetadata().add("key", "value");

    CHECK(builder.metadata().title() == "Song");
    CHECK(builder.metadata().artist() == "Artist");
    CHECK(builder.metadata().album() == "Album");
    CHECK(builder.property().bitDepth() == 16);
    CHECK(builder.tags().names().size() == 2);
    CHECK(builder.customMetadata().pairs().size() == 1);
  }
} // namespace ao::library::test
