// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackSuccession.h"

#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/audio/ScriptedDecoderSession.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/PlaybackTransportTestSupport.h"
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
#include <ao/library/TrackStore.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackMode.h>
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
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct PlaybackSuccessionFixture final
    {
      PlaybackSuccessionFixture()
        : asyncRuntime{executor, 1, {}, &sleeper}
        , writerFixture{libraryFixture.library(), changes}
        , sources{libraryFixture.library(), changes}
        , views{executor, libraryFixture.library(), sources}
        , playbackTransport{makePlaybackTransport(executor, libraryFixture.library(), notifications)}
      {
        PlaybackBootstrap{playbackTransport}.addProvider(makeReadyAudioProvider());
      }

      LibraryWriter& writer() { return writerFixture.writer(); }

      TrackId addPlayableTrack(std::string title, std::uint16_t const year = 2020)
      {
        auto const playableUri = std::format("playable-{}.flac", nextPlayableFile++);
        audio::test::installAudioFixture(libraryFixture.root(), "basic_metadata.flac", playableUri);
        return libraryFixture.addTrack(library::test::TrackSpec{
          .title = std::move(title), .uri = playableUri, .year = year, .codec = AudioCodec::Flac});
      }

      void removePlayableFile(TrackId const trackId)
      {
        auto transaction = libraryFixture.library().readTransaction();
        auto const optView = libraryFixture.library()
                               .tracks()
                               .reader(transaction)
                               .get(trackId, library::TrackStore::Reader::LoadMode::Cold);
        REQUIRE(optView);
        REQUIRE(std::filesystem::remove(libraryFixture.root() / std::filesystem::path{optView->property().uri()}));
      }

      void openManualView(std::span<TrackId const> const trackIds, TrackListViewConfig config = {})
      {
        sources.reloadAllTracks();
        listId = ao::test::requireValue(writer().createList(LibraryWriter::ListDraft{
          .kind = LibraryWriter::ListKind::Manual,
          .name = "Playback order",
          .trackIds = {trackIds.begin(), trackIds.end()},
        }));
        config.listId = listId;
        viewId = ao::test::requireValue(views.createView(config));
        successionPtr = std::make_unique<PlaybackSuccession>(
          executor, views, sources, libraryFixture.library(), playbackTransport, notifications, asyncRuntime);
      }

      void buildThreeTrackManualView(TrackListViewConfig config = {})
      {
        firstTrackId = addPlayableTrack("First", 1990);
        secondTrackId = addPlayableTrack("Second", 2000);
        thirdTrackId = addPlayableTrack("Third", 2010);
        openManualView(std::array{firstTrackId, secondTrackId, thirdTrackId}, std::move(config));
      }

      MusicLibraryFixture libraryFixture;
      ControlledSleeper sleeper;
      InlineExecutor executor;
      async::Runtime asyncRuntime;
      LibraryChanges changes;
      LibraryWriterFixture writerFixture;
      TrackSourceCache sources;
      ViewService views;
      NotificationService notifications{asyncRuntime};
      PlaybackTransport playbackTransport;
      std::unique_ptr<PlaybackSuccession> successionPtr;
      TrackId firstTrackId = kInvalidTrackId;
      TrackId secondTrackId = kInvalidTrackId;
      TrackId thirdTrackId = kInvalidTrackId;
      ListId listId = kInvalidListId;
      ViewId viewId = kInvalidViewId;
      std::uint32_t nextPlayableFile = 0;
    };

    struct PlaybackSuccessionTransportFixture final
    {
      PlaybackSuccessionTransportFixture()
        : asyncRuntime{transport.executor, 1, {}, &sleeper}
        , writerFixture{transport.libraryFixture.library(), changes}
        , sources{transport.libraryFixture.library(), changes}
        , views{transport.executor, transport.libraryFixture.library(), sources}
      {
        transport.onDevicesChangedCb(transport.status.devices);
        transport.executor.drain();
      }

      LibraryWriter& writer() { return writerFixture.writer(); }

      TrackId addPlayableTrack(std::string title)
      {
        auto const fixtureUri = audio::test::installAudioFixture(
          transport.libraryFixture.root(), "basic_metadata.flac", "transport-playable.flac");
        return transport.libraryFixture.addTrack(
          library::test::TrackSpec{.title = std::move(title), .uri = fixtureUri, .codec = AudioCodec::Flac});
      }

      void buildThreeTrackManualView()
      {
        firstTrackId = addPlayableTrack("First");
        secondTrackId = addPlayableTrack("Second");
        thirdTrackId = addPlayableTrack("Third");
        sources.reloadAllTracks();
        listId = ao::test::requireValue(writer().createList(LibraryWriter::ListDraft{
          .kind = LibraryWriter::ListKind::Manual,
          .name = "Transport order",
          .trackIds = {firstTrackId, secondTrackId, thirdTrackId},
        }));
        viewId = ao::test::requireValue(views.createView({.listId = listId}));
        successionPtr = std::make_unique<PlaybackSuccession>(transport.executor,
                                                             views,
                                                             sources,
                                                             transport.libraryFixture.library(),
                                                             transport.playbackTransport,
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

      PlaybackTransportFixture<QueuedExecutor> transport;
      ControlledSleeper sleeper;
      async::Runtime asyncRuntime;
      LibraryChanges changes;
      LibraryWriterFixture writerFixture;
      TrackSourceCache sources;
      ViewService views;
      std::unique_ptr<PlaybackSuccession> successionPtr;
      TrackId firstTrackId = kInvalidTrackId;
      TrackId secondTrackId = kInvalidTrackId;
      TrackId thirdTrackId = kInvalidTrackId;
      ListId listId = kInvalidListId;
      ViewId viewId = kInvalidViewId;
    };

    struct PlaybackSuccessionSeekFixture final
    {
      explicit PlaybackSuccessionSeekFixture(audio::test::StagedFailureGate* const failureGate = nullptr)
        : asyncRuntime{executor}
        , writerFixture{libraryFixture.library(), changes}
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
        transportPtr =
          std::make_unique<PlaybackTransport>(executor, libraryFixture.library(), notifications, std::move(playerPtr));
        PlaybackBootstrap{*transportPtr}.addProvider(makeReadyAudioProvider());
        executor.drain();
      }

      LibraryWriter& writer() { return writerFixture.writer(); }

      void buildThreeTrackManualView()
      {
        firstTrackId = libraryFixture.addTrack(library::test::TrackSpec{
          .title = "First", .uri = "first.flac", .duration = std::chrono::seconds{10}, .codec = AudioCodec::Flac});
        secondTrackId = libraryFixture.addTrack(library::test::TrackSpec{
          .title = "Second", .uri = "second.flac", .duration = std::chrono::seconds{10}, .codec = AudioCodec::Flac});
        thirdTrackId = libraryFixture.addTrack(library::test::TrackSpec{
          .title = "Third", .uri = "third.flac", .duration = std::chrono::seconds{10}, .codec = AudioCodec::Flac});
        sources.reloadAllTracks();
        listId = ao::test::requireValue(writer().createList(LibraryWriter::ListDraft{
          .kind = LibraryWriter::ListKind::Manual,
          .name = "Long playback order",
          .trackIds = {firstTrackId, secondTrackId, thirdTrackId},
        }));
        viewId = ao::test::requireValue(views.createView({.listId = listId}));
        successionPtr = std::make_unique<PlaybackSuccession>(
          executor, views, sources, libraryFixture.library(), *transportPtr, notifications, asyncRuntime);
      }

      void buildSingleTrackManualView()
      {
        firstTrackId = libraryFixture.addTrack(library::test::TrackSpec{
          .title = "Failing current", .uri = "failing-current.flac", .codec = AudioCodec::Flac});
        sources.reloadAllTracks();
        listId = ao::test::requireValue(writer().createList(LibraryWriter::ListDraft{
          .kind = LibraryWriter::ListKind::Manual,
          .name = "Failing playback order",
          .trackIds = {firstTrackId},
        }));
        viewId = ao::test::requireValue(views.createView({.listId = listId}));
        successionPtr = std::make_unique<PlaybackSuccession>(
          executor, views, sources, libraryFixture.library(), *transportPtr, notifications, asyncRuntime);
      }

      MusicLibraryFixture libraryFixture;
      QueuedExecutor executor;
      async::Runtime asyncRuntime;
      LibraryChanges changes;
      LibraryWriterFixture writerFixture;
      TrackSourceCache sources;
      ViewService views;
      NotificationService notifications{asyncRuntime};
      std::unique_ptr<PlaybackTransport> transportPtr;
      std::unique_ptr<PlaybackSuccession> successionPtr;
      TrackId firstTrackId = kInvalidTrackId;
      TrackId secondTrackId = kInvalidTrackId;
      TrackId thirdTrackId = kInvalidTrackId;
      ListId listId = kInvalidListId;
      ViewId viewId = kInvalidViewId;
    };
  } // namespace

  TEST_CASE("PlaybackSuccession - strict launch commits only a validated captured spec",
            "[runtime][unit][playback-succession][launch]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    auto const outsideTrackId = fixture.addPlayableTrack("Outside");
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    std::uint32_t changedCount = 0;
    auto const changedSubscription = succession.onChanged([&](PlaybackSuccessionState const&) { ++changedCount; });

    REQUIRE(succession.playFromView(fixture.viewId, fixture.firstTrackId));
    auto const accepted = succession.state();
    CHECK(accepted.sourceState == PlaybackSuccessionSourceState::Live);
    CHECK(accepted.currentTrackId == fixture.firstTrackId);
    CHECK(accepted.sourceListId == fixture.listId);
    CHECK(accepted.hasNext);
    CHECK_FALSE(accepted.hasPrevious);
    CHECK(accepted.optResolvedSuccessor == fixture.secondTrackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(changedCount == 1);

    SECTION("unknown view")
    {
      auto const rejected = succession.playFromView(ViewId{999999}, fixture.secondTrackId);
      REQUIRE_FALSE(rejected);
      CHECK(rejected.error().code == Error::Code::NotFound);
    }

    SECTION("start absent from captured projection")
    {
      auto const rejected = succession.playFromView(fixture.viewId, outsideTrackId);
      REQUIRE_FALSE(rejected);
      CHECK(rejected.error().code == Error::Code::NotFound);
    }

    CHECK(succession.state() == accepted);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == fixture.firstTrackId);
    CHECK(changedCount == 1);
  }

  TEST_CASE("PlaybackSuccession - staging rejection preserves the accepted session and transport",
            "[runtime][regression][playback-succession][launch]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    auto const current = fixture.addPlayableTrack("Current");
    auto const successor = fixture.addPlayableTrack("Successor");
    auto const broken = fixture.libraryFixture.addTrack(
      library::test::TrackSpec{.title = "Broken", .uri = "missing/relaunch.flac", .codec = AudioCodec::Flac});
    fixture.openManualView(std::array{current, successor, broken});
    auto& succession = *fixture.successionPtr;

    REQUIRE(succession.playFromView(fixture.viewId, current));
    auto const sequenceBeforeRejection = succession.state();
    auto const transportBeforeRejection = fixture.playbackTransport.state();
    REQUIRE(sequenceBeforeRejection.optResolvedSuccessor == successor);
    std::uint32_t changedCount = 0;
    auto const changedSubscription = succession.onChanged([&](PlaybackSuccessionState const&) { ++changedCount; });

    auto const rejected = succession.playFromView(fixture.viewId, broken);

    REQUIRE_FALSE(rejected);
    auto const rejectionFeed = fixture.notifications.feed();
    REQUIRE(rejectionFeed.entries.size() == 1);
    CHECK(rejectionFeed.entries.front().severity == NotificationSeverity::Error);
    CHECK(rejectionFeed.entries.front().lifetime == NotificationLifetime::pinned());
    CHECK(succession.state() == sequenceBeforeRejection);
    CHECK(changedCount == 0);

    auto const transportAfterRejection = fixture.playbackTransport.state();
    CHECK(transportAfterRejection.transport == transportBeforeRejection.transport);
    CHECK(transportAfterRejection.elapsed == transportBeforeRejection.elapsed);
    CHECK(transportAfterRejection.duration == transportBeforeRejection.duration);
    CHECK(transportAfterRejection.ready == transportBeforeRejection.ready);
    CHECK(transportAfterRejection.nowPlaying == transportBeforeRejection.nowPlaying);
    CHECK(transportAfterRejection.volume == transportBeforeRejection.volume);
    CHECK(transportAfterRejection.output == transportBeforeRejection.output);
    CHECK(transportAfterRejection.quality == transportBeforeRejection.quality);
    CHECK(transportAfterRejection.revision == transportBeforeRejection.revision);

    succession.next();

    CHECK(succession.state().currentTrackId == successor);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == successor);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - accepted launch contains observer exceptions and completes publication",
            "[runtime][regression][playback-succession][launch]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    auto& playbackTransport = fixture.playbackTransport;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.firstTrackId));

    bool changedObserverEntered = false;
    auto trailingObserverTrackId = kInvalidTrackId;
    auto changedSubscription = succession.onChanged(
      [&](PlaybackSuccessionState const&)
      {
        changedObserverEntered = true;
        throwException<Exception>("scripted accepted succession observer failure");
      });
    auto trailingSubscription = succession.onChanged([&](PlaybackSuccessionState const& state)
                                                     { trailingObserverTrackId = state.currentTrackId; });

    auto const launched = succession.playFromView(fixture.viewId, fixture.thirdTrackId);

    REQUIRE(launched);
    CHECK(changedObserverEntered);
    CHECK(trailingObserverTrackId == fixture.thirdTrackId);
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Live);
    CHECK(succession.state().currentTrackId == fixture.thirdTrackId);
    CHECK(succession.state().shuffle == ShuffleMode::Off);
    CHECK(succession.state().repeat == RepeatMode::Off);
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(playbackTransport.state().nowPlaying.trackId == succession.state().currentTrackId);

    changedSubscription.reset();
    trailingSubscription.reset();
    succession.previous();

    CHECK(succession.state().currentTrackId == fixture.secondTrackId);
    CHECK(playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - live membership governs succession without interrupting current audio",
            "[runtime][unit][playback-succession][projection]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.firstTrackId));
    auto const beforeRemoval = succession.state();

    auto const removed = fixture.writer().removeManualListTracks(fixture.listId, std::array{fixture.firstTrackId});
    REQUIRE(removed);
    REQUIRE(removed->changed);

    auto const afterRemoval = succession.state();
    CHECK(afterRemoval.currentTrackId == fixture.firstTrackId);
    CHECK(afterRemoval.sourceState == PlaybackSuccessionSourceState::Live);
    CHECK(afterRemoval.optResolvedSuccessor == fixture.secondTrackId);
    CHECK(afterRemoval == beforeRemoval);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == fixture.firstTrackId);

    REQUIRE(fixture.writer().deleteList(fixture.listId));
    auto const invalidated = succession.state();
    CHECK(invalidated.sourceState == PlaybackSuccessionSourceState::Invalidated);
    CHECK(invalidated.currentTrackId == fixture.firstTrackId);
    CHECK_FALSE(invalidated.hasNext);
    CHECK_FALSE(invalidated.optResolvedSuccessor);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);

    auto const rejectedRelaunch = succession.playFromView(fixture.viewId, fixture.firstTrackId);
    REQUIRE_FALSE(rejectedRelaunch);
    CHECK(rejectedRelaunch.error().code == Error::Code::NotFound);
    CHECK(succession.state() == invalidated);

    succession.next();
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);

    auto const feed = fixture.notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    CHECK(std::get<NotificationReport>(feed.entries.front().message).templateId ==
          NotificationReportTemplate::PlaybackSequenceFinished);
    CHECK(feed.entries.front().severity == NotificationSeverity::Info);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::transient());
  }

  TEST_CASE("PlaybackSuccession - launch spec remains detached from later view edits and destruction",
            "[runtime][unit][playback-succession][launch]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView(TrackListViewConfig{.filterExpression = "$year >= 2000"});
    auto& succession = *fixture.successionPtr;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.secondTrackId));
    auto const captured = succession.state();
    REQUIRE(captured.optResolvedSuccessor == fixture.thirdTrackId);

    REQUIRE(fixture.views.setFilter(fixture.viewId, "$year < 2000"));
    REQUIRE(fixture.views.setPresentation(
      fixture.viewId,
      TrackPresentationSpec{.id = "reverse-title", .sortBy = {{.field = TrackSortField::Title, .ascending = false}}}));
    REQUIRE(fixture.views.destroyView(fixture.viewId));

    CHECK(succession.state() == captured);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);

    succession.next();
    CHECK(succession.state().currentTrackId == fixture.thirdTrackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - strict launch rejects invalid quick filters and missing sources atomically",
            "[runtime][unit][playback-succession][launch]")
  {
    SECTION("invalid captured quick filter")
    {
      auto fixture = PlaybackSuccessionFixture{};
      fixture.buildThreeTrackManualView(TrackListViewConfig{.filterExpression = "("});

      auto const result = fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId);

      REQUIRE_FALSE(result);
      CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
      CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);
    }

    SECTION("source deleted after view capture")
    {
      auto fixture = PlaybackSuccessionFixture{};
      fixture.buildThreeTrackManualView();
      REQUIRE(fixture.writer().deleteList(fixture.listId));

      auto const result = fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
      CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
      CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);
    }
  }

  TEST_CASE("PlaybackSuccession - manual reorder updates only the live semantic tuple",
            "[runtime][unit][playback-succession][projection]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.firstTrackId));
    std::uint32_t changedCount = 0;
    auto const subscription = succession.onChanged([&](PlaybackSuccessionState const&) { ++changedCount; });
    auto const beforeMove = succession.state();

    auto const moved =
      fixture.writer().moveManualListTracks(fixture.listId, std::array{fixture.thirdTrackId}, std::size_t{1});
    REQUIRE(moved);
    REQUIRE(moved->changed);

    CHECK(succession.state().currentTrackId == fixture.firstTrackId);
    CHECK(succession.state().optResolvedSuccessor == fixture.thirdTrackId);
    CHECK(succession.state() != beforeMove);
    CHECK(changedCount == 1);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == fixture.firstTrackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);

    auto const noOp =
      fixture.writer().moveManualListTracks(fixture.listId, std::array{fixture.thirdTrackId}, std::size_t{1});
    REQUIRE(noOp);
    CHECK_FALSE(noOp->changed);
    CHECK(succession.state().optResolvedSuccessor == fixture.thirdTrackId);
    CHECK(changedCount == 1);
  }

  TEST_CASE("PlaybackSuccession - smart membership changes retain current audio and a gap anchor",
            "[runtime][unit][playback-succession][projection]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.firstTrackId = fixture.addPlayableTrack("First", 1990);
    fixture.secondTrackId = fixture.addPlayableTrack("Second", 2000);
    fixture.thirdTrackId = fixture.addPlayableTrack("Third", 2010);
    fixture.sources.reloadAllTracks();
    fixture.listId = ao::test::requireValue(fixture.writer().createList(LibraryWriter::ListDraft{
      .kind = LibraryWriter::ListKind::Smart,
      .name = "Recent",
      .expression = "$year >= 2000",
    }));
    fixture.viewId = ao::test::requireValue(fixture.views.createView(TrackListViewConfig{
      .listId = fixture.listId,
      .optPresentation =
        TrackPresentationSpec{.id = "year-order", .sortBy = {{.field = TrackSortField::Year, .ascending = true}}}}));
    fixture.successionPtr = std::make_unique<PlaybackSuccession>(fixture.executor,
                                                                 fixture.views,
                                                                 fixture.sources,
                                                                 fixture.libraryFixture.library(),
                                                                 fixture.playbackTransport,
                                                                 fixture.notifications,
                                                                 fixture.asyncRuntime);
    auto& succession = *fixture.successionPtr;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.secondTrackId));
    auto const beforeAddition = succession.state();
    REQUIRE(beforeAddition.optResolvedSuccessor == fixture.thirdTrackId);
    std::uint32_t changedCount = 0;
    auto const subscription = succession.onChanged([&](PlaybackSuccessionState const&) { ++changedCount; });

    REQUIRE(fixture.writerFixture.updateMetadata(
      std::array{fixture.firstTrackId}, MetadataPatch{.optYear = std::uint16_t{2005}}));
    auto const afterAddition = succession.state();
    CHECK(afterAddition.currentTrackId == fixture.secondTrackId);
    CHECK(afterAddition.optResolvedSuccessor == fixture.firstTrackId);
    CHECK(afterAddition != beforeAddition);
    CHECK(changedCount == 1);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);

    REQUIRE(fixture.writerFixture.updateMetadata(
      std::array{fixture.secondTrackId}, MetadataPatch{.optYear = std::uint16_t{1990}}));
    auto const currentRemoved = succession.state();
    CHECK(currentRemoved.sourceState == PlaybackSuccessionSourceState::Live);
    CHECK(currentRemoved.currentTrackId == fixture.secondTrackId);
    CHECK(currentRemoved.optResolvedSuccessor == fixture.firstTrackId);
    CHECK(currentRemoved == afterAddition);
    CHECK(changedCount == 1);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - empty live repeat-one remains navigable until invalidation",
            "[runtime][unit][playback-succession][repeat]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.firstTrackId = fixture.addPlayableTrack("Only");
    fixture.openManualView(std::array{fixture.firstTrackId});
    auto& succession = *fixture.successionPtr;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.firstTrackId));
    succession.setRepeatMode(RepeatMode::One);

    REQUIRE(fixture.writer().removeManualListTracks(fixture.listId, std::array{fixture.firstTrackId}));
    auto const emptyLive = succession.state();
    CHECK(emptyLive.sourceState == PlaybackSuccessionSourceState::Live);
    CHECK(emptyLive.hasNext);
    CHECK(emptyLive.optResolvedSuccessor == fixture.firstTrackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);

    REQUIRE(fixture.writer().deleteList(fixture.listId));
    auto const invalidated = succession.state();
    CHECK(invalidated.sourceState == PlaybackSuccessionSourceState::Invalidated);
    CHECK_FALSE(invalidated.hasNext);
    CHECK_FALSE(invalidated.optResolvedSuccessor);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);

    succession.next();
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);

    auto const feed = fixture.notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    CHECK(std::get<NotificationReport>(feed.entries.front().message).templateId ==
          NotificationReportTemplate::PlaybackSequenceFinished);
    CHECK(feed.entries.front().severity == NotificationSeverity::Info);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::transient());
  }

  TEST_CASE("PlaybackSuccession - empty live source has no successor with repeat off or all",
            "[runtime][unit][playback-succession][repeat]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.firstTrackId = fixture.addPlayableTrack("Only");
    fixture.openManualView(std::array{fixture.firstTrackId});
    auto& succession = *fixture.successionPtr;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.firstTrackId));

    SECTION("repeat off")
    {
      REQUIRE(succession.state().repeat == RepeatMode::Off);
    }

    SECTION("repeat all")
    {
      succession.setRepeatMode(RepeatMode::All);
    }

    REQUIRE(fixture.writer().removeManualListTracks(fixture.listId, std::array{fixture.firstTrackId}));
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Live);
    CHECK_FALSE(succession.hasNext());
    CHECK_FALSE(succession.state().hasNext);
    CHECK_FALSE(succession.state().optResolvedSuccessor);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);

    succession.next();
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);

    auto const feed = fixture.notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    CHECK(std::get<NotificationReport>(feed.entries.front().message).templateId ==
          NotificationReportTemplate::PlaybackSequenceFinished);
    CHECK(feed.entries.front().severity == NotificationSeverity::Info);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::transient());
  }

  TEST_CASE("PlaybackSuccession - idle fallback advances without a prepared successor",
            "[runtime][regression][playback-succession]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto& playbackTransport = fixture.transport.playbackTransport;
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId));
    fixture.transport.executor.drain();
    REQUIRE(playbackTransport.clearPreparedNext());
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

    CHECK(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
    CHECK(fixture.transport.notificationService.feed().entries.empty());
  }

  TEST_CASE("PlaybackSuccession - output and final seek replace the disarmed lookahead token",
            "[runtime][unit][playback-succession][token]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto& playbackTransport = fixture.transport.playbackTransport;
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId));

    auto const optFirstToken = playbackTransport.clearPreparedNext();
    REQUIRE(optFirstToken);

    SECTION("output edge")
    {
      playbackTransport.setOutputDevice(
        audio::BackendId{"mock_backend"}, audio::DeviceId{"mock_device"}, audio::ProfileId{audio::kProfileShared});
    }

    SECTION("final seek edge")
    {
      playbackTransport.seek(std::chrono::milliseconds{0}, PlaybackTransport::SeekMode::Final);
    }

    auto const optReplacementToken = playbackTransport.clearPreparedNext();
    REQUIRE(optReplacementToken);
    CHECK(*optReplacementToken != *optFirstToken);
    CHECK(fixture.successionPtr->state().currentTrackId == fixture.firstTrackId);
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - natural prepared winner is adopted exactly once",
            "[runtime][unit][playback-succession][token]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto events = std::vector<PlaybackTransport::NowPlayingChanged>{};
    auto const subscription = fixture.transport.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& event) { events.push_back(event); });
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId));
    fixture.transport.executor.drain();
    events.clear();

    fixture.queueNaturalAdvance();
    fixture.transport.executor.drain();

    REQUIRE(events.size() == 1);
    REQUIRE(events.front().optPreparedNextToken);
    CHECK(events.front().trackId == fixture.secondTrackId);
    CHECK(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - retired prepared winner survives a final-seek race",
            "[runtime][unit][playback-succession][token]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto events = std::vector<PlaybackTransport::NowPlayingChanged>{};
    auto const subscription = fixture.transport.playbackTransport.onNowPlayingChanged(
      [&](PlaybackTransport::NowPlayingChanged const& event) { events.push_back(event); });
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId));
    fixture.transport.executor.drain();
    events.clear();

    fixture.queueNaturalAdvance();
    fixture.transport.playbackTransport.seek(std::chrono::milliseconds{0}, PlaybackTransport::SeekMode::Final);
    fixture.transport.executor.drain();

    REQUIRE(events.size() == 1);
    REQUIRE(events.front().optPreparedNextToken);
    CHECK(events.front().trackId == fixture.secondTrackId);
    CHECK(fixture.successionPtr->state().currentTrackId == fixture.secondTrackId);
    CHECK(fixture.successionPtr->state().optResolvedSuccessor == fixture.thirdTrackId);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - explicit session replacement rejects the queued stale advance",
            "[runtime][unit][playback-succession][token]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId));
    fixture.queueNaturalAdvance();

    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.thirdTrackId));
    auto const replacement = fixture.successionPtr->state();
    fixture.transport.executor.drain();

    CHECK(fixture.successionPtr->state() == replacement);
    CHECK(fixture.successionPtr->state().currentTrackId == fixture.thirdTrackId);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.thirdTrackId);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - destruction disconnects a queued natural-advance callback",
            "[runtime][unit][playback-succession][lifecycle]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    std::uint32_t changedCount = 0;
    auto const subscription = fixture.successionPtr->onChanged([&](PlaybackSuccessionState const&) { ++changedCount; });
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(changedCount == 1);
    fixture.queueNaturalAdvance();

    fixture.successionPtr.reset();
    fixture.transport.executor.drain();

    CHECK(changedCount == 1);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.trackId == fixture.secondTrackId);
  }

  TEST_CASE("PlaybackSuccession - navigation stops after three consecutive unplayable candidates",
            "[runtime][unit][playback-succession][failure]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    auto const playable = fixture.addPlayableTrack("Current");
    auto const brokenOne = fixture.libraryFixture.addTrack(
      library::test::TrackSpec{.title = "Broken one", .uri = "missing/one.flac", .codec = AudioCodec::Flac});
    auto const brokenTwo = fixture.libraryFixture.addTrack(
      library::test::TrackSpec{.title = "Broken two", .uri = "missing/two.flac", .codec = AudioCodec::Flac});
    auto const brokenThree = fixture.libraryFixture.addTrack(
      library::test::TrackSpec{.title = "Broken three", .uri = "missing/three.flac", .codec = AudioCodec::Flac});
    auto const unreachable = fixture.addPlayableTrack("Unreachable");
    fixture.openManualView(std::array{playable, brokenOne, brokenTwo, brokenThree, unreachable});
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, playable));

    fixture.successionPtr->next();

    CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Idle);
    CHECK(fixture.playbackTransport.state().nowPlaying.trackId != unreachable);
    auto const feed = fixture.notifications.feed();
    NotificationEntry const* skipSummary = nullptr;
    NotificationEntry const* failureLimit = nullptr;

    for (auto const& entry : feed.entries)
    {
      auto const* report = std::get_if<NotificationReport>(&entry.message);

      if (report != nullptr && report->templateId == NotificationReportTemplate::PlaybackTracksSkipped &&
          report->count == 3)
      {
        skipSummary = &entry;
      }

      if (report != nullptr && report->templateId == NotificationReportTemplate::PlaybackStoppedAfterFailures &&
          report->count == 3)
      {
        failureLimit = &entry;
      }
    }

    REQUIRE(skipSummary != nullptr);
    CHECK(skipSummary->severity == NotificationSeverity::Warning);
    CHECK(skipSummary->lifetime == NotificationLifetime::history());
    REQUIRE(failureLimit != nullptr);
    CHECK(failureLimit->severity == NotificationSeverity::Error);
    CHECK(failureLimit->lifetime == NotificationLifetime::pinned());
  }

  TEST_CASE("PlaybackSuccession - non-recoverable device failure terminates the active session",
            "[runtime][unit][playback-succession][failure]")
  {
    auto fixture = PlaybackSuccessionTransportFixture{};
    fixture.buildThreeTrackManualView();
    auto failures = std::vector<PlaybackFailure>{};
    auto const subscription = fixture.transport.playbackTransport.onPlaybackFailure([&](PlaybackFailure const& failure)
                                                                                    { failures.push_back(failure); });
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(fixture.transport.renderTarget != nullptr);

    fixture.transport.renderTarget->handleBackendError("device lost during succession playback");
    REQUIRE(fixture.transport.executor.drainUntil([&] { return !failures.empty(); }));

    REQUIRE(failures.size() == 1);
    CHECK(failures.front().kind == PlaybackFailureKind::DeviceLost);
    CHECK_FALSE(failures.front().recoverable);
    CHECK(failures.front().disposition == PlaybackFailureDisposition::Stopped);
    CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(fixture.transport.playbackTransport.state().transport == audio::Transport::Idle);

    auto const feed = fixture.transport.notificationService.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
  }

  TEST_CASE("PlaybackSuccession - track failure on an invalidated source posts one terminal succession error",
            "[runtime][unit][playback-succession][concurrency]")
  {
    auto failureGate = audio::test::StagedFailureGate{};
    auto fixture = PlaybackSuccessionSeekFixture{&failureGate};
    auto releaseGuard = audio::test::StagedFailureReleaseGuard{failureGate};
    fixture.buildSingleTrackManualView();
    auto failures = std::vector<PlaybackFailure>{};
    auto const subscription =
      fixture.transportPtr->onPlaybackFailure([&](PlaybackFailure const& failure) { failures.push_back(failure); });
    REQUIRE(fixture.successionPtr->playFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(failureGate.waitForRead());

    REQUIRE(fixture.writer().deleteList(fixture.listId));
    REQUIRE(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Invalidated);
    releaseGuard.release();
    REQUIRE(fixture.executor.drainUntil([&] { return !failures.empty(); }));

    REQUIRE(failures.size() == 1);
    CHECK(failures.front().kind == PlaybackFailureKind::Decode);
    CHECK(failures.front().disposition == PlaybackFailureDisposition::Stopped);
    CHECK(fixture.successionPtr->state().sourceState == PlaybackSuccessionSourceState::Inactive);

    auto const feed = fixture.notifications.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().severity == NotificationSeverity::Error);
    CHECK(feed.entries.front().lifetime == NotificationLifetime::pinned());
    REQUIRE(std::holds_alternative<NotificationReport>(feed.entries.front().message));
    auto const& report = std::get<NotificationReport>(feed.entries.front().message);
    CHECK(report.templateId == NotificationReportTemplate::PlaybackStoppedForTrack);
    CHECK(report.subject == "Failing current");
    CHECK(report.detail == "gated staged decode failure");
  }

  TEST_CASE("PlaybackSuccession - previous restart uses a strict greater-than three-second final seek",
            "[runtime][unit][playback-succession][previous]")
  {
    auto fixture = PlaybackSuccessionSeekFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    auto& playbackTransport = *fixture.transportPtr;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.firstTrackId));
    fixture.executor.drain();
    playbackTransport.pause();
    REQUIRE(playbackTransport.state().transport == audio::Transport::Paused);

    playbackTransport.seek(std::chrono::milliseconds{3000}, PlaybackTransport::SeekMode::Final);
    CHECK(playbackTransport.elapsed() == std::chrono::milliseconds{3000});
    CHECK_FALSE(succession.hasPrevious());
    CHECK_FALSE(succession.state().hasPrevious);

    playbackTransport.seek(std::chrono::milliseconds{3001}, PlaybackTransport::SeekMode::Final);
    CHECK(playbackTransport.elapsed() == std::chrono::milliseconds{3001});
    CHECK(succession.hasPrevious());
    CHECK(succession.state().hasPrevious);

    succession.previous();
    CHECK(succession.state().currentTrackId == fixture.firstTrackId);
    CHECK_FALSE(succession.hasPrevious());
    CHECK_FALSE(succession.state().hasPrevious);
    CHECK(playbackTransport.elapsed() == std::chrono::milliseconds{0});
    CHECK(playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - commands and dedicated mode signals follow cursor resolution",
            "[runtime][unit][playback-succession][command]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.firstTrackId));

    std::uint32_t shuffleEvents = 0;
    std::uint32_t repeatEvents = 0;
    auto const shuffleSubscription =
      succession.onShuffleModeChanged([&](PlaybackSuccession::ShuffleModeChanged const&) { ++shuffleEvents; });
    auto const repeatSubscription =
      succession.onRepeatModeChanged([&](PlaybackSuccession::RepeatModeChanged const&) { ++repeatEvents; });

    succession.next();
    CHECK(succession.state().currentTrackId == fixture.secondTrackId);
    CHECK(succession.hasPrevious());

    succession.previous();
    CHECK(succession.state().currentTrackId == fixture.firstTrackId);

    succession.setRepeatMode(RepeatMode::One);
    CHECK(succession.state().repeat == RepeatMode::One);
    CHECK(succession.state().optResolvedSuccessor == fixture.firstTrackId);
    CHECK(repeatEvents == 1);
    succession.setRepeatMode(RepeatMode::One);
    CHECK(repeatEvents == 1);

    succession.setShuffleMode(ShuffleMode::On);
    CHECK(succession.state().shuffle == ShuffleMode::On);
    CHECK(shuffleEvents == 1);

    succession.clear();
    CHECK(succession.state().sourceState == PlaybackSuccessionSourceState::Inactive);
    CHECK(succession.state().currentTrackId == kInvalidTrackId);
    CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSuccession - shuffle failure walks preserve shuffle direction and semantic parity",
            "[runtime][unit][playback-succession][shuffle]")
  {
    auto fixture = PlaybackSuccessionFixture{};
    fixture.buildThreeTrackManualView();
    auto& succession = *fixture.successionPtr;
    REQUIRE(succession.playFromView(fixture.viewId, fixture.firstTrackId));
    succession.setShuffleMode(ShuffleMode::On);

    SECTION("failed forward candidate is excluded before the sticky candidate is re-resolved")
    {
      auto const optFailedCandidate = succession.state().optResolvedSuccessor;
      REQUIRE(optFailedCandidate);
      fixture.removePlayableFile(*optFailedCandidate);

      succession.next();

      CHECK(succession.state().currentTrackId != fixture.firstTrackId);
      CHECK(succession.state().currentTrackId != *optFailedCandidate);
      CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    }

    SECTION("failed history previous does not fall through to sequential previous")
    {
      succession.next();
      auto const currentTrackId = succession.state().currentTrackId;
      REQUIRE(currentTrackId != fixture.firstTrackId);
      REQUIRE(succession.hasPrevious());
      fixture.removePlayableFile(fixture.firstTrackId);

      succession.previous();

      CHECK(succession.state().currentTrackId == currentTrackId);
      CHECK_FALSE(succession.hasPrevious());
      CHECK(fixture.playbackTransport.state().transport == audio::Transport::Playing);
    }
  }
} // namespace ao::rt::test
