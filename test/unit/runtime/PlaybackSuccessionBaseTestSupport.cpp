// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/runtime/PlaybackSuccessionBaseTestSupport.h"

#include "runtime/playback/PlaybackBootstrap.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace ao::rt::test::playback_succession
{
  Result<> playFromViewAndWait(PlaybackSuccession& succession,
                               QueuedExecutor& executor,
                               ViewId const viewId,
                               TrackId const trackId)
  {
    bool settled = false;
    auto const settlementSubscription = succession.onExplicitStartSettled([&] noexcept { settled = true; });

    if (auto startedRes = succession.playFromView(viewId, trackId); !startedRes)
    {
      return startedRes;
    }

    if (!executor.drainUntil([&] { return settled; }, std::chrono::seconds{5}))
    {
      return makeError(Error::Code::InvalidState, "Timed out waiting for explicit playback start settlement");
    }

    return {};
  }

  NavigationRequest navigationRequest(TrackListViewConfig config)
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

  PlaybackSuccessionFixture::PlaybackSuccessionFixture()
    : asyncRuntime{executor, 1, &sleeper}
    , changes{libraryChangesExecutor, 0, "test-library"}
    , writerFixture{libraryFixture.library(), changes}
    , sources{libraryFixture.library(), changes}
    , views{executor, libraryFixture.library(), sources, changes}
    , workspace{executor, views, changes}
    , playbackTransport{makePlaybackTransport(asyncRuntime, libraryFixture.library(), notifications)}
  {
    PlaybackBootstrap{playbackTransport}.addProvider(makeReadyAudioProvider());
    executor.drain();
  }

  PlaybackSuccessionFixture::~PlaybackSuccessionFixture() = default;

  LibraryWriter& PlaybackSuccessionFixture::writer()
  {
    return writerFixture.writer();
  }

  TrackId PlaybackSuccessionFixture::addPlayableTrack(std::string title, std::uint16_t const year)
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

  void PlaybackSuccessionFixture::removePlayableFile(TrackId const trackId)
  {
    auto transaction = libraryFixture.library().readTransaction();
    auto const optView =
      libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Cold);
    REQUIRE(optView);
    REQUIRE(std::filesystem::remove(libraryFixture.root() / std::filesystem::path{optView->property().uri()}));
  }

  void PlaybackSuccessionFixture::openManualView(std::span<TrackId const> const trackIds, TrackListViewConfig config)
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

  void PlaybackSuccessionFixture::removeFromList(std::span<TrackId const> const trackIds)
  {
    auto const membershipTag = std::array{std::string{"playbackorder"}};
    REQUIRE(writerFixture.editTags(trackIds, {}, membershipTag));
  }

  Result<LibraryWriter::MoveOrderAuthoringResult> PlaybackSuccessionFixture::moveListOrder(
    std::span<TrackId const> const selectedTrackIds,
    std::optional<TrackId> const optBeforeTrackId)
  {
    auto lease = ao::test::requireValue(sources.acquire(listId));
    auto const effectiveTrackIds = sourceTrackIds(lease.source());
    auto binding = ao::test::requireValue(writerFixture.library().bindListOrder(listId, effectiveTrackIds));
    return writer().moveListOrder(binding, selectedTrackIds, optBeforeTrackId);
  }

  void PlaybackSuccessionFixture::buildThreeTrackManualView(TrackListViewConfig config)
  {
    firstTrackId = addPlayableTrack("First", 1990);
    secondTrackId = addPlayableTrack("Second", 2000);
    thirdTrackId = addPlayableTrack("Third", 2010);
    openManualView(std::array{firstTrackId, secondTrackId, thirdTrackId}, std::move(config));
  }

  Result<> PlaybackSuccessionFixture::playAndWait(TrackId const trackId)
  {
    return playFromViewAndWait(*successionPtr, executor, viewId, trackId);
  }
} // namespace ao::rt::test::playback_succession
