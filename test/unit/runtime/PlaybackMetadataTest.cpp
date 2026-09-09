// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/LibraryWriteLane.h"
#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/ApplicationPlaybackTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/PlaybackTestSupport.h"
#include "test/unit/runtime/PlaybackTransportTestSupport.h"
#include "test/unit/runtime/library/LibraryWriteLaneTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/audio/Transport.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/TrackBuilder.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    // Only resource identity is exercised here; frontend tests own image decoding.
    constexpr auto kFirstCover = std::array{std::byte{1}};
    constexpr auto kSecondCover = std::array{std::byte{2}};

    struct PlaybackMetadataFixture final
    {
      PlaybackMetadataFixture()
      {
        transport.onDevicesChangedCb(transport.status.devices);
        transport.executor.drain();
      }

      template<typename Operation>
      auto commit(Operation operation)
      {
        auto res = runTestTask(transport.asyncRuntime,
                               transport.executor,
                               executeInteractiveMutationAsync(writeLane.captureSubmission(), std::move(operation)));
        REQUIRE(res);
        transport.executor.drain();
        return res->value;
      }

      TrackId addTrack(std::string const& title, std::span<std::byte const> const cover = kFirstCover)
      {
        auto const uri = transport.installAudioFixture("basic_metadata.flac", std::format("{}.flac", title));
        return commit(
          [&](library::LibraryWrite& write) -> Result<OperationOutcome<TrackId>>
          {
            auto builder = library::TrackBuilder::makeEmpty();
            builder.property().uri(uri);
            builder.metadata().title(title).artist("Launch artist").album("Launch album");

            if (!cover.empty())
            {
              builder.coverArt().add(PictureType::FrontCover, cover);
            }

            auto res = write.tracks().create(builder, library::FileManifestBuilder::makeEmpty());
            REQUIRE(res);
            return Changed<TrackId>{.value = *res, .changeSet = {.tracksInserted = {*res}}};
          });
      }

      void mutate(TrackId const id, std::function<void(library::TrackBuilder&)> update, bool const reset = false)
      {
        commit(
          [&](library::LibraryWrite& write) -> Result<OperationOutcome<bool>>
          {
            auto writer = write.tracks();
            auto const optView = writer.get(id);
            REQUIRE(optView);
            auto builder =
              library::TrackBuilder::fromCompleteView(*optView, transport.libraryFixture.library().dictionary());
            update(builder);
            REQUIRE(writer.update(id, builder));
            return Changed<bool>{
              .value = true,
              .changeSet = reset ? LibraryChangeSet{.libraryReset = true} : LibraryChangeSet{.tracksMutated = {id}}};
          });
      }

      void start(TrackId const id)
      {
        REQUIRE(transport.playbackTransport.playTrack(id, ListId{7}));
        REQUIRE(transport.executor.tryDrainUntil(
          [&] { return playback.snapshot().transport.transport == audio::Transport::Playing; }));
        transport.executor.drain();
      }

      PlaybackTransportFixture<QueuedExecutor> transport;
      LibraryChanges changes{transport.executor, 0, "metadata-test-library"};
      LibraryWriteLane writeLane{transport.executor,
                                 library::test::requireWritableLibrary(transport.libraryFixture.library()),
                                 changes};
      TrackSourceCache sources{transport.libraryFixture.library(), changes};
      ViewService views{transport.executor, transport.libraryFixture.library(), sources, changes};
      PlaybackSuccession succession{transport.executor,
                                    views,
                                    sources,
                                    transport.libraryFixture.library(),
                                    transport.playbackTransport,
                                    transport.notificationService,
                                    transport.asyncRuntime};
      PlaybackBootstrap bootstrap{transport.playbackTransport};
      PlaybackService playback{
        bootstrap.createPlaybackService(transport.executor, succession, transport.libraryFixture.library(), changes)};
    };
  } // namespace

  TEST_CASE("PlaybackService metadata - committed edits reach consumers without changing playback identity",
            "[runtime][regression][playback][concurrency]")
  {
    auto fixture = PlaybackMetadataFixture{};
    auto const trackId = fixture.addTrack("Launch");
    fixture.start(trackId);

    if (auto const paused = GENERATE(false, true); paused)
    {
      fixture.playback.commands().pause();
      fixture.transport.executor.drain();
    }

    auto const before = fixture.playback.snapshot();
    REQUIRE(before.transport.nowPlaying.coverArtId != kInvalidResourceId);
    auto rendered = uimodel::NowPlayingViewState{};
    auto const viewModel = uimodel::NowPlayingViewModel{
      fixture.playback, ao::test::englishMessageCatalog(), [&](auto const& view) { rendered = view; }};
    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto const subscription =
      fixture.playback.events().onSnapshot([&](auto const& snapshot) { snapshots.push_back(snapshot); });
    fixture.mutate(trackId,
                   [](library::TrackBuilder& builder)
                   {
                     builder.metadata().title("Edited").artist("Edited artist").album("Edited album");
                     builder.coverArt().clear().add(PictureType::FrontCover, kSecondCover);
                   });

    REQUIRE(snapshots.size() == 1);
    auto const after = fixture.playback.snapshot();
    CHECK(after.transport.nowPlaying.title == "Edited");
    CHECK(after.transport.nowPlaying.artist == "Edited artist");
    CHECK(after.transport.nowPlaying.album == "Edited album");
    CHECK(after.transport.nowPlaying.coverArtId != kInvalidResourceId);
    CHECK(after.transport.nowPlaying.coverArtId != before.transport.nowPlaying.coverArtId);
    auto expected = before;
    expected.transport.nowPlaying = after.transport.nowPlaying;
    CHECK(after == expected);
    CHECK(rendered.title == "Edited");
    CHECK(rendered.artist == "Edited artist");
    CHECK(rendered.coverArtId == after.transport.nowPlaying.coverArtId);
    CHECK(rendered.coverArtPlaceholderIdentity.primaryText == "Edited album");
    CHECK(fixture.transport.playbackTransport.state().nowPlaying == before.transport.nowPlaying);

    fixture.playback.commands().setVolume(0.25F);
    fixture.playback.commands().resume();
    fixture.transport.executor.drain();
    CHECK(fixture.playback.snapshot().transport.nowPlaying == after.transport.nowPlaying);
  }

  TEST_CASE("PlaybackService metadata - cover removal and reset survive later transport snapshots",
            "[runtime][regression][playback][concurrency]")
  {
    auto fixture = PlaybackMetadataFixture{};
    auto const trackId = fixture.addTrack("Launch");
    fixture.start(trackId);
    auto const reset = GENERATE(false, true);
    auto const before = fixture.playback.snapshot();
    fixture.mutate(
      trackId,
      [](library::TrackBuilder& builder)
      {
        builder.coverArt().clear();
        builder.metadata().title("Reset title");
      },
      reset);
    auto expected = before;
    expected.transport.nowPlaying.coverArtId = kInvalidResourceId;
    expected.transport.nowPlaying.title = "Reset title";
    CHECK(fixture.playback.snapshot() == expected);

    fixture.playback.commands().pause();
    fixture.playback.commands().resume();
    fixture.transport.executor.drain();
    CHECK(fixture.playback.snapshot().transport.nowPlaying == expected.transport.nowPlaying);
  }

  TEST_CASE("PlaybackService metadata - deleted tracks keep launch text and clear the cover",
            "[runtime][regression][playback][concurrency]")
  {
    auto fixture = PlaybackMetadataFixture{};
    auto const trackId = fixture.addTrack("Launch");
    fixture.start(trackId);
    auto const before = fixture.playback.snapshot();
    fixture.mutate(trackId, [](library::TrackBuilder& builder) { builder.metadata().title("Edited before deletion"); });
    REQUIRE(fixture.playback.snapshot().transport.nowPlaying.title == "Edited before deletion");
    fixture.commit(
      [&](library::LibraryWrite& write) -> Result<OperationOutcome<bool>>
      {
        REQUIRE(write.tracks().remove(trackId));
        return Changed<bool>{.value = true, .changeSet = {.tracksDeleted = {trackId}}};
      });
    auto expected = before;
    expected.transport.nowPlaying.coverArtId = kInvalidResourceId;
    CHECK(fixture.playback.snapshot() == expected);
  }

  TEST_CASE("PlaybackService metadata - unrelated and unchanged presentation edits publish nothing",
            "[runtime][regression][playback][concurrency]")
  {
    auto fixture = PlaybackMetadataFixture{};
    auto const trackId = fixture.addTrack("Launch");
    auto const otherId = fixture.addTrack("Other");
    fixture.start(trackId);
    auto const before = fixture.playback.snapshot();
    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto const subscription =
      fixture.playback.events().onSnapshot([&](auto const& snapshot) { snapshots.push_back(snapshot); });
    fixture.mutate(otherId, [](library::TrackBuilder& builder) { builder.metadata().title("Other edited"); });
    fixture.mutate(trackId, [](library::TrackBuilder& builder) { builder.metadata().year(2026); });
    CHECK(snapshots.empty());
    CHECK(fixture.playback.snapshot() == before);
  }

  TEST_CASE("PlaybackService metadata - prepared successors publish current metadata on activation",
            "[runtime][regression][playback][concurrency]")
  {
    auto fixture = PlaybackMetadataFixture{};
    auto const currentId = fixture.addTrack("Current");
    auto const nextId = fixture.addTrack("Next");
    fixture.start(currentId);
    auto const nextRequestRes = playbackRequestForTrack(fixture.transport.libraryFixture.library(), nextId);
    REQUIRE(nextRequestRes);
    REQUIRE(fixture.transport.playbackTransport.prepareNext(*nextRequestRes, ListId{7}));
    fixture.mutate(nextId,
                   [](library::TrackBuilder& builder)
                   {
                     builder.metadata().title("Edited next").artist("Next artist").album("Next album");
                     builder.coverArt().clear();
                   });
    auto const deleted = GENERATE(false, true);

    if (deleted)
    {
      fixture.commit(
        [&](library::LibraryWrite& write) -> Result<OperationOutcome<bool>>
        {
          REQUIRE(write.tracks().remove(nextId));
          return Changed<bool>{.value = true, .changeSet = {.tracksDeleted = {nextId}}};
        });
    }

    CHECK(fixture.playback.snapshot().transport.nowPlaying.trackId == currentId);
    auto const beforeActivation = fixture.playback.snapshot();
    auto output = std::array<std::byte, 4096>{};
    REQUIRE(fixture.transport.renderTarget != nullptr);
    REQUIRE(tryDriveRenderUntil(*fixture.transport.renderTarget,
                                fixture.transport.executor,
                                output,
                                [&] { return fixture.playback.snapshot().transport.nowPlaying.trackId == nextId; }));
    CHECK(fixture.playback.snapshot().transport.nowPlaying.title == (deleted ? "Next" : "Edited next"));
    CHECK(fixture.playback.snapshot().transport.nowPlaying.artist == (deleted ? "Launch artist" : "Next artist"));
    CHECK(fixture.playback.snapshot().transport.nowPlaying.album == (deleted ? "Launch album" : "Next album"));
    CHECK(fixture.playback.snapshot().transport.nowPlaying.sourceListId == ListId{7});
    CHECK(fixture.playback.snapshot().transport.positionRevision != beforeActivation.transport.positionRevision);
    CHECK(fixture.playback.snapshot().transport.finalSeekRevision == beforeActivation.transport.finalSeekRevision);
    CHECK(fixture.playback.snapshot().transport.nowPlaying.coverArtId == kInvalidResourceId);
    CHECK(fixture.transport.playbackTransport.state().nowPlaying.title == "Next");
  }

  TEST_CASE("PlaybackService metadata - live succession edits defer observer commands",
            "[runtime][regression][playback][concurrency]")
  {
    auto fixture = ApplicationPlaybackFixtureT<QueuedExecutor>{};
    auto const trackId = fixture.commandsFixture.addTrack({.title = "Launch"});
    fixture.addReadyProvider();
    fixture.executor.drain();
    auto const viewRes = fixture.workspace.navigate({.target = kAllTracksListId});
    REQUIRE(viewRes);
    REQUIRE(fixture.commands().startFromView(*viewRes, trackId));
    REQUIRE(fixture.executor.tryDrainUntil(
      [&] { return fixture.playback.snapshot().transport.transport == audio::Transport::Playing; }));
    fixture.executor.drain();
    auto const before = fixture.playback.snapshot();
    auto snapshots = std::vector<PlaybackSnapshot>{};
    bool requestedPause = false;
    bool stayedPlayingInsideObserver = false;
    auto const subscription = fixture.playback.events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot)
      {
        snapshots.push_back(snapshot);

        if (snapshot.transport.nowPlaying.title == "Edited" && !requestedPause)
        {
          requestedPause = true;
          fixture.commands().pause();
          stayedPlayingInsideObserver = fixture.playback.snapshot().transport.transport == audio::Transport::Playing;
        }
      });
    REQUIRE(fixture.commandsFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Edited"}));
    fixture.executor.drain();
    REQUIRE(snapshots.size() == 2);
    auto expected = before;
    expected.transport.nowPlaying.title = "Edited";
    CHECK(snapshots.front() == expected);
    CHECK(stayedPlayingInsideObserver);
    CHECK(snapshots.back().transport.transport == audio::Transport::Paused);
    CHECK(snapshots.back().transport.nowPlaying.title == "Edited");
  }

  TEST_CASE("PlaybackService metadata - settlement observers cannot lose a committed library edit",
            "[runtime][regression][playback][concurrency]")
  {
    auto fixture = PlaybackMetadataFixture{};
    auto const trackId = fixture.addTrack("Launch");
    fixture.start(trackId);
    auto snapshots = std::vector<PlaybackSnapshot>{};
    bool edited = false;
    auto const subscription = fixture.playback.events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot)
      {
        snapshots.push_back(snapshot);

        if (!edited)
        {
          edited = true;
          // Complete a real write and its library publication before returning
          // from the command's snapshot observer.
          fixture.mutate(trackId, [](library::TrackBuilder& builder) { builder.metadata().title("Reentrant edit"); });
        }
      });
    fixture.playback.commands().pause();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots.front().transport.nowPlaying.title == "Launch");
    fixture.transport.executor.drain();
    REQUIRE(snapshots.size() == 2);
    auto expected = snapshots.front();
    expected.transport.nowPlaying.title = "Reentrant edit";
    CHECK(snapshots.back() == expected);
  }

  TEST_CASE("PlaybackService metadata - clearing the active title does not mark the view inactive",
            "[runtime][regression][playback][concurrency]")
  {
    auto fixture = PlaybackMetadataFixture{};
    auto const trackId = fixture.addTrack("Launch");
    fixture.start(trackId);
    auto rendered = uimodel::NowPlayingViewState{};
    auto const viewModel = uimodel::NowPlayingViewModel{
      fixture.playback, ao::test::englishMessageCatalog(), [&](auto const& view) { rendered = view; }};
    fixture.mutate(trackId, [](library::TrackBuilder& builder) { builder.metadata().title(""); });
    CHECK(fixture.playback.snapshot().transport.nowPlaying.title.empty());
    CHECK(rendered.title.empty());
    CHECK(rendered.isActive);
  }

  TEST_CASE("PlaybackService metadata - destruction retires a queued library refresh",
            "[runtime][regression][playback][concurrency]")
  {
    auto fixture = ApplicationPlaybackFixtureT<QueuedExecutor>{};
    auto const trackId = fixture.commandsFixture.addTrack({.title = "Launch"});
    fixture.addReadyProvider();
    fixture.executor.drain();
    auto const viewRes = fixture.workspace.navigate({.target = kAllTracksListId});
    REQUIRE(viewRes);
    REQUIRE(fixture.commands().startFromView(*viewRes, trackId));
    REQUIRE(fixture.executor.tryDrainUntil(
      [&] { return fixture.playback.snapshot().transport.transport == audio::Transport::Playing; }));
    fixture.executor.drain();
    std::size_t publications = 0;
    auto const subscription = fixture.playback.events().onSnapshot([&](PlaybackSnapshot const&) { ++publications; });
    auto const retirement = fixture.changes.onChanged(
      [&](LibraryChangeSet const& changeSet)
      {
        if (std::ranges::contains(changeSet.tracksMutated, trackId))
        {
          fixture.playbackStoragePtr.reset();
        }
      });
    REQUIRE(fixture.commandsFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Edited"}));
    fixture.executor.drain();
    CHECK(fixture.playbackStoragePtr == nullptr);
    CHECK(publications == 0);
  }
} // namespace ao::rt::test
