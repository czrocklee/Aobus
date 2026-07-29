// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/audio/ScriptedDecoderSession.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/PlaybackTransportTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Device.h>
#include <ao/audio/Format.h>
#include <ao/audio/Player.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test::playback_succession
{
  struct DecoderActivationProbe final
  {
    void registerPath(std::filesystem::path const& path)
    {
      auto const lock = std::scoped_lock{mutex};
      counts.try_emplace(path, 0);
    }

    void notify(std::filesystem::path const& path)
    {
      auto const lock = std::scoped_lock{mutex};

      if (auto const it = counts.find(path); it != counts.end())
      {
        ++it->second;
      }
    }

    std::size_t count(std::filesystem::path const& path) const
    {
      auto const lock = std::scoped_lock{mutex};
      auto const it = counts.find(path);
      return it == counts.end() ? 0 : it->second;
    }

    std::map<std::filesystem::path, std::size_t> snapshot() const
    {
      auto const lock = std::scoped_lock{mutex};
      return counts;
    }

    mutable std::mutex mutex;
    std::map<std::filesystem::path, std::size_t> counts;
  };

  class [[nodiscard]] PreparationReleaseGuard final
  {
  public:
    explicit PreparationReleaseGuard(std::shared_ptr<audio::test::BlockingPreparationGate> gatePtr)
      : _gatePtr{std::move(gatePtr)}
    {
    }

    ~PreparationReleaseGuard()
    {
      if (_gatePtr)
      {
        _gatePtr->release.release();
      }
    }

    void release()
    {
      _gatePtr->release.release();
      _gatePtr.reset();
    }

    PreparationReleaseGuard(PreparationReleaseGuard const&) = delete;
    PreparationReleaseGuard& operator=(PreparationReleaseGuard const&) = delete;
    PreparationReleaseGuard(PreparationReleaseGuard&&) = delete;
    PreparationReleaseGuard& operator=(PreparationReleaseGuard&&) = delete;

  private:
    std::shared_ptr<audio::test::BlockingPreparationGate> _gatePtr;
  };

  inline auto makeActivationProbedDecoderFactory(
    std::shared_ptr<DecoderActivationProbe> probePtr,
    std::shared_ptr<audio::test::BlockingPreparationGate> blockingGatePtr = {},
    std::filesystem::path blockedFileName = {},
    bool const failBlockedPreparation = false,
    bool const blockEveryLookahead = false)
  {
    return [probePtr = std::move(probePtr),
            blockingGatePtr = std::move(blockingGatePtr),
            blockedFileName = std::move(blockedFileName),
            failBlockedPreparation,
            blockEveryLookahead](std::filesystem::path const& path, audio::Format const&)
    {
      auto const blocks =
        blockingGatePtr &&
        (path.filename() == blockedFileName ||
         (blockEveryLookahead && path.filename() != std::filesystem::path{"transport-playable-0.flac"}));

      if (blocks)
      {
        blockingGatePtr->enterAndWait();
      }

      auto const format = audio::Format{
        .sampleRate = 44100,
        .channels = 2,
        .bitDepth = 16,
        .isInterleaved = true,
      };
      auto decoderPtr = std::make_unique<audio::test::ScriptedDecoderSession>(audio::DecodedStreamInfo{
        .sourceFormat = format,
        .outputFormat = format,
        .duration = std::chrono::seconds{2},
        .isLossy = false,
        .codec = AudioCodec::Flac,
      });
      decoderPtr->setReadScript(
        {{.data = std::vector<std::byte>(100000, std::byte{0}), .endOfStream = false}, {.endOfStream = true}});

      if (blocks && failBlockedPreparation)
      {
        decoderPtr->setOpenResult(makeError(Error::Code::IoError, "Scripted lookahead preparation failure"));
      }

      decoderPtr->setReadObserver(
        [probePtr, path](std::size_t const readCount)
        {
          if (readCount == 2)
          {
            probePtr->notify(path);
          }
        });

      if (blocks)
      {
        blockingGatePtr->createdPtr->fetch_add(1, std::memory_order_relaxed);
        decoderPtr->setDestroyCounter(blockingGatePtr->destroyedPtr);
      }

      return decoderPtr;
    };
  }

  inline Result<> playFromViewAndWait(PlaybackSuccession& succession,
                                      QueuedExecutor& executor,
                                      ViewId const viewId,
                                      TrackId const trackId)
  {
    bool settled = false;
    auto const settlementSubscription = succession.onExplicitStartSettled([&] noexcept { settled = true; });

    if (auto started = succession.playFromView(viewId, trackId); !started)
    {
      return started;
    }

    if (!executor.drainUntil([&] { return settled; }, std::chrono::seconds{5}))
    {
      return makeError(Error::Code::InvalidState, "Timed out waiting for explicit playback start settlement");
    }

    return {};
  }

  inline NavigationRequest navigationRequest(TrackListViewConfig config)
  {
    auto request = NavigationRequest{
      .target =
        FilteredListTarget{
          .listId = config.listId,
          .filterExpression = std::move(config.filterExpression),
        },
    };

    if (config.optPresentation)
    {
      request.optPresentation = NavigationPresentation{.spec = std::move(*config.optPresentation)};
    }
    else if (config.groupBy != TrackGroupKey::None || !config.sortBy.empty())
    {
      request.optPresentation = NavigationPresentation{
        .spec = TrackPresentationSpec{.groupBy = config.groupBy, .sortBy = std::move(config.sortBy)},
      };
    }

    return request;
  }

  struct PlaybackSuccessionFixture final
  {
    PlaybackSuccessionFixture()
      : asyncRuntime{executor, 1, {}, &sleeper}
      , changes{libraryChangesExecutor, 0}
      , writerFixture{libraryFixture.library(), changes}
      , sources{libraryFixture.library(), changes}
      , views{executor, libraryFixture.library(), sources}
      , workspace{executor, views, changes}
      , playbackTransport{makePlaybackTransport(asyncRuntime, libraryFixture.library(), notifications)}
    {
      PlaybackBootstrap{playbackTransport}.addProvider(makeReadyAudioProvider());
      executor.drain();
    }

    LibraryWriter& writer() { return writerFixture.writer(); }

    TrackId addPlayableTrack(std::string title, std::uint16_t const year = 2020)
    {
      auto const playableUri = std::format("playable-{}.flac", nextPlayableFile++);
      audio::test::installAudioFixture(libraryFixture.root(), "basic_metadata.flac", playableUri);
      auto const created = ao::test::requireValue(writer().createTrackFromFile(libraryFixture.root() / playableUri));
      executor.drain();
      REQUIRE(
        writerFixture.updateMetadata(std::array{created.trackId}, MetadataPatch{.optTitle = title, .optYear = year}));
      executor.drain();
      return created.trackId;
    }

    void removePlayableFile(TrackId const trackId)
    {
      auto transaction = libraryFixture.library().readTransaction();
      auto const optView =
        libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Cold);
      REQUIRE(optView);
      REQUIRE(std::filesystem::remove(libraryFixture.root() / std::filesystem::path{optView->property().uri()}));
    }

    void openManualView(std::span<TrackId const> const trackIds, TrackListViewConfig config = {})
    {
      auto const membershipTag = std::array{std::string{"playbackorder"}};
      REQUIRE(writerFixture.editTags(trackIds, membershipTag, {}));
      sources.reloadAllTracks();
      listId = ao::test::requireValue(writer().createList(LibraryWriter::ListDraft{
        .name = "Playback order",
        .expression = "#playbackorder",
      }));

      if (!config.optPresentation)
      {
        config.optPresentation = TrackPresentationSpec{.id = std::string{kListOrderTrackPresentationId}};
      }

      config.listId = listId;
      viewId = ao::test::requireValue(workspace.navigate(navigationRequest(std::move(config))));
      successionPtr = std::make_unique<PlaybackSuccession>(
        executor, views, sources, libraryFixture.library(), playbackTransport, notifications, asyncRuntime);
    }

    void removeFromList(std::span<TrackId const> const trackIds)
    {
      auto const membershipTag = std::array{std::string{"playbackorder"}};
      REQUIRE(writerFixture.editTags(trackIds, {}, membershipTag));
    }

    Result<LibraryWriter::MoveOrderAuthoringResult> moveListOrder(std::span<TrackId const> const selectedTrackIds,
                                                                  std::optional<TrackId> const optBeforeTrackId)
    {
      auto lease = ao::test::requireValue(sources.acquire(listId));
      auto const effectiveTrackIds = sourceTrackIds(lease.source());
      auto binding = ao::test::requireValue(writerFixture.library().bindListOrder(listId, effectiveTrackIds));
      return writer().moveListOrder(binding, selectedTrackIds, optBeforeTrackId);
    }

    void buildThreeTrackManualView(TrackListViewConfig config = {})
    {
      firstTrackId = addPlayableTrack("First", 1990);
      secondTrackId = addPlayableTrack("Second", 2000);
      thirdTrackId = addPlayableTrack("Third", 2010);
      openManualView(std::array{firstTrackId, secondTrackId, thirdTrackId}, std::move(config));
    }

    Result<> playAndWait(TrackId const trackId)
    {
      return playFromViewAndWait(*successionPtr, executor, viewId, trackId);
    }

    MusicLibraryFixture libraryFixture;
    ControlledSleeper sleeper;
    QueuedExecutor executor;
    InlineExecutor libraryChangesExecutor;
    async::Runtime asyncRuntime;
    LibraryChanges changes;
    LibraryWriterFixture writerFixture;
    TrackSourceCache sources;
    ViewService views;
    WorkspaceService workspace;
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

  struct PlaybackSuccessionTransportFixtureConfig final
  {
    std::shared_ptr<audio::test::BlockingPreparationGate> blockingGatePtr{};
    std::filesystem::path blockedFileName{};
    bool failBlockedPreparation = false;
    bool blockEveryLookahead = false;
  };

  struct PlaybackSuccessionTransportFixture final
  {
    explicit PlaybackSuccessionTransportFixture(PlaybackSuccessionTransportFixtureConfig config = {})
      : decoderProbePtr{std::make_shared<DecoderActivationProbe>()}
      , transport{makeActivationProbedDecoderFactory(decoderProbePtr,
                                                     std::move(config.blockingGatePtr),
                                                     std::move(config.blockedFileName),
                                                     config.failBlockedPreparation,
                                                     config.blockEveryLookahead)}
      , asyncRuntime{transport.executor, 1, {}, &sleeper}
      , changes{libraryChangesExecutor, 0}
      , writerFixture{transport.libraryFixture.library(), changes}
      , sources{transport.libraryFixture.library(), changes}
      , views{transport.executor, transport.libraryFixture.library(), sources}
      , workspace{transport.executor, views, changes}
    {
      transport.onDevicesChangedCb(transport.status.devices);
      transport.executor.drain();
    }

    LibraryWriter& writer() { return writerFixture.writer(); }

    TrackId addPlayableTrack(std::string title)
    {
      auto const libraryUri = std::format("transport-playable-{}.flac", nextPlayableFile++);
      auto const fixtureUri =
        audio::test::installAudioFixture(transport.libraryFixture.root(), "basic_metadata.flac", libraryUri);
      auto const created =
        ao::test::requireValue(writer().createTrackFromFile(transport.libraryFixture.root() / fixtureUri));
      transport.executor.drain();
      REQUIRE(writerFixture.updateMetadata(std::array{created.trackId}, MetadataPatch{.optTitle = title}));
      transport.executor.drain();
      auto const trackId = created.trackId;
      auto const path = transport.libraryFixture.root() / fixtureUri;
      decoderProbePtr->registerPath(path);
      trackPaths.insert_or_assign(trackId, path);
      return trackId;
    }

    void buildThreeTrackManualView()
    {
      firstTrackId = addPlayableTrack("First");
      secondTrackId = addPlayableTrack("Second");
      thirdTrackId = addPlayableTrack("Third");
      sources.reloadAllTracks();
      listId = ao::test::requireValue(writer().createList(LibraryWriter::ListDraft{
        .name = "Transport order",
      }));
      viewId = ao::test::requireValue(workspace.navigate({.target = listId}));
      successionPtr = std::make_unique<PlaybackSuccession>(transport.executor,
                                                           views,
                                                           sources,
                                                           transport.libraryFixture.library(),
                                                           transport.playbackTransport,
                                                           transport.notificationService,
                                                           asyncRuntime);
    }

    void buildTwoTrackManualView()
    {
      firstTrackId = addPlayableTrack("First");
      secondTrackId = addPlayableTrack("Second");
      sources.reloadAllTracks();
      listId = ao::test::requireValue(writer().createList(LibraryWriter::ListDraft{
        .name = "Transport order",
      }));
      viewId = ao::test::requireValue(workspace.navigate({.target = listId}));
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

    Result<> playAndWait(TrackId const trackId)
    {
      auto const activationCounts = decoderProbePtr->snapshot();
      auto started = playFromViewAndWait(*successionPtr, transport.executor, viewId, trackId);

      if (!started || !successionPtr->state().optResolvedSuccessor)
      {
        return started;
      }

      auto const successorId = *successionPtr->state().optResolvedSuccessor;
      auto const& successorPath = trackPaths.at(successorId);
      auto const it = activationCounts.find(successorPath);
      auto const previousCount = it == activationCounts.end() ? 0 : it->second;

      if (!waitForLookaheadAfter(successorId, previousCount))
      {
        return makeError(Error::Code::InvalidState, "Timed out waiting for playback lookahead activation");
      }

      return {};
    }

    std::size_t lookaheadActivationCount(TrackId const trackId) const
    {
      return decoderProbePtr->count(trackPaths.at(trackId));
    }

    bool waitForLookaheadAfter(TrackId const trackId, std::size_t const previousCount)
    {
      auto const& path = trackPaths.at(trackId);
      return transport.executor.drainUntil(
        [&] { return decoderProbePtr->count(path) > previousCount; }, std::chrono::seconds{5});
    }

    std::shared_ptr<DecoderActivationProbe> decoderProbePtr;
    PlaybackTransportFixture<QueuedExecutor> transport;
    ControlledSleeper sleeper;
    InlineExecutor libraryChangesExecutor;
    async::Runtime asyncRuntime;
    LibraryChanges changes;
    LibraryWriterFixture writerFixture;
    TrackSourceCache sources;
    ViewService views;
    WorkspaceService workspace;
    std::unique_ptr<PlaybackSuccession> successionPtr;
    TrackId firstTrackId = kInvalidTrackId;
    TrackId secondTrackId = kInvalidTrackId;
    TrackId thirdTrackId = kInvalidTrackId;
    ListId listId = kInvalidListId;
    ViewId viewId = kInvalidViewId;
    std::map<TrackId, std::filesystem::path> trackPaths;
    std::uint32_t nextPlayableFile = 0;
  };

  struct PlaybackSuccessionSeekFixture final
  {
    explicit PlaybackSuccessionSeekFixture(audio::test::StagedFailureGate* const failureGate = nullptr)
      : asyncRuntime{executor}
      , changes{libraryChangesExecutor, 0}
      , writerFixture{libraryFixture.library(), changes}
      , sources{libraryFixture.library(), changes}
      , views{executor, libraryFixture.library(), sources}
      , workspace{executor, views, changes}
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

      auto playerPtr = std::make_unique<audio::Player>(asyncRuntime, std::move(decoderFactory));
      transportPtr =
        std::make_unique<PlaybackTransport>(executor, libraryFixture.library(), notifications, std::move(playerPtr));
      PlaybackBootstrap{*transportPtr}.addProvider(makeReadyAudioProvider());
      executor.drain();
    }

    LibraryWriter& writer() { return writerFixture.writer(); }

    TrackId addPlayableTrack(std::string title)
    {
      auto const playableUri = std::format("seek-playable-{}.flac", nextPlayableFile++);
      audio::test::installAudioFixture(libraryFixture.root(), "basic_metadata.flac", playableUri);
      auto const created = ao::test::requireValue(writer().createTrackFromFile(libraryFixture.root() / playableUri));
      executor.drain();
      REQUIRE(writerFixture.updateMetadata(std::array{created.trackId}, MetadataPatch{.optTitle = title}));
      executor.drain();
      return created.trackId;
    }

    void buildThreeTrackManualView()
    {
      firstTrackId = addPlayableTrack("First");
      secondTrackId = addPlayableTrack("Second");
      thirdTrackId = addPlayableTrack("Third");
      sources.reloadAllTracks();
      listId = ao::test::requireValue(writer().createList(LibraryWriter::ListDraft{
        .name = "Long playback order",
      }));
      viewId = ao::test::requireValue(workspace.navigate({.target = listId}));
      successionPtr = std::make_unique<PlaybackSuccession>(
        executor, views, sources, libraryFixture.library(), *transportPtr, notifications, asyncRuntime);
    }

    void buildSingleTrackManualView()
    {
      firstTrackId = addPlayableTrack("Failing current");
      sources.reloadAllTracks();
      listId = ao::test::requireValue(writer().createList(LibraryWriter::ListDraft{
        .name = "Failing playback order",
      }));
      viewId = ao::test::requireValue(workspace.navigate({.target = listId}));
      successionPtr = std::make_unique<PlaybackSuccession>(
        executor, views, sources, libraryFixture.library(), *transportPtr, notifications, asyncRuntime);
    }

    Result<> playAndWait(TrackId const trackId)
    {
      return playFromViewAndWait(*successionPtr, executor, viewId, trackId);
    }

    MusicLibraryFixture libraryFixture;
    QueuedExecutor executor;
    InlineExecutor libraryChangesExecutor;
    async::Runtime asyncRuntime;
    LibraryChanges changes;
    LibraryWriterFixture writerFixture;
    TrackSourceCache sources;
    ViewService views;
    WorkspaceService workspace;
    NotificationService notifications{asyncRuntime};
    std::unique_ptr<PlaybackTransport> transportPtr;
    std::unique_ptr<PlaybackSuccession> successionPtr;
    TrackId firstTrackId = kInvalidTrackId;
    TrackId secondTrackId = kInvalidTrackId;
    TrackId thirdTrackId = kInvalidTrackId;
    ListId listId = kInvalidListId;
    ViewId viewId = kInvalidViewId;
    std::uint32_t nextPlayableFile = 0;
  };
} // namespace ao::rt::test::playback_succession
