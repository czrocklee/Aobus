// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/audio/Transport.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    template<typename T>
    concept HasTrackOnlyStart = requires(T& target) { target.playTrack(kInvalidTrackId, kInvalidListId); };

    template<typename T>
    concept HasTrackOnlyPreparation = requires(T& target) { target.prepareNext(kInvalidTrackId, kInvalidListId); };

    template<typename T>
    concept HasPreparedNextClear = requires(T& target) { target.clearPreparedNext(); };

    template<typename T>
    concept HasLegacySuccessionAccessor = requires(T& target) { target.playbackSequence(); };

    struct PlaybackServiceFixture final
    {
      PlaybackServiceFixture() = default;

      PlaybackServiceFixture(PlaybackServiceFixture const&) = delete;
      PlaybackServiceFixture& operator=(PlaybackServiceFixture const&) = delete;
      PlaybackServiceFixture(PlaybackServiceFixture&&) = delete;
      PlaybackServiceFixture& operator=(PlaybackServiceFixture&&) = delete;
      ~PlaybackServiceFixture() = default;

      LibraryWriter& writer() { return writerFixture.writer(); }

      TrackId addPlayableTrack(std::string title)
      {
        auto const playableUri = std::format("playable-{}.flac", nextPlayableFile++);
        audio::test::installAudioFixture(libraryFixture.root(), "basic_metadata.flac", playableUri);
        return libraryFixture.addTrack(
          library::test::TrackSpec{.title = std::move(title), .uri = playableUri, .codec = AudioCodec::Flac});
      }

      void buildThreeTrackManualView()
      {
        firstTrackId = addPlayableTrack("First");
        secondTrackId = addPlayableTrack("Second");
        thirdTrackId = addPlayableTrack("Third");
        sources.reloadAllTracks();
        listId = ao::test::requireValue(writer().createList(LibraryWriter::ListDraft{
          .kind = LibraryWriter::ListKind::Manual,
          .name = "Playback order",
          .trackIds = {firstTrackId, secondTrackId, thirdTrackId},
        }));
        viewId = ao::test::requireValue(views.createView({.listId = listId}, true)).viewId;
        successionPtr = std::make_unique<PlaybackSuccession>(
          executor, views, sources, libraryFixture.library(), playbackTransport, notifications, asyncRuntime);
        playbackBootstrapPtr = std::make_unique<PlaybackBootstrap>(playbackTransport);
        playbackBootstrapPtr->addProvider(makeReadyAudioProvider());
        playbackPtr = playbackBootstrapPtr->createPlaybackService(executor, *successionPtr);
      }

      PlaybackCommands& commands() const { return playbackPtr->commands(); }
      PlaybackService& playback() const { return *playbackPtr; }

      MusicLibraryFixture libraryFixture;
      ControlledSleeper sleeper;
      InlineExecutor executor;
      async::Runtime asyncRuntime{executor, 1, {}, &sleeper};
      LibraryChanges changes;
      LibraryWriterFixture writerFixture{libraryFixture.library(), changes};
      TrackSourceCache sources{libraryFixture.library(), changes};
      ViewService views{executor, libraryFixture.library(), sources};
      NotificationService notifications{asyncRuntime};
      PlaybackTransport playbackTransport{makePlaybackTransport(executor, libraryFixture.library(), notifications)};
      std::unique_ptr<PlaybackSuccession> successionPtr;
      std::unique_ptr<PlaybackBootstrap> playbackBootstrapPtr;
      std::unique_ptr<PlaybackService> playbackPtr;

      TrackId firstTrackId = kInvalidTrackId;
      TrackId secondTrackId = kInvalidTrackId;
      TrackId thirdTrackId = kInvalidTrackId;
      ListId listId = kInvalidListId;
      ViewId viewId = kInvalidViewId;
      std::uint32_t nextPlayableFile = 0;
    };
  } // namespace

  TEST_CASE("PlaybackService - public commands cannot bypass succession", "[runtime][unit][playback][boundary]")
  {
    // The internal transport still supports focused collaborator tests, while
    // neither public entry point can create a transport-only playback subject.
    STATIC_REQUIRE(HasTrackOnlyStart<PlaybackTransport>);
    STATIC_REQUIRE(HasTrackOnlyPreparation<PlaybackTransport>);
    STATIC_REQUIRE(HasPreparedNextClear<PlaybackTransport>);
    STATIC_REQUIRE(!HasTrackOnlyStart<PlaybackService>);
    STATIC_REQUIRE(!HasTrackOnlyStart<PlaybackCommands>);
    STATIC_REQUIRE(!HasTrackOnlyPreparation<PlaybackService>);
    STATIC_REQUIRE(!HasTrackOnlyPreparation<PlaybackCommands>);
    STATIC_REQUIRE(!HasPreparedNextClear<PlaybackService>);
    STATIC_REQUIRE(!HasPreparedNextClear<PlaybackCommands>);
    STATIC_REQUIRE(!HasLegacySuccessionAccessor<AppRuntime>);
    STATIC_REQUIRE(std::same_as<decltype(std::declval<AppRuntime&>().playback()), PlaybackService&>);
  }

  TEST_CASE("PlaybackService - a view start publishes one coherent snapshot", "[runtime][unit][playback][coherence]")
  {
    auto fixture = PlaybackServiceFixture{};
    fixture.buildThreeTrackManualView();

    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto const subscription = fixture.playback().events().onSnapshot([&snapshots](PlaybackSnapshot const& snapshot)
                                                                     { snapshots.push_back(snapshot); });

    auto const started = fixture.commands().startFromView(fixture.viewId, fixture.firstTrackId);
    REQUIRE(started);

    // One accepted command publishes exactly one snapshot even though it drives
    // several lower transport and succession signals.
    REQUIRE(snapshots.size() == 1);

    auto const& published = snapshots.front();
    CHECK(published.revision.value == 1);
    // The transport subject and the succession subject describe one track.
    CHECK(published.transport.nowPlaying.trackId == fixture.firstTrackId);
    CHECK(published.succession.currentTrackId == fixture.firstTrackId);
    CHECK(published.succession.sourceState == PlaybackSourceState::Live);
    CHECK(published.transport.transport != audio::Transport::Idle);
    // Output readiness and quality are captured inside the same revisioned
    // value rather than arriving as independently correlated public state.
    CHECK(published.transport.output == fixture.playbackTransport.state().output);
    CHECK(published.transport.ready == fixture.playbackTransport.state().ready);
    CHECK(published.transport.quality == fixture.playbackTransport.state().quality);

    // snapshot() reflects the same coherent state on demand.
    auto const observed = fixture.playback().snapshot();
    CHECK(observed.revision.value == 1);
    CHECK(observed.sameContentAs(published));
  }

  TEST_CASE("PlaybackService - revisions advance monotonically on real change", "[runtime][unit][playback][coherence]")
  {
    auto fixture = PlaybackServiceFixture{};
    fixture.buildThreeTrackManualView();

    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto const subscription = fixture.playback().events().onSnapshot([&snapshots](PlaybackSnapshot const& snapshot)
                                                                     { snapshots.push_back(snapshot); });

    REQUIRE(fixture.commands().startFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots.back().revision.value == 1);

    fixture.commands().setShuffleMode(ShuffleMode::On);
    REQUIRE(snapshots.size() == 2);
    CHECK(snapshots.back().revision.value == 2);
    CHECK(snapshots.back().succession.shuffle == ShuffleMode::On);

    // Re-issuing the same shuffle mode changes no content and must not publish
    // or advance the revision.
    fixture.commands().setShuffleMode(ShuffleMode::On);
    CHECK(snapshots.size() == 2);
    CHECK(fixture.playback().snapshot().revision.value == 2);

    fixture.commands().stop();
    REQUIRE(snapshots.size() == 3);
    CHECK(snapshots.back().revision.value == 3);
    CHECK(snapshots.back().transport.transport == audio::Transport::Idle);
    CHECK(snapshots.back().succession.sourceState == PlaybackSourceState::Inactive);
  }

  TEST_CASE("PlaybackService - spontaneous lower changes coalesce at the end of one executor turn",
            "[runtime][unit][playback][coherence]")
  {
    auto fixture = ApplicationPlaybackFixtureT<QueuedExecutor>{};
    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto const subscription = fixture.playback.events().onSnapshot([&snapshots](PlaybackSnapshot const& snapshot)
                                                                   { snapshots.push_back(snapshot); });

    // These calls intentionally bypass PlaybackService to model independent lower-layer
    // observations arriving within one callback-executor turn.
    fixture.succession.setShuffleMode(ShuffleMode::On);
    fixture.succession.setRepeatMode(RepeatMode::All);

    CHECK(snapshots.empty());

    fixture.executor.drain();

    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots.front().revision.value == 1);
    CHECK(snapshots.front().succession.shuffle == ShuffleMode::On);
    CHECK(snapshots.front().succession.repeat == RepeatMode::All);
  }
} // namespace ao::rt::test
