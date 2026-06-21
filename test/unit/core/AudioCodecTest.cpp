// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/AudioCodec.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::test
{
  TEST_CASE("AudioCodec - names map to stable display strings", "[core][unit][codec]")
  {
    CHECK(audioCodecName(AudioCodec::Unknown).empty());
    CHECK(audioCodecName(AudioCodec::Flac) == "FLAC");
    CHECK(audioCodecName(AudioCodec::Alac) == "ALAC");
    CHECK(audioCodecName(AudioCodec::Aac) == "AAC");
    CHECK(audioCodecName(AudioCodec::Mp3) == "MP3");
  }

  TEST_CASE("AudioCodec - parser accepts case-insensitive names", "[core][unit][codec]")
  {
    CHECK(parseAudioCodecName("Flac") == AudioCodec::Flac);
    CHECK(parseAudioCodecName("alac") == AudioCodec::Alac);
    CHECK(parseAudioCodecName("aac") == AudioCodec::Aac);
    CHECK(parseAudioCodecName("mp3") == AudioCodec::Mp3);
    CHECK(parseAudioCodecName("unknown") == AudioCodec::Unknown);
    CHECK_FALSE(parseAudioCodecName("not-a-codec").has_value());
  }

  TEST_CASE("AudioCodec - storage conversion rejects unknown raw values", "[core][unit][codec]")
  {
    CHECK(audioCodecStorageValue(AudioCodec::Flac) == 1);
    CHECK(audioCodecStorageValue(AudioCodec::Alac) == 2);
    CHECK(audioCodecStorageValue(AudioCodec::Aac) == 128);
    CHECK(audioCodecStorageValue(AudioCodec::Mp3) == 129);
    CHECK(audioCodecFromStorage(1) == AudioCodec::Flac);
    CHECK(audioCodecFromStorage(2) == AudioCodec::Alac);
    CHECK(audioCodecFromStorage(128) == AudioCodec::Aac);
    CHECK(audioCodecFromStorage(129) == AudioCodec::Mp3);
    CHECK(audioCodecFromStorage(3) == AudioCodec::Unknown);
    CHECK(audioCodecFromStorage(255) == AudioCodec::Unknown);
  }
} // namespace ao::test
