// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/DecoderFactory.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/BackendTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/media/wav/TestWav.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackSuccessionBaseTestSupport.h"
#include "test/unit/runtime/PlaybackTransportTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/Transport.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr std::size_t kSegmentFrames = 588;
    constexpr std::size_t kFrameBytes = 2;

    std::vector<TrackId> storeShortCarrier(PlaybackTransportFixture<QueuedExecutor>& fixture,
                                           std::filesystem::path const& sourcePath)
    {
      std::filesystem::copy_file(sourcePath, fixture.libraryFixture.root() / "disc.wav");
      auto tracks = std::array{
        library::TrackBuilder::makeEmpty(), library::TrackBuilder::makeEmpty(), library::TrackBuilder::makeEmpty()};
      tracks[0].metadata().title("First segment");
      tracks[1].metadata().title("Second segment");
      tracks[2].metadata().title("Third segment");

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
            .startFrame = index * kSegmentFrames,
            .frameCount = kSegmentFrames,
            .ordinal = static_cast<std::uint16_t>(index + 1),
          });
      }

      auto transaction = library::test::writeTransaction(fixture.libraryFixture.library());
      auto createdRes = transaction.apply(
        [&](library::LibraryWrite& write)
        {
          return write.tracks().createCarrier(tracks, library::FileManifestBuilder::makeEmpty(), kSegmentFrames * 3);
        });
      REQUIRE(createdRes);
      REQUIRE(transaction.commit());
      return *createdRes;
    }

    async::Task<void> preparationFenceAsync(async::Runtime* runtime, std::shared_ptr<bool> settledPtr)
    {
      co_await runtime->resumeOnCallbackExecutorAsync();
      *settledPtr = true;
    }

    void settleFinalOpen(PlaybackTransportFixture<QueuedExecutor>& fixture,
                         std::atomic<std::size_t> const& finalOpens,
                         std::size_t const expectedCount)
    {
      REQUIRE(fixture.executor.tryDrainUntil([&] { return finalOpens.load() >= expectedCount; }));
      // This fixture has exactly one preparation worker. Once the final decoder
      // opens, its uninterrupted preroll queues adoption before this worker fence
      // can run. The fence's owner turn therefore follows adoption, not just open.
      auto settledPtr = std::make_shared<bool>(false);
      auto future = fixture.asyncRuntime.spawn(preparationFenceAsync(&fixture.asyncRuntime, settledPtr));
      REQUIRE(fixture.executor.tryDrainUntil([&] { return *settledPtr; }));
      future.get();
      fixture.executor.drain();
      REQUIRE(finalOpens.load() == expectedCount);
    }

    void renderExact(audio::RenderTarget& target, std::size_t const frameCount, std::vector<std::byte>& captured)
    {
      auto output = std::vector<std::byte>(frameCount * kFrameBytes);
      auto const rendered = target.renderPcm(output);
      REQUIRE(rendered.bytesWritten == output.size());
      CHECK_FALSE(rendered.drained);
      target.handlePositionAdvanced(rendered.positionFrames);
      captured.append_range(output);
    }

    void requireDrain(audio::RenderTarget& target)
    {
      auto output = std::array<std::byte, kFrameBytes>{};
      auto const rendered = target.renderPcm(output);
      CHECK(rendered.bytesWritten == 0);
      REQUIRE(rendered.drained);
      // No further render is admitted in this run after this acknowledgement.
      target.handleDrainComplete();
    }

    // Every frame in the entire carrier has a different signed-16 PCM marker.
    ao::test::wav::Spec markedCarrierSpec()
    {
      auto spec = ao::test::wav::Spec{
        .sampleRate = 44100,
        .audioData = std::vector<std::uint8_t>(kSegmentFrames * 3 * kFrameBytes),
      };

      for (std::size_t frame = 0; frame < kSegmentFrames * 3; ++frame)
      {
        spec.audioData[frame * kFrameBytes] = static_cast<std::uint8_t>(frame & 0xffU);
        spec.audioData[(frame * kFrameBytes) + 1] = static_cast<std::uint8_t>(frame >> 8U);
      }

      return spec;
    }

    // Three segments of one WAV carrier, in list order in a navigated view.
    struct CarrierSuccessionFixture final
    {
      explicit CarrierSuccessionFixture(ao::test::wav::Spec const& spec)
        : file{ao::test::wav::makeWav(spec), ".wav"}
        , transport{[finalOpensPtr = finalOpensPtr](std::filesystem::path const& path,
                                                    std::optional<audio::SampleEncoding> optEncoding)
                    {
                      auto decoderRes = audio::openDecoderSession(path, optEncoding);

                      if (decoderRes && optEncoding)
                      {
                        finalOpensPtr->fetch_add(1);
                      }

                      return decoderRes;
                    }}
        , ids{storeShortCarrier(transport, file.path)}
        , changes{makeLibraryChanges(transport.executor, transport.libraryFixture.library())}
        , commands{transport.libraryFixture.library(), changes, transport.executor}
        , sources{transport.libraryFixture.library(), changes}
        , views{transport.executor, transport.libraryFixture.library(), sources, changes}
        , workspace{transport.executor, views, transport.libraryFixture.library(), changes}
        , succession{transport.executor,
                     views,
                     sources,
                     transport.libraryFixture.library(),
                     transport.playbackTransport,
                     transport.notificationService,
                     transport.asyncRuntime}
      {
        REQUIRE(ids.size() == 3);
        auto const membershipTag = std::array{std::string{"carrierorder"}};
        REQUIRE(commands.editTags(ids, membershipTag, {}));
        sources.reloadAllTracks();
        listId = ao::test::requireValue(commands.runTask(commands.commands().createListAsync(ListDraft{
          .name = "Carrier order",
          .expression = "#carrierorder",
        })));
        viewId = ao::test::requireValue(workspace.navigate(playback_succession::navigationRequest(TrackListViewConfig{
          .listId = listId,
          .optPresentation = TrackPresentationSpec{.id = std::string{kListOrderTrackPresentationId}},
        })));
      }

      std::shared_ptr<std::atomic<std::size_t>> finalOpensPtr = std::make_shared<std::atomic<std::size_t>>(0);
      ao::test::TempFile file;
      PlaybackTransportFixture<QueuedExecutor> transport;
      std::vector<TrackId> ids;
      LibraryChanges changes;
      LibraryCommandsFixture commands;
      TrackSourceCache sources;
      ViewService views;
      WorkspaceService workspace;
      PlaybackSuccession succession;
      ListId listId = kInvalidListId;
      ViewId viewId = kInvalidViewId;
    };
  } // namespace

  TEST_CASE("Carrier succession - three short segments preserve PCM with delayed owner callbacks",
            "[runtime][integration][playback-succession][segment][concurrency]")
  {
    auto const spec = markedCarrierSpec();
    auto expected = std::vector<std::byte>(spec.audioData.size());

    for (std::size_t index = 0; index < expected.size(); ++index)
    {
      expected[index] = static_cast<std::byte>(spec.audioData[index]);
    }

    auto carrier = CarrierSuccessionFixture{spec};
    auto& fixture = carrier.transport;
    auto const& ids = carrier.ids;
    auto const listId = carrier.listId;
    auto const viewId = carrier.viewId;
    auto& succession = carrier.succession;
    auto& finalOpensPtr = carrier.finalOpensPtr;
    auto& transport = fixture.playbackTransport;
    audio::test::SpyBackend<>& backend = *fixture.spyBackendPtr;
    auto& backendMock = backend.mock();
    auto nowPlaying = std::vector<PlaybackTransport::NowPlayingChanged>{};
    auto currentTracks = std::vector<TrackId>{};
    auto occurrences = std::vector<PlaybackOccurrenceId>{};
    auto eventOrder = std::vector<std::string>{};
    auto const nowPlayingSubscription = transport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& event)
      {
        nowPlaying.push_back(event);
        eventOrder.emplace_back("track");
      });
    auto const successionSubscription = succession.onChanged(
      [&](PlaybackSuccessionState const& state)
      {
        if (currentTracks.empty() || currentTracks.back() != state.currentTrackId)
        {
          currentTracks.push_back(state.currentTrackId);
          occurrences.push_back(transport.state().occurrenceId);
        }
      });
    auto const idleSubscription = transport.onIdle([&] { eventOrder.emplace_back("idle"); });
    auto const startedSubscription = transport.onStarted([&] { eventOrder.emplace_back("started"); });
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();
    REQUIRE(playback_succession::playFromViewAndWait(succession, fixture.executor, viewId, ids[0]));
    settleFinalOpen(fixture, *finalOpensPtr, 2);
    REQUIRE(fixture.renderTarget != nullptr);
    REQUIRE(succession.state().optResolvedSuccessor == ids[1]);
    CHECK(nowPlaying.empty());
    CHECK(eventOrder.empty());

    auto captured = std::vector<std::byte>{};
    // Cross A -> B and consume one quarter of B without delivering owner turns.
    renderExact(*fixture.renderTarget, kSegmentFrames + 147, captured);
    CHECK(nowPlaying.empty());
    CHECK(transport.state().nowPlaying.trackId == ids[0]);
    CHECK(succession.state().currentTrackId == ids[0]);
    CHECK(finalOpensPtr->load() == 2);
    bool late = false;

    SECTION("owner advances while B still has PCM and rearms C before its boundary")
    {
      REQUIRE(fixture.executor.tryDrainUntil([&] { return succession.state().currentTrackId == ids[1]; }));
      settleFinalOpen(fixture, *finalOpensPtr, 3);
      REQUIRE(succession.state().optResolvedSuccessor == ids[2]);
      CHECK(transport.state().transport == audio::Transport::Playing);
      CHECK(eventOrder == std::vector<std::string>{"track"});
      renderExact(*fixture.renderTarget, 441 + kSegmentFrames, captured);
      REQUIRE(fixture.executor.tryDrainUntil([&] { return succession.state().currentTrackId == ids[2]; }));
      fixture.executor.drain();
      CHECK(eventOrder == std::vector<std::string>{"track", "track"});
      fakeit::Verify(Method(backendMock, open)).Once();
      fakeit::Verify(Method(backendMock, start)).Once();
    }

    SECTION("owner is delayed past B drain and C starts through the idle fallback")
    {
      late = true;
      renderExact(*fixture.renderTarget, 441, captured);
      CHECK(nowPlaying.empty());
      CHECK(finalOpensPtr->load() == 2);
      requireDrain(*fixture.renderTarget);
      REQUIRE(fixture.executor.tryDrainUntil(
        [&]
        {
          return succession.state().currentTrackId == ids[2] &&
                 transport.state().transport == audio::Transport::Playing;
        }));
      fixture.executor.drain();
      // Natural idle recovery intentionally suppresses transport announcements;
      // succession publishes C and the backend starts a new run instead.
      CHECK(eventOrder == std::vector<std::string>{"track", "idle"});
      renderExact(*fixture.renderTarget, kSegmentFrames, captured);
      fakeit::Verify(Method(backendMock, open)).Exactly(2);
      fakeit::Verify(Method(backendMock, start)).Exactly(2);
    }

    REQUIRE(nowPlaying.size() == (late ? 1 : 2));
    CHECK(nowPlaying[0].trackId == ids[1]);
    CHECK(nowPlaying[0].sourceListId == listId);
    REQUIRE(nowPlaying[0].optPreparedNextToken);

    if (!late)
    {
      CHECK(nowPlaying[1].trackId == ids[2]);
      CHECK(nowPlaying[1].sourceListId == listId);
      REQUIRE(nowPlaying[1].optPreparedNextToken);
      CHECK(nowPlaying[1].optPreparedNextToken != nowPlaying[0].optPreparedNextToken);
    }

    CHECK(currentTracks == ids);
    REQUIRE(occurrences.size() == 3);
    CHECK(occurrences[0] != PlaybackOccurrenceId{});
    CHECK(occurrences[1] != PlaybackOccurrenceId{});
    CHECK(occurrences[2] != PlaybackOccurrenceId{});
    CHECK(occurrences[0] != occurrences[1]);
    CHECK(occurrences[1] != occurrences[2]);
    CHECK(occurrences[0] != occurrences[2]);
    CHECK(transport.state().occurrenceId == occurrences[2]);
    CHECK(transport.state().nowPlaying.trackId == ids[2]);
    CHECK_FALSE(succession.state().optResolvedSuccessor);
    CHECK(captured == expected);
    CHECK(fixture.notificationService.feed().entries.empty());

    // The only remaining drain is the ordinary terminal end of C.
    requireDrain(*fixture.renderTarget);
    REQUIRE(fixture.executor.tryDrainUntil([&] { return transport.state().transport == audio::Transport::Idle; }));
    // Idle can arrive via the Engine state callback before its event worker
    // queues track-ended delivery. An empty owner queue is not a terminal fence;
    // wait for succession's completion report before checking its payload.
    REQUIRE(fixture.executor.tryDrainUntil([&] { return !fixture.notificationService.feed().entries.empty(); }));
    auto const feed = fixture.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Info);
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    CHECK(std::get<NotificationReport>(feed.entries.front().message).templateId ==
          NotificationReportTemplate::PlaybackSequenceFinished);
  }

  TEST_CASE("Carrier succession - an end seek with the same-file successor armed advances once to it",
            "[runtime][integration][playback-succession][segment][seek]")
  {
    auto const spec = markedCarrierSpec();
    auto carrier = CarrierSuccessionFixture{spec};
    auto& fixture = carrier.transport;
    auto const& ids = carrier.ids;
    auto& transport = fixture.playbackTransport;
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();
    REQUIRE(playback_succession::playFromViewAndWait(carrier.succession, fixture.executor, carrier.viewId, ids[0]));
    settleFinalOpen(fixture, *carrier.finalOpensPtr, 2);
    REQUIRE(fixture.renderTarget != nullptr);
    REQUIRE(carrier.succession.state().optResolvedSuccessor == ids[1]);
    auto const first = transport.state();
    REQUIRE(first.nowPlaying.trackId == ids[0]);
    REQUIRE(first.duration > std::chrono::milliseconds{0});

    // The armed successor reads the same file; ending the current segment must
    // not also consume the successor and skip to the third segment.
    REQUIRE(transport.trySeek(first.occurrenceId, first.duration));
    REQUIRE(fixture.executor.tryDrainUntil(
      [&]
      {
        return carrier.succession.state().currentTrackId == ids[1] &&
               transport.state().transport == audio::Transport::Playing;
      }));
    fixture.executor.drain();
    CHECK(transport.state().nowPlaying.trackId == ids[1]);
    CHECK(transport.state().occurrenceId != first.occurrenceId);
    REQUIRE(fixture.renderTarget != nullptr);
    auto captured = std::vector<std::byte>{};
    renderExact(*fixture.renderTarget, kSegmentFrames / 2, captured);
    auto expected = std::vector<std::byte>{};

    for (auto const byte :
         std::span<std::uint8_t const>{spec.audioData}.subspan(kSegmentFrames * kFrameBytes, captured.size()))
    {
      expected.push_back(static_cast<std::byte>(byte));
    }

    // The second segment starts at its own first frame.
    CHECK(captured == expected);
    CHECK(carrier.succession.state().currentTrackId == ids[1]);
  }
} // namespace ao::rt::test
