// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackTransport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/ScriptedDecoderSession.h"
#include "test/library/TrackTestSupport.h"
#include "test/media/wav/TestWav.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/PlaybackSuccessionTransportTestSupport.h"
#include "test/runtime/PlaybackTransportTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/Transport.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    PlaybackTransport::PlaybackRequest startEndSeekTrack(PlaybackTransportFixture<QueuedExecutor>& fixture,
                                                         std::string_view const fileName)
    {
      fixture.onDevicesChangedCb(fixture.status.devices);
      fixture.executor.drain();
      auto const uri = fixture.installAudioFixture(fileName, fileName);
      auto const request = playbackRequest(
        TrackId{1}, (fixture.libraryFixture.root() / uri).string(), "End seek", "Artist", std::chrono::minutes{2});
      REQUIRE(fixture.playbackTransport.play(request, ListId{1}));
      fixture.executor.drain();
      return request;
    }
  } // namespace

  TEST_CASE("PlaybackTransport - final end seek completes every codec without a decoder EOF",
            "[runtime][unit][playback][seek]")
  {
    for (auto const fileName : std::to_array<std::string_view>({"basic_metadata.flac",
                                                                "basic_metadata.wav",
                                                                "basic_metadata.mp3",
                                                                "basic_metadata.m4a",
                                                                "alac16.m4a",
                                                                "basic_metadata.opus"}))
    {
      for (bool const paused : {false, true})
      {
        CAPTURE(fileName, paused);
        auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
        auto const request = startEndSeekTrack(fixture, fileName);
        auto const duration = fixture.playbackTransport.state().duration;
        auto const occurrence = fixture.playbackTransport.state().occurrenceId;
        REQUIRE(duration > std::chrono::milliseconds{0});
        std::size_t endedCount = 0;
        std::size_t stoppedCount = 0;
        auto const ended = fixture.playbackTransport.onIdle([&] { ++endedCount; });
        auto const stopped = fixture.playbackTransport.onStopped([&] { ++stoppedCount; });

        if (paused)
        {
          fixture.playbackTransport.pause();
        }

        REQUIRE(fixture.playbackTransport.trySeek(occurrence, duration, PlaybackTransport::SeekMode::Preview));
        CHECK(endedCount == 0);
        CHECK_FALSE(fixture.playbackTransport.trySeek(occurrence, duration + std::chrono::milliseconds{1}));
        REQUIRE(fixture.playbackTransport.trySeek(occurrence, duration));

        if (paused)
        {
          CHECK(endedCount == 0);
          CHECK(fixture.playbackTransport.state().transport == audio::Transport::Paused);
          CHECK(fixture.playbackTransport.elapsed() == duration);
          fixture.executor.drain();
          CHECK(fixture.playbackTransport.state().transport == audio::Transport::Paused);
          CHECK(fixture.playbackTransport.elapsed() == duration);
          // An end intent must not reopen even a file that vanished while held.
          REQUIRE(std::filesystem::remove(request.input.filePath));
          fixture.playbackTransport.resume();
        }

        CHECK(endedCount == 1);
        CHECK(stoppedCount == 0);
        CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);
        CHECK(fixture.playbackTransport.state().occurrenceId.value == 0);
        fixture.playbackTransport.resume();
        fixture.executor.drain();
        CHECK(endedCount == 1);
        CHECK(stoppedCount == 0);
      }
    }
  }

  TEST_CASE("PlaybackTransport - unguarded final seek clamps past the end and negative positions",
            "[runtime][unit][playback][seek]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    std::ignore = startEndSeekTrack(fixture, "basic_metadata.wav");
    fixture.playbackTransport.pause();
    fixture.playbackTransport.seek(std::chrono::milliseconds{-10});
    CHECK(fixture.playbackTransport.elapsed() == std::chrono::milliseconds{0});
    auto const duration = fixture.playbackTransport.state().duration;
    fixture.playbackTransport.seek(duration + std::chrono::milliseconds{10});
    CHECK(fixture.playbackTransport.elapsed() == duration);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Paused);
    fixture.playbackTransport.resume();
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);
  }

  TEST_CASE("PlaybackTransport - unguarded final seek keeps a position beyond an unknown duration",
            "[runtime][unit][playback][seek]")
  {
    auto const format =
      audio::PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = audio::SampleEncoding::Signed16Le};
    auto seekOffsetPtr = std::make_shared<std::atomic<std::int64_t>>(-1);
    auto const factory = [format, seekOffsetPtr](auto const&, std::optional<audio::SampleEncoding> optOutputEncoding)
    {
      auto const sourceFormat = audio::signalFormat(format);
      // Zero duration is unknown: there is no end for the transport to clamp to.
      auto decoderPtr = std::make_unique<audio::test::ScriptedDecoderSession>(audio::DecodedStreamInfo{
        .sourceFormat = sourceFormat,
        .outputFormat = audio::pcmFormat(sourceFormat, optOutputEncoding.value_or(format.encoding)),
        .duration = std::chrono::milliseconds{0},
        .isLossy = false});
      auto const data = std::vector<std::byte>(20000, std::byte{0});
      decoderPtr->setReadScript({{.data = data}, {.data = data}, {.endOfStream = true}});
      decoderPtr->setSeekObserver([seekOffsetPtr](std::chrono::milliseconds const offset)
                                  { seekOffsetPtr->store(offset.count()); });
      return decoderPtr;
    };
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{factory};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();
    auto const uri = fixture.installAudioFixture();
    auto const request = playbackRequest(
      TrackId{1}, (fixture.libraryFixture.root() / uri).string(), "Unknown", "Artist", std::chrono::milliseconds{0});
    REQUIRE(fixture.playbackTransport.play(request, ListId{1}));
    fixture.executor.drain();
    REQUIRE(fixture.playbackTransport.state().duration == std::chrono::milliseconds{0});
    fixture.playbackTransport.pause();

    fixture.playbackTransport.seek(std::chrono::milliseconds{3000});

    REQUIRE(fixture.executor.tryDrainUntil([&] { return seekOffsetPtr->load() == 3000; }));
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Paused);
  }

  TEST_CASE("PlaybackTransport - seeking inside a held paused end resumes from the new position",
            "[runtime][unit][playback][seek]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    std::ignore = startEndSeekTrack(fixture, "basic_metadata.wav");
    fixture.playbackTransport.pause();
    auto const duration = fixture.playbackTransport.state().duration;
    auto const occurrence = fixture.playbackTransport.state().occurrenceId;
    // 100 ms is frame-aligned at 44.1 kHz; this case tests held-intent
    // replacement rather than the decoder's millisecond flooring.
    auto const insideElapsed = std::chrono::milliseconds{100};
    REQUIRE(insideElapsed < duration);
    REQUIRE(fixture.playbackTransport.trySeek(occurrence, duration));
    REQUIRE(fixture.playbackTransport.trySeek(occurrence, insideElapsed));
    fixture.executor.drain();
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Paused);
    CHECK(fixture.playbackTransport.elapsed() == insideElapsed);
    fixture.playbackTransport.resume();
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.playbackTransport.elapsed() == insideElapsed);
  }

  TEST_CASE("PlaybackTransport - resuming inside a held end opens the file of a Track relinked meanwhile",
            "[runtime][unit][playback][seek]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();
    auto const uri = fixture.installAudioFixture("basic_metadata.wav", "held.wav");
    auto const trackId = fixture.libraryFixture.addTrack(library::test::TrackSpec{.title = "Held", .uri = uri});
    auto const requestRes = playbackRequestForTrack(fixture.libraryFixture.library(), trackId);
    REQUIRE(requestRes);
    REQUIRE(fixture.playbackTransport.play(*requestRes, ListId{1}));
    fixture.executor.drain();
    fixture.playbackTransport.pause();
    auto const duration = fixture.playbackTransport.state().duration;
    auto const occurrence = fixture.playbackTransport.state().occurrenceId;
    auto const insideElapsed = std::chrono::milliseconds{100};
    REQUIRE(insideElapsed < duration);
    REQUIRE(fixture.playbackTransport.trySeek(occurrence, duration));
    REQUIRE(fixture.playbackTransport.trySeek(occurrence, insideElapsed));
    fixture.executor.drain();

    // The held request still names the old path when the Track is relinked.
    auto const root = fixture.libraryFixture.root();
    std::filesystem::rename(root / uri, root / "moved.wav");
    fixture.libraryFixture.updateTrack(trackId, [](library::test::TrackSpec& spec) { spec.uri = "moved.wav"; });
    fixture.playbackTransport.resume();
    fixture.executor.drain();

    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == trackId);
    CHECK(fixture.playbackTransport.elapsed() == insideElapsed);
  }

  TEST_CASE("PlaybackTransport - end seek rejects an occurrence whose successor already spliced",
            "[runtime][unit][playback][concurrency]")
  {
    auto fixture = playback_succession::PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto playback = fixture.createPlayback();
    REQUIRE(fixture.playAndWait(fixture.firstTrackId));
    fixture.transport.executor.drain();
    auto const before = playback.snapshot().transport;
    fixture.queueNaturalAdvance();
    REQUIRE(fixture.transport.playbackTransport.state().occurrenceId == before.occurrenceId);

    CHECK_FALSE(playback.commands().trySeek(before.occurrenceId, before.duration));
    fixture.transport.executor.drain();
    CHECK(playback.snapshot().transport.nowPlaying.trackId == fixture.secondTrackId);
    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
    CHECK(playback.snapshot().transport.occurrenceId != before.occurrenceId);
    CHECK(playback.snapshot().transport.finalSeekRevision == before.finalSeekRevision);
  }

  TEST_CASE("PlaybackTransport - queued natural completion and end seek publish only one end",
            "[runtime][unit][playback][concurrency]")
  {
    auto const bytes = ao::test::wav::makeWav(ao::test::wav::Spec{
      .sampleRate = 44100,
      .audioData = std::vector<std::uint8_t>(std::size_t{588} * 2, 0x11),
    });
    auto const file = ao::test::TempFile{bytes, ".wav"};
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();
    auto const request =
      playbackRequest(TrackId{1}, file.path.string(), "Partial millisecond", "Artist", std::chrono::milliseconds{13});
    REQUIRE(fixture.playbackTransport.play(request, ListId{1}));
    fixture.executor.drain();
    auto const duration = fixture.playbackTransport.state().duration;
    REQUIRE(duration == std::chrono::milliseconds{13});
    std::size_t endedCount = 0;
    auto const ended = fixture.playbackTransport.onIdle([&] { ++endedCount; });
    REQUIRE(fixture.renderTarget != nullptr);
    auto output = std::array<std::byte, 4096>{};

    // Render without delivering owner callbacks, then submit the competing end.
    REQUIRE(fixture.renderTarget->renderPcm(output).bytesWritten == std::size_t{588} * 2);
    REQUIRE(fixture.renderTarget->renderPcm(output).bytesWritten == 0);
    fixture.renderTarget->handleDrainComplete();
    REQUIRE(fixture.executor.tryWaitUntilQueued(std::chrono::seconds{10}));
    fixture.playbackTransport.seek(duration);
    // A queued state notification does not mean the event worker has also
    // posted its terminal callback. Await that observable completion boundary.
    REQUIRE(fixture.executor.tryDrainUntil([&] { return endedCount == 1; }));
    CHECK(endedCount == 1);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);
    fixture.playbackTransport.resume();
    fixture.executor.drain();
    CHECK(endedCount == 1);
  }
} // namespace ao::rt::test
