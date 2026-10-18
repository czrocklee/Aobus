// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/media/wav/TestWav.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTransportTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/audio/RenderTarget.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::vector<TrackId> storeCarrier(PlaybackTransportFixture<QueuedExecutor>& fixture,
                                      std::filesystem::path const& sourcePath,
                                      std::uint64_t const segmentFrames)
    {
      std::filesystem::copy_file(sourcePath, fixture.libraryFixture.root() / "disc.wav");
      auto tracks = std::array{library::TrackBuilder::makeEmpty(), library::TrackBuilder::makeEmpty()};
      tracks[0].metadata().title("First segment");
      tracks[1].metadata().title("Second segment");

      for (std::size_t index = 0; index < tracks.size(); ++index)
      {
        tracks[index]
          .property()
          .uri("disc.wav")
          .sampleRate(SampleRate{44100})
          .channels(Channels{1})
          .bitDepth(BitDepth{16})
          .codec(AudioCodec::Wav)
          .segment(library::TrackSegment{
            .startFrame = index * segmentFrames,
            .frameCount = segmentFrames,
            .ordinal = static_cast<std::uint16_t>(index + 1),
          });
      }

      auto transaction = library::test::writeTransaction(fixture.libraryFixture.library());
      auto createdRes = transaction.apply(
        [&](library::LibraryWrite& write)
        { return write.tracks().createCarrier(tracks, library::FileManifestBuilder::makeEmpty(), segmentFrames * 2); });
      REQUIRE(createdRes);
      REQUIRE(transaction.commit());
      return *createdRes;
    }

    void readyCarrierPlayback(PlaybackTransportFixture<QueuedExecutor>& fixture)
    {
      fixture.onDevicesChangedCb(fixture.status.devices);
      fixture.executor.drain();
    }
  } // namespace

  TEST_CASE("Carrier playback - stored second segment reaches final PCM with exact source boundaries",
            "[runtime][integration][playback][segment]")
  {
    auto spec = ao::test::wav::Spec{
      .sampleRate = 44100,
      .audioData = std::vector<std::uint8_t>(std::size_t{1176} * 2),
    };

    for (std::size_t index = 0; index < 1176; ++index)
    {
      spec.audioData[index * 2] = static_cast<std::uint8_t>(index & 0xffU);
      spec.audioData[(index * 2) + 1] = static_cast<std::uint8_t>(index >> 8U);
    }

    auto const bytes = ao::test::wav::makeWav(spec);
    auto const file = ao::test::TempFile{bytes, ".wav"};
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    auto const ids = storeCarrier(fixture, file.path, 588);
    REQUIRE(ids.size() == 2);
    auto const requestRes = playbackRequestForTrack(fixture.libraryFixture.library(), ids[1]);
    REQUIRE(requestRes);
    REQUIRE(requestRes->input.optSegment);
    CHECK(requestRes->input.optSegment->startFrame == 588);
    CHECK(requestRes->input.optSegment->frameCount == 588);
    CHECK(requestRes->input.optSegment->totalFrames == 1176);
    CHECK(requestRes->input.sampleRateHint == 44100);
    CHECK(requestRes->input.duration == std::chrono::milliseconds{13});
    CHECK(requestRes->trackOrdinal == 2);
    readyCarrierPlayback(fixture);
    REQUIRE(fixture.playbackTransport.playTrack(ids[1], ListId{1}));
    fixture.executor.drain();
    REQUIRE(fixture.renderTarget != nullptr);
    auto output = std::array<std::byte, 4096>{};
    auto const rendered = fixture.renderTarget->renderPcm(output);
    REQUIRE(rendered.bytesWritten == std::size_t{588} * 2);

    for (std::size_t index = 0; index < rendered.bytesWritten; ++index)
    {
      CHECK(output[index] == static_cast<std::byte>(spec.audioData[(std::size_t{588} * 2) + index]));
    }

    CHECK(fixture.renderTarget->renderPcm(output).bytesWritten == 0);
  }

  TEST_CASE("Carrier playback - nonempty zero millisecond segment plays at offset zero",
            "[runtime][integration][playback][segment]")
  {
    auto const bytes = ao::test::wav::makeWav(ao::test::wav::Spec{
      .sampleRate = 44100,
      .audioData = {0x11, 0x12, 0x21, 0x22},
    });
    auto const file = ao::test::TempFile{bytes, ".wav"};
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    auto const ids = storeCarrier(fixture, file.path, 1);
    readyCarrierPlayback(fixture);
    REQUIRE(fixture.playbackTransport.playTrack(ids[1], ListId{1}));
    fixture.executor.drain();
    CHECK(fixture.playbackTransport.state().duration == std::chrono::milliseconds{0});
    CHECK_FALSE(
      fixture.playbackTransport.trySeek(fixture.playbackTransport.state().occurrenceId, std::chrono::milliseconds{0}));
    fixture.playbackTransport.seek(std::chrono::milliseconds{0});
    REQUIRE(fixture.renderTarget != nullptr);
    auto output = std::array<std::byte, 16>{};
    auto const rendered = fixture.renderTarget->renderPcm(output);
    REQUIRE(rendered.bytesWritten == 2);
    CHECK(output[0] == std::byte{0x21});
    CHECK(output[1] == std::byte{0x22});
  }
} // namespace ao::rt::test
