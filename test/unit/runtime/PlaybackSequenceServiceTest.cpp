// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/audio/ScriptedDecoderSession.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/async/Runtime.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Format.h>
#include <ao/audio/Player.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PreparedPlayback.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct PlaybackSequenceFixture final
    {
      PlaybackSequenceFixture()
        : asyncRuntime{executor}
        , writer{libraryFixture.library(), changes}
        , sources{libraryFixture.library(), changes}
        , views{executor, libraryFixture.library(), sources}
        , playback{makePlaybackService(executor, libraryFixture.library(), notifications)}
      {
        playback.addProvider(makeReadyAudioProvider());
      }

      TrackId addPlayableTrack(std::string title, std::uint16_t const year = 2020)
      {
        auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac");
        return libraryFixture.addTrack(library::test::TrackSpec{
          .title = std::move(title), .uri = fixturePath.string(), .year = year, .codec = AudioCodec::Flac});
      }

      void openManualView(std::span<TrackId const> const trackIds, TrackListViewConfig config = {})
      {
        sources.reloadAllTracks();
        listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
          .kind = LibraryWriter::ListKind::Manual,
          .name = "Playback order",
          .trackIds = {trackIds.begin(), trackIds.end()},
        }));
        config.listId = listId;
        viewId = ao::test::requireValue(views.createView(config, true)).viewId;
        sequencePtr = std::make_unique<PlaybackSequenceService>(
          executor, views, sources, libraryFixture.library(), playback, notifications, asyncRuntime);
      }

      void buildThreeTrackManualView(TrackListViewConfig config = {})
      {
        firstTrackId = addPlayableTrack("First", 1990);
        secondTrackId = addPlayableTrack("Second", 2000);
        thirdTrackId = addPlayableTrack("Third", 2010);
        openManualView(std::array{firstTrackId, secondTrackId, thirdTrackId}, std::move(config));
      }

      MusicLibraryFixture libraryFixture;
      InlineExecutor executor;
      async::Runtime asyncRuntime;
      LibraryChanges changes;
      LibraryWriter writer;
      TrackSourceCache sources;
      ViewService views;
      NotificationService notifications;
      PlaybackService playback;
      std::unique_ptr<PlaybackSequenceService> sequencePtr;
      TrackId firstTrackId = kInvalidTrackId;
      TrackId secondTrackId = kInvalidTrackId;
      TrackId thirdTrackId = kInvalidTrackId;
      ListId listId = kInvalidListId;
      ViewId viewId = kInvalidViewId;
    };

    struct PlaybackSequenceTransportFixture final
    {
      PlaybackSequenceTransportFixture()
        : asyncRuntime{transport.executor}
        , writer{transport.libraryFixture.library(), changes}
        , sources{transport.libraryFixture.library(), changes}
        , views{transport.executor, transport.libraryFixture.library(), sources}
      {
        transport.onDevicesChangedCb(transport.status.devices);
        transport.executor.drain();
      }

      TrackId addPlayableTrack(std::string title)
      {
        auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac");
        return transport.libraryFixture.addTrack(
          library::test::TrackSpec{.title = std::move(title), .uri = fixturePath.string(), .codec = AudioCodec::Flac});
      }

      void buildThreeTrackManualView()
      {
        firstTrackId = addPlayableTrack("First");
        secondTrackId = addPlayableTrack("Second");
        thirdTrackId = addPlayableTrack("Third");
        sources.reloadAllTracks();
        listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
          .kind = LibraryWriter::ListKind::Manual,
          .name = "Transport order",
          .trackIds = {firstTrackId, secondTrackId, thirdTrackId},
        }));
        viewId = ao::test::requireValue(views.createView({.listId = listId}, true)).viewId;
        sequencePtr = std::make_unique<PlaybackSequenceService>(transport.executor,
                                                                views,
                                                                sources,
                                                                transport.libraryFixture.library(),
                                                                transport.playbackService,
                                                                transport.notificationService,
                                                                asyncRuntime);
      }

      void queueNaturalAdvance()
      {
        transport.executor.drain();
        REQUIRE(transport.renderTarget != nullptr);
        auto output = std::array<std::byte, 4096>{};
        REQUIRE(driveRenderUntilTaskQueued(*transport.renderTarget, transport.executor, output));
      }

      PlaybackFixture<QueuedExecutor> transport;
      async::Runtime asyncRuntime;
      LibraryChanges changes;
      LibraryWriter writer;
      TrackSourceCache sources;
      ViewService views;
      std::unique_ptr<PlaybackSequenceService> sequencePtr;
      TrackId firstTrackId = kInvalidTrackId;
      TrackId secondTrackId = kInvalidTrackId;
      TrackId thirdTrackId = kInvalidTrackId;
      ListId listId = kInvalidListId;
      ViewId viewId = kInvalidViewId;
    };

    struct PlaybackSequenceSeekFixture final
    {
      explicit PlaybackSequenceSeekFixture(audio::test::StagedFailureGate* const failureGate = nullptr)
        : asyncRuntime{executor}
        , writer{libraryFixture.library(), changes}
        , sources{libraryFixture.library(), changes}
        , views{executor, libraryFixture.library(), sources}
      {
        // A 48 kHz clock represents every whole millisecond exactly, including
        // the 3001 ms edge immediately above the strict restart threshold.
        auto const format = audio::Format{.sampleRate = 48000, .channels = 2, .bitDepth = 16, .isInterleaved = true};
        auto decoderFactory = audio::DecoderFactoryFn{};

        if (failureGate != nullptr)
        {
          decoderFactory = [failureGate](std::filesystem::path const&, audio::Format const&)
          { return std::make_unique<audio::test::StagedFailureDecoderSession>(failureGate); };
        }
        else
        {
          decoderFactory = [format](std::filesystem::path const&, audio::Format const&)
          {
            auto decoderPtr = std::make_unique<audio::test::ScriptedDecoderSession>(audio::DecodedStreamInfo{
              .sourceFormat = format,
              .outputFormat = format,
              .duration = std::chrono::seconds{10},
              .isLossy = false,
              .codec = AudioCodec::Flac,
            });
            auto const data = std::vector<std::byte>(100000, std::byte{0});
            decoderPtr->setReadScript(
              {{.data = data, .endOfStream = false}, {.data = data, .endOfStream = false}, {.endOfStream = true}});
            return decoderPtr;
          };
        }

        auto playerPtr = std::make_unique<audio::Player>(executor, std::move(decoderFactory));
        playbackPtr =
          std::make_unique<PlaybackService>(executor, libraryFixture.library(), notifications, std::move(playerPtr));
        playbackPtr->addProvider(makeReadyAudioProvider());
        executor.drain();
      }

      void buildThreeTrackManualView()
      {
        firstTrackId = libraryFixture.addTrack(library::test::TrackSpec{
          .title = "First", .uri = "first.flac", .duration = std::chrono::seconds{10}, .codec = AudioCodec::Flac});
        secondTrackId = libraryFixture.addTrack(library::test::TrackSpec{
          .title = "Second", .uri = "second.flac", .duration = std::chrono::seconds{10}, .codec = AudioCodec::Flac});
        thirdTrackId = libraryFixture.addTrack(library::test::TrackSpec{
          .title = "Third", .uri = "third.flac", .duration = std::chrono::seconds{10}, .codec = AudioCodec::Flac});
        sources.reloadAllTracks();
        listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
          .kind = LibraryWriter::ListKind::Manual,
          .name = "Long playback order",
          .trackIds = {firstTrackId, secondTrackId, thirdTrackId},
        }));
        viewId = ao::test::requireValue(views.createView({.listId = listId}, true)).viewId;
        sequencePtr = std::make_unique<PlaybackSequenceService>(
          executor, views, sources, libraryFixture.library(), *playbackPtr, notifications, asyncRuntime);
      }

      void buildSingleTrackManualView()
      {
        firstTrackId = libraryFixture.addTrack(library::test::TrackSpec{
          .title = "Failing current", .uri = "failing-current.flac", .codec = AudioCodec::Flac});
        sources.reloadAllTracks();
        listId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{
          .kind = LibraryWriter::ListKind::Manual,
          .name = "Failing playback order",
          .trackIds = {firstTrackId},
        }));
        viewId = ao::test::requireValue(views.createView({.listId = listId}, true)).viewId;
        sequencePtr = std::make_unique<PlaybackSequenceService>(
          executor, views, sources, libraryFixture.library(), *playbackPtr, notifications, asyncRuntime);
      }

      MusicLibraryFixture libraryFixture;
      QueuedExecutor executor;
      async::Runtime asyncRuntime;
      LibraryChanges changes;
      LibraryWriter writer;
      TrackSourceCache sources;
      ViewService views;
      NotificationService notifications;
      std::unique_ptr<PlaybackService> playbackPtr;
      std::unique_ptr<PlaybackSequenceService> sequencePtr;
      TrackId firstTrackId = kInvalidTrackId;
      TrackId secondTrackId = kInvalidTrackId;
      TrackId thirdTrackId = kInvalidTrackId;
      ListId listId = kInvalidListId;
      ViewId viewId = kInvalidViewId;
    };
  } // namespace

  TEST_CASE("PlaybackSequenceService - strict launch commits only a validated captured spec",
            "[runtime][unit][playback-sequence][launch]")
  {
    auto fixture = PlaybackSequenceFixture{};
    fixture.buildThreeTrackManualView();
    auto& sequence = *fixture.sequencePtr;
    std::uint32_t changedCount = 0;
    auto const changedSubscription = sequence.onChanged([&](PlaybackSequenceState const&) { ++changedCount; });

    REQUIRE(sequence.playFromView(fixture.viewId, fixture.firstTrackId));
    auto const accepted = sequence.state();
    CHECK(accepted.sourceState == PlaybackSequenceSourceState::Live);
    CHECK(accepted.currentTrackId == fixture.firstTrackId);
    CHECK(accepted.sourceListId == fixture.listId);
    CHECK(accepted.hasNext);
    CHECK_FALSE(accepted.hasPrevious);
    CHECK(accepted.optResolvedSuccessor == fixture.secondTrackId);
    CHECK(fixture.playback.state().transport == audio::Transport::Playing);
    CHECK(changedCount == 1);

    SECTION("unknown view")
    {
      auto const rejected = sequence.playFromView(ViewId{999999}, fixture.secondTrackId);
      REQUIRE_FALSE(rejected);
      CHECK(rejected.error().code == Error::Code::NotFound);
    }

    SECTION("start absent from captured projection")
    {
      auto const outsideTrackId = fixture.libraryFixture.addTrack(library::test::TrackSpec{.title = "Outside"});
      fixture.sources.reloadAllTracks();
      auto const rejected = sequence.playFromView(fixture.viewId, outsideTrackId);
      REQUIRE_FALSE(rejected);
      CHECK(rejected.error().code == Error::Code::NotFound);
    }

    CHECK(sequence.state() == accepted);
    CHECK(fixture.playback.state().nowPlaying.trackId == fixture.firstTrackId);
    CHECK(changedCount == 1);
  }

  TEST_CASE("PlaybackSequenceService - staging rejection preserves the accepted session and transport",
            "[runtime][regression][playback-sequence][launch]")
  {
    auto fixture = PlaybackSequenceFixture{};
    auto const current = fixture.addPlayableTrack("Current");
    auto const successor = fixture.addPlayableTrack("Successor");
    auto const broken = fixture.libraryFixture.addTrack(
      library::test::TrackSpec{.title = "Broken", .uri = "/missing/relaunch.flac", .codec = AudioCodec::Flac});
    fixture.openManualView(std::array{current, successor, broken});
    auto& sequence = *fixture.sequencePtr;

    REQUIRE(sequence.playFromView(fixture.viewId, current));
    auto const sequenceBeforeRejection = sequence.state();
    auto const transportBeforeRejection = fixture.playback.state();
    REQUIRE(sequenceBeforeRejection.optResolvedSuccessor == successor);

    auto const rejected = sequence.playFromView(fixture.viewId, broken);

    REQUIRE_FALSE(rejected);
    auto const rejectionFeed = fixture.notifications.feed();
    REQUIRE(rejectionFeed.entries.size() == 1);
    CHECK(rejectionFeed.entries.front().severity == NotificationSeverity::Error);
    CHECK(rejectionFeed.entries.front().sticky);
    CHECK(rejectionFeed.entries.front().content.topic == NotificationTopic::PlaybackError);
    CHECK(sequence.state() == sequenceBeforeRejection);
    CHECK(sequence.state().semanticRevision == sequenceBeforeRejection.semanticRevision);

    auto const transportAfterRejection = fixture.playback.state();
    CHECK(transportAfterRejection.transport == transportBeforeRejection.transport);
    CHECK(transportAfterRejection.elapsed == transportBeforeRejection.elapsed);
    CHECK(transportAfterRejection.duration == transportBeforeRejection.duration);
    CHECK(transportAfterRejection.ready == transportBeforeRejection.ready);
    CHECK(transportAfterRejection.nowPlaying == transportBeforeRejection.nowPlaying);
    CHECK(transportAfterRejection.volume == transportBeforeRejection.volume);
    CHECK(transportAfterRejection.output == transportBeforeRejection.output);
    CHECK(transportAfterRejection.quality == transportBeforeRejection.quality);
    CHECK(transportAfterRejection.revision == transportBeforeRejection.revision);

    sequence.next();

    CHECK(sequence.state().currentTrackId == successor);
    CHECK(fixture.playback.state().nowPlaying.trackId == successor);
    CHECK(fixture.playback.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSequenceService - accepted launch contains synchronous reentrant commands and observer throws",
            "[runtime][regression][playback-sequence][launch]")
  {
    auto fixture = PlaybackSequenceFixture{};
    fixture.buildThreeTrackManualView();
    auto& sequence = *fixture.sequencePtr;
    auto& playback = fixture.playback;
    REQUIRE(sequence.playFromView(fixture.viewId, fixture.firstTrackId));

    bool callbackEntered = false;
    bool changedObserverEntered = false;
    bool reentrantLaunchAccepted = false;
    auto reentrantLaunchError = Error::Code::Generic;
    auto reentrantStopBarrier = PreparedCancellationBarrier{};
    auto changedSubscription = sequence.onChanged(
      [&](PlaybackSequenceState const&)
      {
        changedObserverEntered = true;
        throwException<Exception>("scripted accepted sequence observer failure");
      });
    auto const startedSubscription = playback.onStarted(
      [&]
      {
        if (callbackEntered)
        {
          return;
        }

        callbackEntered = true;
        sequence.next();
        auto const launch = sequence.playFromView(fixture.viewId, fixture.secondTrackId);
        reentrantLaunchAccepted = launch.has_value();

        if (!launch)
        {
          reentrantLaunchError = launch.error().code;
        }

        reentrantStopBarrier = playback.stop();
        sequence.clear();
        sequence.setShuffleMode(ShuffleMode::On);
        sequence.setRepeatMode(RepeatMode::All);
        throwException<Exception>("scripted accepted-launch observer failure");
      });

    auto const launched = sequence.playFromView(fixture.viewId, fixture.thirdTrackId);

    REQUIRE(launched);
    CHECK(callbackEntered);
    CHECK(changedObserverEntered);
    CHECK_FALSE(reentrantLaunchAccepted);
    CHECK(reentrantLaunchError == Error::Code::InvalidState);
    CHECK(reentrantStopBarrier.generation == 0);
    CHECK(sequence.state().sourceState == PlaybackSequenceSourceState::Live);
    CHECK(sequence.state().currentTrackId == fixture.thirdTrackId);
    CHECK(sequence.state().shuffle == ShuffleMode::Off);
    CHECK(sequence.state().repeat == RepeatMode::Off);
    CHECK(playback.state().transport == audio::Transport::Playing);
    CHECK(playback.state().nowPlaying.trackId == sequence.state().currentTrackId);

    changedSubscription.reset();
    sequence.previous();

    CHECK(sequence.state().currentTrackId == fixture.secondTrackId);
    CHECK(playback.state().nowPlaying.trackId == fixture.secondTrackId);
    CHECK(playback.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSequenceService - live membership governs succession without interrupting current audio",
            "[runtime][unit][playback-sequence][projection]")
  {
    auto fixture = PlaybackSequenceFixture{};
    fixture.buildThreeTrackManualView();
    auto& sequence = *fixture.sequencePtr;
    REQUIRE(sequence.playFromView(fixture.viewId, fixture.firstTrackId));
    auto const beforeRemoval = sequence.state();

    auto const removed = fixture.writer.removeManualListTracks(fixture.listId, std::array{fixture.firstTrackId});
    REQUIRE(removed);
    REQUIRE(removed->changed);

    auto const afterRemoval = sequence.state();
    CHECK(afterRemoval.currentTrackId == fixture.firstTrackId);
    CHECK(afterRemoval.sourceState == PlaybackSequenceSourceState::Live);
    CHECK(afterRemoval.optResolvedSuccessor == fixture.secondTrackId);
    CHECK(afterRemoval.semanticRevision == beforeRemoval.semanticRevision);
    CHECK(fixture.playback.state().transport == audio::Transport::Playing);
    CHECK(fixture.playback.state().nowPlaying.trackId == fixture.firstTrackId);

    REQUIRE(fixture.writer.deleteList(fixture.listId));
    auto const invalidated = sequence.state();
    CHECK(invalidated.sourceState == PlaybackSequenceSourceState::Invalidated);
    CHECK(invalidated.currentTrackId == fixture.firstTrackId);
    CHECK_FALSE(invalidated.hasNext);
    CHECK_FALSE(invalidated.optResolvedSuccessor);
    CHECK(fixture.playback.state().transport == audio::Transport::Playing);

    auto const rejectedRelaunch = sequence.playFromView(fixture.viewId, fixture.firstTrackId);
    REQUIRE_FALSE(rejectedRelaunch);
    CHECK(rejectedRelaunch.error().code == Error::Code::NotFound);
    CHECK(sequence.state() == invalidated);

    sequence.next();
    CHECK(sequence.state().sourceState == PlaybackSequenceSourceState::Inactive);
    CHECK(fixture.playback.state().transport == audio::Transport::Idle);

    auto const feed = fixture.notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().message == "Playback sequence finished");
    CHECK(feed.entries.front().severity == NotificationSeverity::Info);
    CHECK_FALSE(feed.entries.front().sticky);
    CHECK(feed.entries.front().content.topic == NotificationTopic::PlaybackSequence);
  }

  TEST_CASE("PlaybackSequenceService - launch spec remains detached from later view edits and destruction",
            "[runtime][unit][playback-sequence][launch]")
  {
    auto fixture = PlaybackSequenceFixture{};
    fixture.buildThreeTrackManualView(TrackListViewConfig{.filterExpression = "$year >= 2000"});
    auto& sequence = *fixture.sequencePtr;
    REQUIRE(sequence.playFromView(fixture.viewId, fixture.secondTrackId));
    auto const captured = sequence.state();
    REQUIRE(captured.optResolvedSuccessor == fixture.thirdTrackId);

    REQUIRE(fixture.views.setFilter(fixture.viewId, "$year < 2000"));
    REQUIRE(fixture.views.setPresentation(
      fixture.viewId,
      TrackPresentationSpec{.id = "reverse-title", .sortBy = {{.field = TrackSortField::Title, .ascending = false}}}));
    REQUIRE(fixture.views.destroyView(fixture.viewId));

    CHECK(sequence.state() == captured);
    CHECK(fixture.playback.state().nowPlaying.trackId == fixture.secondTrackId);

    sequence.next();
    CHECK(sequence.state().currentTrackId == fixture.thirdTrackId);
    CHECK(fixture.playback.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSequenceService - strict launch rejects invalid quick filters and missing sources atomically",
            "[runtime][unit][playback-sequence][launch]")
  {
    SECTION("invalid captured quick filter")
    {
      auto fixture = PlaybackSequenceFixture{};
      fixture.buildThreeTrackManualView(TrackListViewConfig{.filterExpression = "("});

      auto const result = fixture.sequencePtr->playFromView(fixture.viewId, fixture.firstTrackId);

      REQUIRE_FALSE(result);
      CHECK(fixture.sequencePtr->state().sourceState == PlaybackSequenceSourceState::Inactive);
      CHECK(fixture.playback.state().transport == audio::Transport::Idle);
    }

    SECTION("source deleted after view capture")
    {
      auto fixture = PlaybackSequenceFixture{};
      fixture.buildThreeTrackManualView();
      REQUIRE(fixture.writer.deleteList(fixture.listId));

      auto const result = fixture.sequencePtr->playFromView(fixture.viewId, fixture.firstTrackId);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
      CHECK(fixture.sequencePtr->state().sourceState == PlaybackSequenceSourceState::Inactive);
      CHECK(fixture.playback.state().transport == audio::Transport::Idle);
    }
  }

  TEST_CASE("PlaybackSequenceService - manual reorder updates only the live semantic tuple",
            "[runtime][unit][playback-sequence][projection]")
  {
    auto fixture = PlaybackSequenceFixture{};
    fixture.buildThreeTrackManualView();
    auto& sequence = *fixture.sequencePtr;
    REQUIRE(sequence.playFromView(fixture.viewId, fixture.firstTrackId));
    std::uint32_t changedCount = 0;
    auto const subscription = sequence.onChanged([&](PlaybackSequenceState const&) { ++changedCount; });
    auto const beforeMove = sequence.state();

    auto const moved =
      fixture.writer.moveManualListTracks(fixture.listId, std::array{fixture.thirdTrackId}, std::size_t{1});
    REQUIRE(moved);
    REQUIRE(moved->changed);

    CHECK(sequence.state().currentTrackId == fixture.firstTrackId);
    CHECK(sequence.state().optResolvedSuccessor == fixture.thirdTrackId);
    CHECK(sequence.state().semanticRevision == beforeMove.semanticRevision + 1);
    CHECK(changedCount == 1);
    CHECK(fixture.playback.state().nowPlaying.trackId == fixture.firstTrackId);
    CHECK(fixture.playback.state().transport == audio::Transport::Playing);

    auto const noOp =
      fixture.writer.moveManualListTracks(fixture.listId, std::array{fixture.thirdTrackId}, std::size_t{1});
    REQUIRE(noOp);
    CHECK_FALSE(noOp->changed);
    CHECK(sequence.state().semanticRevision == beforeMove.semanticRevision + 1);
    CHECK(changedCount == 1);
  }

  TEST_CASE("PlaybackSequenceService - smart membership changes retain current audio and a gap anchor",
            "[runtime][unit][playback-sequence][projection]")
  {
    auto fixture = PlaybackSequenceFixture{};
    fixture.firstTrackId = fixture.addPlayableTrack("First", 1990);
    fixture.secondTrackId = fixture.addPlayableTrack("Second", 2000);
    fixture.thirdTrackId = fixture.addPlayableTrack("Third", 2010);
    fixture.sources.reloadAllTracks();
    fixture.listId = ao::test::requireValue(fixture.writer.createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Smart,
      .name = "Recent",
      .expression = "$year >= 2000",
    }));
    fixture.viewId =
      ao::test::requireValue(
        fixture.views.createView(
          TrackListViewConfig{
            .listId = fixture.listId,
            .optPresentation = TrackPresentationSpec{.id = "year-order",
                                                     .sortBy = {{.field = TrackSortField::Year, .ascending = true}}}},
          true))
        .viewId;
    fixture.sequencePtr = std::make_unique<PlaybackSequenceService>(fixture.executor,
                                                                    fixture.views,
                                                                    fixture.sources,
                                                                    fixture.libraryFixture.library(),
                                                                    fixture.playback,
                                                                    fixture.notifications,
                                                                    fixture.asyncRuntime);
    auto& sequence = *fixture.sequencePtr;
    REQUIRE(sequence.playFromView(fixture.viewId, fixture.secondTrackId));
    auto const beforeAddition = sequence.state();
    REQUIRE(beforeAddition.optResolvedSuccessor == fixture.thirdTrackId);

    REQUIRE(
      fixture.writer.updateMetadata(std::array{fixture.firstTrackId}, MetadataPatch{.optYear = std::uint16_t{2005}}));
    auto const afterAddition = sequence.state();
    CHECK(afterAddition.currentTrackId == fixture.secondTrackId);
    CHECK(afterAddition.optResolvedSuccessor == fixture.firstTrackId);
    CHECK(afterAddition.semanticRevision == beforeAddition.semanticRevision + 1);
    CHECK(fixture.playback.state().nowPlaying.trackId == fixture.secondTrackId);

    REQUIRE(
      fixture.writer.updateMetadata(std::array{fixture.secondTrackId}, MetadataPatch{.optYear = std::uint16_t{1990}}));
    auto const currentRemoved = sequence.state();
    CHECK(currentRemoved.sourceState == PlaybackSequenceSourceState::Live);
    CHECK(currentRemoved.currentTrackId == fixture.secondTrackId);
    CHECK(currentRemoved.optResolvedSuccessor == fixture.firstTrackId);
    CHECK(currentRemoved.semanticRevision == afterAddition.semanticRevision);
    CHECK(fixture.playback.state().nowPlaying.trackId == fixture.secondTrackId);
    CHECK(fixture.playback.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSequenceService - empty live repeat-one remains navigable until invalidation",
            "[runtime][unit][playback-sequence][repeat]")
  {
    auto fixture = PlaybackSequenceFixture{};
    fixture.firstTrackId = fixture.addPlayableTrack("Only");
    fixture.openManualView(std::array{fixture.firstTrackId});
    auto& sequence = *fixture.sequencePtr;
    REQUIRE(sequence.playFromView(fixture.viewId, fixture.firstTrackId));
    sequence.setRepeatMode(RepeatMode::One);

    REQUIRE(fixture.writer.removeManualListTracks(fixture.listId, std::array{fixture.firstTrackId}));
    auto const emptyLive = sequence.state();
    CHECK(emptyLive.sourceState == PlaybackSequenceSourceState::Live);
    CHECK(emptyLive.hasNext);
    CHECK(emptyLive.optResolvedSuccessor == fixture.firstTrackId);
    CHECK(fixture.playback.state().transport == audio::Transport::Playing);

    REQUIRE(fixture.writer.deleteList(fixture.listId));
    auto const invalidated = sequence.state();
    CHECK(invalidated.sourceState == PlaybackSequenceSourceState::Invalidated);
    CHECK_FALSE(invalidated.hasNext);
    CHECK_FALSE(invalidated.optResolvedSuccessor);
    CHECK(fixture.playback.state().transport == audio::Transport::Playing);

    sequence.next();
    CHECK(sequence.state().sourceState == PlaybackSequenceSourceState::Inactive);
    CHECK(fixture.playback.state().transport == audio::Transport::Idle);

    auto const feed = fixture.notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().message == "Playback sequence finished");
    CHECK(feed.entries.front().severity == NotificationSeverity::Info);
    CHECK_FALSE(feed.entries.front().sticky);
    CHECK(feed.entries.front().content.topic == NotificationTopic::PlaybackSequence);
  }

  TEST_CASE("PlaybackSequenceService - empty live source has no successor with repeat off or all",
            "[runtime][unit][playback-sequence][repeat]")
  {
    auto fixture = PlaybackSequenceFixture{};
    fixture.firstTrackId = fixture.addPlayableTrack("Only");
    fixture.openManualView(std::array{fixture.firstTrackId});
    auto& sequence = *fixture.sequencePtr;
    REQUIRE(sequence.playFromView(fixture.viewId, fixture.firstTrackId));

    SECTION("repeat off")
    {
      REQUIRE(sequence.state().repeat == RepeatMode::Off);
    }

    SECTION("repeat all")
    {
      sequence.setRepeatMode(RepeatMode::All);
    }

    REQUIRE(fixture.writer.removeManualListTracks(fixture.listId, std::array{fixture.firstTrackId}));
    CHECK(sequence.state().sourceState == PlaybackSequenceSourceState::Live);
    CHECK_FALSE(sequence.hasNext());
    CHECK_FALSE(sequence.state().hasNext);
    CHECK_FALSE(sequence.state().optResolvedSuccessor);
    CHECK(fixture.playback.state().transport == audio::Transport::Playing);

    sequence.next();
    CHECK(sequence.state().sourceState == PlaybackSequenceSourceState::Inactive);
    CHECK(fixture.playback.state().transport == audio::Transport::Idle);

    auto const feed = fixture.notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().message == "Playback sequence finished");
    CHECK(feed.entries.front().severity == NotificationSeverity::Info);
    CHECK_FALSE(feed.entries.front().sticky);
    CHECK(feed.entries.front().content.topic == NotificationTopic::PlaybackSequence);
  }

  TEST_CASE("PlaybackSequenceService - idle fallback advances without a prepared successor",
            "[runtime][regression][playback-sequence]")
  {
    auto fixture = PlaybackSequenceTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto& playback = fixture.transport.playbackService;
    REQUIRE(fixture.sequencePtr->playFromView(fixture.viewId, fixture.firstTrackId));
    fixture.transport.executor.drain();
    REQUIRE(playback.clearPreparedNext());
    REQUIRE(fixture.transport.renderTarget != nullptr);

    auto output = std::array<std::byte, 4096>{};
    bool drained = false;

    for (std::int32_t attempt = 0; attempt < 100000 && !drained; ++attempt)
    {
      drained = fixture.transport.renderTarget->renderPcm(output).drained;
    }

    REQUIRE(drained);
    fixture.transport.renderTarget->handleDrainComplete();
    fixture.transport.executor.checkQueued(std::chrono::seconds{5});
    fixture.transport.executor.drain();

    CHECK(fixture.sequencePtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(playback.state().nowPlaying.trackId == fixture.secondTrackId);
    CHECK(playback.state().transport == audio::Transport::Playing);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSequenceService - output and final seek replace the disarmed lookahead token",
            "[runtime][unit][playback-sequence][token]")
  {
    auto fixture = PlaybackSequenceTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto& playback = fixture.transport.playbackService;
    REQUIRE(fixture.sequencePtr->playFromView(fixture.viewId, fixture.firstTrackId));

    auto const optFirstToken = playback.clearPreparedNext();
    REQUIRE(optFirstToken);

    SECTION("output edge")
    {
      playback.setOutputDevice(
        audio::BackendId{"mock_backend"}, audio::DeviceId{"mock_device"}, audio::ProfileId{audio::kProfileShared});
    }

    SECTION("final seek edge")
    {
      playback.seek(std::chrono::milliseconds{0}, PlaybackService::SeekMode::Final);
    }

    auto const optReplacementToken = playback.clearPreparedNext();
    REQUIRE(optReplacementToken);
    CHECK(*optReplacementToken != *optFirstToken);
    CHECK(fixture.sequencePtr->state().currentTrackId == fixture.firstTrackId);
    CHECK(playback.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSequenceService - natural prepared winner is adopted exactly once",
            "[runtime][unit][playback-sequence][token]")
  {
    auto fixture = PlaybackSequenceTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto events = std::vector<PlaybackService::NowPlayingChanged>{};
    auto const subscription = fixture.transport.playbackService.onNowPlayingChanged(
      [&](PlaybackService::NowPlayingChanged const& event) { events.push_back(event); });
    REQUIRE(fixture.sequencePtr->playFromView(fixture.viewId, fixture.firstTrackId));
    fixture.transport.executor.drain();
    events.clear();
    auto const revisionBeforeAdvance = fixture.sequencePtr->state().semanticRevision;

    fixture.queueNaturalAdvance();
    fixture.transport.executor.drain();

    REQUIRE(events.size() == 1);
    REQUIRE(events.front().optPreparedNextToken);
    CHECK(events.front().trackId == fixture.secondTrackId);
    CHECK(fixture.sequencePtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(fixture.sequencePtr->state().semanticRevision == revisionBeforeAdvance + 1);
    CHECK(fixture.transport.playbackService.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSequenceService - retired prepared winner survives a final-seek race",
            "[runtime][unit][playback-sequence][token]")
  {
    auto fixture = PlaybackSequenceTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto events = std::vector<PlaybackService::NowPlayingChanged>{};
    auto const subscription = fixture.transport.playbackService.onNowPlayingChanged(
      [&](PlaybackService::NowPlayingChanged const& event) { events.push_back(event); });
    REQUIRE(fixture.sequencePtr->playFromView(fixture.viewId, fixture.firstTrackId));
    fixture.transport.executor.drain();
    events.clear();

    fixture.queueNaturalAdvance();
    fixture.transport.playbackService.seek(std::chrono::milliseconds{0}, PlaybackService::SeekMode::Final);
    fixture.transport.executor.drain();

    REQUIRE(events.size() == 1);
    REQUIRE(events.front().optPreparedNextToken);
    CHECK(events.front().trackId == fixture.secondTrackId);
    CHECK(fixture.sequencePtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(fixture.sequencePtr->state().optResolvedSuccessor == fixture.thirdTrackId);
    CHECK(fixture.transport.playbackService.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSequenceService - explicit session replacement rejects the queued stale advance",
            "[runtime][unit][playback-sequence][token]")
  {
    auto fixture = PlaybackSequenceTransportFixture{};
    fixture.buildThreeTrackManualView();
    REQUIRE(fixture.sequencePtr->playFromView(fixture.viewId, fixture.firstTrackId));
    fixture.queueNaturalAdvance();

    REQUIRE(fixture.sequencePtr->playFromView(fixture.viewId, fixture.thirdTrackId));
    auto const replacement = fixture.sequencePtr->state();
    fixture.transport.executor.drain();

    CHECK(fixture.sequencePtr->state() == replacement);
    CHECK(fixture.sequencePtr->state().currentTrackId == fixture.thirdTrackId);
    CHECK(fixture.transport.playbackService.state().nowPlaying.trackId == fixture.thirdTrackId);
    CHECK(fixture.transport.playbackService.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSequenceService - destruction disconnects a queued natural-advance callback",
            "[runtime][unit][playback-sequence][lifecycle]")
  {
    auto fixture = PlaybackSequenceTransportFixture{};
    fixture.buildThreeTrackManualView();
    std::uint32_t changedCount = 0;
    auto const subscription = fixture.sequencePtr->onChanged([&](PlaybackSequenceState const&) { ++changedCount; });
    REQUIRE(fixture.sequencePtr->playFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(changedCount == 1);
    fixture.queueNaturalAdvance();

    fixture.sequencePtr.reset();
    fixture.transport.executor.drain();

    CHECK(changedCount == 1);
    CHECK(fixture.transport.playbackService.state().nowPlaying.trackId == fixture.secondTrackId);
  }

  TEST_CASE("PlaybackSequenceService - navigation stops after three consecutive unplayable candidates",
            "[runtime][unit][playback-sequence][failure]")
  {
    auto fixture = PlaybackSequenceFixture{};
    auto const playable = fixture.addPlayableTrack("Current");
    auto const brokenOne = fixture.libraryFixture.addTrack(
      library::test::TrackSpec{.title = "Broken one", .uri = "/missing/one.flac", .codec = AudioCodec::Flac});
    auto const brokenTwo = fixture.libraryFixture.addTrack(
      library::test::TrackSpec{.title = "Broken two", .uri = "/missing/two.flac", .codec = AudioCodec::Flac});
    auto const brokenThree = fixture.libraryFixture.addTrack(
      library::test::TrackSpec{.title = "Broken three", .uri = "/missing/three.flac", .codec = AudioCodec::Flac});
    auto const unreachable = fixture.addPlayableTrack("Unreachable");
    fixture.openManualView(std::array{playable, brokenOne, brokenTwo, brokenThree, unreachable});
    REQUIRE(fixture.sequencePtr->playFromView(fixture.viewId, playable));

    fixture.sequencePtr->next();

    CHECK(fixture.sequencePtr->state().sourceState == PlaybackSequenceSourceState::Inactive);
    CHECK(fixture.playback.state().transport == audio::Transport::Idle);
    CHECK(fixture.playback.state().nowPlaying.trackId != unreachable);
    auto const feed = fixture.notifications.feed();
    NotificationEntry const* skipSummary = nullptr;
    NotificationEntry const* failureLimit = nullptr;

    for (auto const& entry : feed.entries)
    {
      if (entry.message == "Skipped 3 unplayable tracks")
      {
        skipSummary = &entry;
      }

      if (entry.message == "Playback stopped after 3 unplayable tracks")
      {
        failureLimit = &entry;
      }
    }

    REQUIRE(skipSummary != nullptr);
    CHECK(skipSummary->severity == NotificationSeverity::Warning);
    CHECK_FALSE(skipSummary->sticky);
    CHECK(skipSummary->content.topic == NotificationTopic::PlaybackSequence);
    REQUIRE(failureLimit != nullptr);
    CHECK(failureLimit->severity == NotificationSeverity::Error);
    CHECK(failureLimit->sticky);
    CHECK(failureLimit->content.topic == NotificationTopic::PlaybackSequence);
  }

  TEST_CASE("PlaybackSequenceService - non-recoverable device failure terminates the active session",
            "[runtime][unit][playback-sequence][failure]")
  {
    auto fixture = PlaybackSequenceTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto failures = std::vector<PlaybackFailure>{};
    auto const subscription = fixture.transport.playbackService.onPlaybackFailure([&](PlaybackFailure const& failure)
                                                                                  { failures.push_back(failure); });
    REQUIRE(fixture.sequencePtr->playFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(fixture.transport.renderTarget != nullptr);

    fixture.transport.renderTarget->handleBackendError("device lost during sequence playback");
    REQUIRE(fixture.transport.executor.drainUntil([&] { return !failures.empty(); }));

    REQUIRE(failures.size() == 1);
    CHECK(failures.front().kind == PlaybackFailureKind::DeviceLost);
    CHECK_FALSE(failures.front().recoverable);
    CHECK(failures.front().disposition == PlaybackFailureDisposition::Stopped);
    CHECK(fixture.sequencePtr->state().sourceState == PlaybackSequenceSourceState::Inactive);
    CHECK(fixture.transport.playbackService.state().transport == audio::Transport::Idle);

    auto const feed = fixture.transport.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
    CHECK(feed.entries.front().content.topic == NotificationTopic::PlaybackError);
  }

  TEST_CASE("PlaybackSequenceService - track failure on an invalidated source posts one terminal sequence error",
            "[runtime][unit][playback-sequence][concurrency]")
  {
    auto failureGate = audio::test::StagedFailureGate{};
    auto fixture = PlaybackSequenceSeekFixture{&failureGate};
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    fixture.buildSingleTrackManualView();
    auto failures = std::vector<PlaybackFailure>{};
    auto const subscription =
      fixture.playbackPtr->onPlaybackFailure([&](PlaybackFailure const& failure) { failures.push_back(failure); });
    REQUIRE(fixture.sequencePtr->playFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(failureGate.waitForRead());

    REQUIRE(fixture.writer.deleteList(fixture.listId));
    REQUIRE(fixture.sequencePtr->state().sourceState == PlaybackSequenceSourceState::Invalidated);
    releaseGuard.release();
    REQUIRE(fixture.executor.drainUntil([&] { return !failures.empty(); }));

    REQUIRE(failures.size() == 1);
    CHECK(failures.front().kind == PlaybackFailureKind::Decode);
    CHECK(failures.front().disposition == PlaybackFailureDisposition::Stopped);
    CHECK(fixture.sequencePtr->state().sourceState == PlaybackSequenceSourceState::Inactive);

    auto const feed = fixture.notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
    CHECK(feed.entries.front().sticky);
    CHECK(feed.entries.front().content.topic == NotificationTopic::PlaybackSequence);
    CHECK(feed.entries.front().message.contains("Failing current"));
    CHECK(feed.entries.front().message.contains("gated staged decode failure"));
  }

  TEST_CASE("PlaybackSequenceService - previous restart uses a strict greater-than three-second final seek",
            "[runtime][unit][playback-sequence][previous]")
  {
    auto fixture = PlaybackSequenceSeekFixture{};
    fixture.buildThreeTrackManualView();
    auto& sequence = *fixture.sequencePtr;
    auto& playback = *fixture.playbackPtr;
    REQUIRE(sequence.playFromView(fixture.viewId, fixture.firstTrackId));
    fixture.executor.drain();
    playback.pause();
    REQUIRE(playback.state().transport == audio::Transport::Paused);

    playback.seek(std::chrono::milliseconds{3000}, PlaybackService::SeekMode::Final);
    CHECK(playback.elapsed() == std::chrono::milliseconds{3000});
    CHECK_FALSE(sequence.hasPrevious());
    CHECK_FALSE(sequence.state().hasPrevious);

    auto const revisionAtBoundary = sequence.state().semanticRevision;
    playback.seek(std::chrono::milliseconds{3001}, PlaybackService::SeekMode::Final);
    CHECK(playback.elapsed() == std::chrono::milliseconds{3001});
    CHECK(sequence.hasPrevious());
    CHECK(sequence.state().hasPrevious);
    CHECK(sequence.state().semanticRevision == revisionAtBoundary + 1);

    sequence.previous();
    CHECK(sequence.state().currentTrackId == fixture.firstTrackId);
    CHECK_FALSE(sequence.hasPrevious());
    CHECK_FALSE(sequence.state().hasPrevious);
    CHECK(playback.elapsed() == std::chrono::milliseconds{0});
    CHECK(playback.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSequenceService - commands and dedicated mode signals follow cursor resolution",
            "[runtime][unit][playback-sequence][command]")
  {
    auto fixture = PlaybackSequenceFixture{};
    fixture.buildThreeTrackManualView();
    auto& sequence = *fixture.sequencePtr;
    REQUIRE(sequence.playFromView(fixture.viewId, fixture.firstTrackId));

    std::uint32_t shuffleEvents = 0;
    std::uint32_t repeatEvents = 0;
    auto const shuffleSubscription =
      sequence.onShuffleModeChanged([&](PlaybackSequenceService::ShuffleModeChanged const&) { ++shuffleEvents; });
    auto const repeatSubscription =
      sequence.onRepeatModeChanged([&](PlaybackSequenceService::RepeatModeChanged const&) { ++repeatEvents; });

    sequence.next();
    CHECK(sequence.state().currentTrackId == fixture.secondTrackId);
    CHECK(sequence.hasPrevious());

    sequence.previous();
    CHECK(sequence.state().currentTrackId == fixture.firstTrackId);

    auto const revisionBeforeRepeat = sequence.state().semanticRevision;
    sequence.setRepeatMode(RepeatMode::One);
    CHECK(sequence.state().repeat == RepeatMode::One);
    CHECK(sequence.state().optResolvedSuccessor == fixture.firstTrackId);
    CHECK(sequence.state().semanticRevision > revisionBeforeRepeat);
    CHECK(repeatEvents == 1);
    sequence.setRepeatMode(RepeatMode::One);
    CHECK(repeatEvents == 1);

    sequence.setShuffleMode(ShuffleMode::On);
    CHECK(sequence.state().shuffle == ShuffleMode::On);
    CHECK(shuffleEvents == 1);

    sequence.clear();
    CHECK(sequence.state().sourceState == PlaybackSequenceSourceState::Inactive);
    CHECK(sequence.state().currentTrackId == kInvalidTrackId);
    CHECK(fixture.playback.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSequenceService - shuffle failure walks preserve shuffle direction and semantic parity",
            "[runtime][unit][playback-sequence][shuffle]")
  {
    auto fixture = PlaybackSequenceFixture{};
    fixture.buildThreeTrackManualView();
    auto& sequence = *fixture.sequencePtr;
    REQUIRE(sequence.playFromView(fixture.viewId, fixture.firstTrackId));
    sequence.setShuffleMode(ShuffleMode::On);

    SECTION("failed forward candidate is excluded before the sticky candidate is re-resolved")
    {
      auto const optFailedCandidate = sequence.state().optResolvedSuccessor;
      REQUIRE(optFailedCandidate);
      library::test::updateTrackSpec(fixture.libraryFixture.library(),
                                     *optFailedCandidate,
                                     [](library::test::TrackSpec& spec)
                                     { spec.uri = "/missing/shuffle-forward.flac"; });

      sequence.next();

      CHECK(sequence.state().currentTrackId != fixture.firstTrackId);
      CHECK(sequence.state().currentTrackId != *optFailedCandidate);
      CHECK(fixture.playback.state().transport == audio::Transport::Playing);
    }

    SECTION("failed history previous does not fall through to sequential previous")
    {
      sequence.next();
      auto const currentTrackId = sequence.state().currentTrackId;
      REQUIRE(currentTrackId != fixture.firstTrackId);
      REQUIRE(sequence.hasPrevious());
      auto const revisionBeforeFailure = sequence.state().semanticRevision;
      library::test::updateTrackSpec(fixture.libraryFixture.library(),
                                     fixture.firstTrackId,
                                     [](library::test::TrackSpec& spec)
                                     { spec.uri = "/missing/shuffle-history.flac"; });

      sequence.previous();

      CHECK(sequence.state().currentTrackId == currentTrackId);
      CHECK_FALSE(sequence.hasPrevious());
      CHECK(sequence.state().semanticRevision > revisionBeforeFailure);
      CHECK(fixture.playback.state().transport == audio::Transport::Playing);
    }
  }
} // namespace ao::rt::test
