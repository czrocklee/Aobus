// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/PlaybackTransportTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Property.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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

    class RejectingDeferExecutor final : public async::Executor
    {
    public:
      bool isCurrent() const noexcept override { return _delegate.isCurrent(); }
      void dispatch(std::move_only_function<void()> task) override { _delegate.dispatch(std::move(task)); }

      void defer(std::move_only_function<void()> task) override
      {
        if (_rejectNextDefer.exchange(false))
        {
          throwException<Exception>("scripted defer rejection");
        }

        _delegate.defer(std::move(task));
      }

      void rejectNextDefer() noexcept { _rejectNextDefer.store(true); }
      void drain() { _delegate.drain(); }

    private:
      QueuedExecutor _delegate;
      std::atomic_bool _rejectNextDefer{false};
    };

    class ThrowingVolumeArm final
    {
    public:
      void arm() noexcept { _armed = true; }
      bool consume() noexcept { return std::exchange(_armed, false); }

    private:
      bool _armed = false;
    };

    class ThrowingVolumeBackend final : public audio::NullBackend
    {
    public:
      explicit ThrowingVolumeBackend(ThrowingVolumeArm& arm)
        : _arm{&arm}
      {
      }

      audio::BackendId backendId() const override { return audio::BackendId{"throwing_backend"}; }
      audio::ProfileId profileId() const override { return audio::ProfileId{audio::kProfileShared}; }

      Result<> setProperty(audio::PropertyId const id, audio::PropertyValue const& value) override
      {
        if (id == audio::PropertyId::Volume && _arm->consume())
        {
          throwException<Exception>("scripted volume exception");
        }

        return NullBackend::setProperty(id, value);
      }

    private:
      ThrowingVolumeArm* _arm;
    };

    class ThrowingVolumeProvider final : public audio::BackendProvider
    {
    public:
      explicit ThrowingVolumeProvider(ThrowingVolumeArm& arm)
        : _arm{&arm}
      {
        _status.descriptor.id = audio::BackendId{"throwing_backend"};
        _status.descriptor.supportedProfiles.push_back({.id = audio::kProfileShared});
        _status.devices.push_back(audio::Device{
          .id = audio::DeviceId{"throwing_device"},
          .displayName = "Throwing Device",
          .description = "Controlled exception output",
          .isDefault = false,
          .backendId = audio::BackendId{"throwing_backend"},
        });
      }

      void shutdown() noexcept override {}

      audio::Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        callback(_status.devices);
        return {};
      }

      Status status() const override { return _status; }

      std::unique_ptr<audio::Backend> createBackend(audio::Device const& /*device*/,
                                                    audio::ProfileId const& /*profile*/) override
      {
        return std::make_unique<ThrowingVolumeBackend>(*_arm);
      }

      audio::Subscription subscribeGraph(std::string_view /*routeAnchor*/, OnGraphChangedCallback /*callback*/) override
      {
        return {};
      }

    private:
      ThrowingVolumeArm* _arm;
      Status _status;
    };

    template<typename ExecutorT = QueuedExecutor>
    struct PlaybackServiceFixture final
    {
      PlaybackServiceFixture() = default;

      PlaybackServiceFixture(PlaybackServiceFixture const&) = delete;
      PlaybackServiceFixture& operator=(PlaybackServiceFixture const&) = delete;
      PlaybackServiceFixture(PlaybackServiceFixture&&) = delete;
      PlaybackServiceFixture& operator=(PlaybackServiceFixture&&) = delete;
      ~PlaybackServiceFixture() = default;

      LibraryWriter& writer() { return application.writer(); }

      TrackId addPlayableTrack(std::string title)
      {
        auto const playableUri = std::format("playable-{}.flac", nextPlayableFile++);
        audio::test::installAudioFixture(application.libraryFixture.root(), "basic_metadata.flac", playableUri);
        return application.libraryFixture.addTrack(
          library::test::TrackSpec{.title = std::move(title), .uri = playableUri, .codec = AudioCodec::Flac});
      }

      void buildThreeTrackManualView()
      {
        firstTrackId = addPlayableTrack("First");
        secondTrackId = addPlayableTrack("Second");
        thirdTrackId = addPlayableTrack("Third");
        application.sources.reloadAllTracks();
        listId = ao::test::requireValue(writer().createList(LibraryWriter::ListDraft{
          .kind = LibraryWriter::ListKind::Manual,
          .name = "Playback order",
          .trackIds = {firstTrackId, secondTrackId, thirdTrackId},
        }));
        viewId = ao::test::requireValue(application.views.createView({.listId = listId}));
        application.addReadyProvider();
        application.executor.drain();
      }

      PlaybackCommands& commands() { return application.commands(); }
      PlaybackService& playback() { return application.playback; }
      bool waitForTrack(TrackId const trackId)
      {
        auto const settled =
          waitForPlaybackSettlement(application.executor,
                                    observedPositionRevision,
                                    [this] { return playback().snapshot().transport.positionRevision; });
        observedPositionRevision = playback().snapshot().transport.positionRevision;
        return settled && playback().snapshot().transport.nowPlaying.trackId == trackId;
      }

      ApplicationPlaybackFixtureT<ExecutorT> application;

      TrackId firstTrackId = kInvalidTrackId;
      TrackId secondTrackId = kInvalidTrackId;
      TrackId thirdTrackId = kInvalidTrackId;
      ListId listId = kInvalidListId;
      ViewId viewId = kInvalidViewId;
      std::uint32_t nextPlayableFile = 0;
      PlaybackPositionRevision observedPositionRevision{};
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
    STATIC_REQUIRE(std::same_as<decltype(std::declval<PlaybackService const&>().snapshot()), PlaybackSnapshot const&>);
  }

  TEST_CASE("PlaybackService - a view start publishes one coherent snapshot", "[runtime][unit][playback][coherence]")
  {
    auto fixture = PlaybackServiceFixture<>{};
    fixture.buildThreeTrackManualView();

    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto const subscription = fixture.playback().events().onSnapshot([&snapshots](PlaybackSnapshot const& snapshot)
                                                                     { snapshots.push_back(snapshot); });

    auto const started = fixture.commands().startFromView(fixture.viewId, fixture.firstTrackId);
    REQUIRE(started);
    REQUIRE(fixture.waitForTrack(fixture.firstTrackId));

    // One accepted command publishes exactly one snapshot even though it drives
    // several lower transport and succession signals.
    REQUIRE(snapshots.size() == 1);

    auto const& published = snapshots.front();
    // The transport subject and the succession subject describe one track.
    CHECK(published.transport.nowPlaying.trackId == fixture.firstTrackId);
    CHECK(published.succession.currentTrackId == fixture.firstTrackId);
    CHECK(published.succession.sourceState == PlaybackSourceState::Live);
    CHECK(published.transport.transport != audio::Transport::Idle);
    // Output readiness and quality are captured inside the same value rather
    // than arriving as independently correlated public state.
    CHECK(published.transport.output == fixture.application.playbackTransport.state().output);
    CHECK(published.transport.ready == fixture.application.playbackTransport.state().ready);
    CHECK(published.transport.quality == fixture.application.playbackTransport.state().quality);

    // snapshot() reflects the same coherent state on demand.
    auto const observed = fixture.playback().snapshot();
    CHECK(observed == published);
  }

  TEST_CASE("PlaybackService - real changes publish once and no-ops publish none",
            "[runtime][unit][playback][coherence]")
  {
    auto fixture = PlaybackServiceFixture<>{};
    fixture.buildThreeTrackManualView();

    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto const subscription = fixture.playback().events().onSnapshot([&snapshots](PlaybackSnapshot const& snapshot)
                                                                     { snapshots.push_back(snapshot); });

    REQUIRE(fixture.commands().startFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(fixture.waitForTrack(fixture.firstTrackId));
    REQUIRE(snapshots.size() == 1);

    fixture.commands().setShuffleMode(ShuffleMode::On);
    REQUIRE(snapshots.size() == 2);
    CHECK(snapshots.back().succession.shuffle == ShuffleMode::On);

    // Re-issuing the same shuffle mode changes no content and must not publish.
    fixture.commands().setShuffleMode(ShuffleMode::On);
    CHECK(snapshots.size() == 2);
    CHECK(fixture.playback().snapshot() == snapshots.back());

    fixture.commands().stop();
    REQUIRE(snapshots.size() == 3);
    CHECK(snapshots.back().transport.transport == audio::Transport::Idle);
    CHECK(snapshots.back().succession.sourceState == PlaybackSourceState::Inactive);
  }

  TEST_CASE("PlaybackSnapshot - elapsed clock samples do not define content equality",
            "[runtime][regression][playback][snapshot]")
  {
    auto before = PlaybackSnapshot{};
    before.transport.elapsed = std::chrono::milliseconds{100};
    auto after = before;
    after.transport.elapsed = std::chrono::milliseconds{900};

    CHECK(after == before);

    after.transport.positionRevision = PlaybackPositionRevision{.value = 1};
    CHECK_FALSE(after == before);
  }

  TEST_CASE("PlaybackService - final seeks advance explicit position identities",
            "[runtime][regression][playback][seek]")
  {
    auto fixture = PlaybackServiceFixture<>{};
    fixture.buildThreeTrackManualView();
    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto const subscription = fixture.playback().events().onSnapshot([&snapshots](PlaybackSnapshot const& snapshot)
                                                                     { snapshots.push_back(snapshot); });

    REQUIRE(fixture.commands().startFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(fixture.waitForTrack(fixture.firstTrackId));
    REQUIRE(snapshots.size() == 1);
    auto const started = snapshots.back();
    CHECK(started.transport.positionRevision.value == 1);
    CHECK(started.transport.finalSeekRevision.value == 0);

    fixture.commands().seek(std::chrono::milliseconds{250});
    REQUIRE(snapshots.size() == 2);
    CHECK(snapshots.back().transport.elapsed == std::chrono::milliseconds{250});
    CHECK(snapshots.back().transport.positionRevision.value == started.transport.positionRevision.value + 1);
    CHECK(snapshots.back().transport.finalSeekRevision.value == started.transport.finalSeekRevision.value + 1);

    fixture.commands().setVolume(0.5F);
    REQUIRE(snapshots.size() == 3);
    CHECK(snapshots.back().transport.positionRevision == snapshots[1].transport.positionRevision);
    CHECK(snapshots.back().transport.finalSeekRevision == snapshots[1].transport.finalSeekRevision);
  }

  TEST_CASE("PlaybackService - elapsed refresh does not create position or availability changes",
            "[runtime][regression][playback][snapshot]")
  {
    auto fixture = PlaybackTransportFixture<QueuedExecutor>{};
    auto changes = LibraryChanges{};
    auto sources = TrackSourceCache{fixture.libraryFixture.library(), changes};
    auto views = ViewService{fixture.executor, fixture.libraryFixture.library(), sources};
    auto succession = PlaybackSuccession{fixture.executor,
                                         views,
                                         sources,
                                         fixture.libraryFixture.library(),
                                         fixture.playbackTransport,
                                         fixture.notificationService,
                                         fixture.asyncRuntime};
    auto bootstrap = PlaybackBootstrap{fixture.playbackTransport};
    auto playbackPtr = bootstrap.createPlaybackService(fixture.executor, succession);
    auto commandSurface = uimodel::PlaybackCommandSurface{*playbackPtr, [] {}};
    std::size_t availabilityChanged = 0;
    auto const availabilitySubscription =
      commandSurface.onAvailabilityChanged([&availabilityChanged] { ++availabilityChanged; });
    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto const snapshotSubscription = playbackPtr->events().onSnapshot([&snapshots](PlaybackSnapshot const& snapshot)
                                                                       { snapshots.push_back(snapshot); });

    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.executor.drain();
    auto const fixtureUri = fixture.installAudioFixture();
    auto const trackId = fixture.libraryFixture.addTrack({.title = "Clock", .uri = fixtureUri});
    REQUIRE(fixture.playbackTransport.playTrack(trackId, ListId{7}));
    REQUIRE(fixture.executor.drainUntil(
      [&fixture] { return fixture.playbackTransport.state().transport == audio::Transport::Playing; }));
    fixture.executor.drain();
    REQUIRE(fixture.renderTarget != nullptr);

    auto const before = playbackPtr->snapshot();
    snapshots.clear();
    availabilityChanged = 0;
    auto output = std::array<std::byte, 4096>{};
    auto const renderResult = fixture.renderTarget->renderPcm(output);
    REQUIRE(renderResult.bytesWritten > 0);
    fixture.renderTarget->handlePositionAdvanced(renderResult.positionFrames);

    playbackPtr->commands().setVolume(0.5F);

    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots.back().transport.elapsed > before.transport.elapsed);
    CHECK(snapshots.back().transport.positionRevision == before.transport.positionRevision);
    CHECK(snapshots.back().transport.finalSeekRevision == before.transport.finalSeekRevision);
    CHECK(availabilityChanged == 0);

    auto const afterVolume = snapshots.back();
    snapshots.clear();
    playbackPtr->commands().setShuffleMode(ShuffleMode::On);

    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots.back().succession.shuffle == ShuffleMode::On);
    CHECK(snapshots.back().transport.positionRevision == afterVolume.transport.positionRevision);
    CHECK(snapshots.back().transport.finalSeekRevision == afterVolume.transport.finalSeekRevision);
  }

  TEST_CASE("PlaybackService - spontaneous lower changes coalesce at the end of one executor turn",
            "[runtime][unit][playback][coherence]")
  {
    auto fixture = ApplicationPlaybackFixtureT<QueuedExecutor>{};
    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto const subscription = fixture.playback.events().onSnapshot([&snapshots](PlaybackSnapshot const& snapshot)
                                                                   { snapshots.push_back(snapshot); });
    auto const before = fixture.playback.snapshot();

    // These calls intentionally bypass PlaybackService to model independent lower-layer
    // observations arriving within one callback-executor turn.
    fixture.succession.setShuffleMode(ShuffleMode::On);
    fixture.succession.setRepeatMode(RepeatMode::All);

    CHECK(snapshots.empty());
    CHECK(fixture.playback.snapshot() == before);

    fixture.executor.drain();

    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots.front().succession.shuffle == ShuffleMode::On);
    CHECK(snapshots.front().succession.repeat == RepeatMode::All);
  }

  TEST_CASE("PlaybackService - current snapshot updates without subscribers", "[runtime][unit][playback][coherence]")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto const before = fixture.playback.snapshot();

    fixture.commands().setShuffleMode(ShuffleMode::On);

    auto const after = fixture.playback.snapshot();
    CHECK(after != before);
    CHECK(after.succession.shuffle == ShuffleMode::On);
  }

  TEST_CASE("PlaybackService - a no-op stop does not publish", "[runtime][unit][playback][coherence]")
  {
    auto fixture = ApplicationPlaybackFixture{};
    // The first stop synchronizes the transport snapshot with the underlying
    // Player. Repeating it is the semantic no-op under test.
    fixture.commands().stop();

    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto const subscription = fixture.playback.events().onSnapshot([&snapshots](PlaybackSnapshot const& snapshot)
                                                                   { snapshots.push_back(snapshot); });
    auto const before = fixture.playback.snapshot();

    fixture.commands().stop();

    CHECK(snapshots.empty());
    CHECK(fixture.playback.snapshot() == before);
  }

  TEST_CASE("PlaybackService - same-track navigation commits a new position anchor",
            "[runtime][regression][playback][coherence]")
  {
    auto fixture = PlaybackServiceFixture<>{};
    fixture.buildThreeTrackManualView();
    REQUIRE(fixture.commands().startFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(fixture.waitForTrack(fixture.firstTrackId));
    fixture.commands().setRepeatMode(RepeatMode::One);

    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto const subscription = fixture.playback().events().onSnapshot([&snapshots](PlaybackSnapshot const& snapshot)
                                                                     { snapshots.push_back(snapshot); });
    auto const before = fixture.playback().snapshot();

    fixture.commands().next();

    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots.back().transport.nowPlaying.trackId == fixture.firstTrackId);
    CHECK(snapshots.back().transport.positionRevision.value == before.transport.positionRevision.value + 1);
    CHECK(snapshots.back().transport.finalSeekRevision == before.transport.finalSeekRevision);
  }

  TEST_CASE("PlaybackService - silent lower navigation still commits its position anchor",
            "[runtime][regression][playback][coherence]")
  {
    auto fixture = PlaybackServiceFixture<QueuedExecutor>{};
    fixture.buildThreeTrackManualView();
    fixture.application.executor.drain();
    REQUIRE(fixture.commands().startFromView(fixture.viewId, fixture.firstTrackId));
    REQUIRE(fixture.waitForTrack(fixture.firstTrackId));

    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto const subscription = fixture.playback().events().onSnapshot([&snapshots](PlaybackSnapshot const& snapshot)
                                                                     { snapshots.push_back(snapshot); });
    auto const before = fixture.playback().snapshot();

    REQUIRE(fixture.application.succession.next());
    fixture.application.executor.drain();

    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots.back().transport.nowPlaying.trackId == fixture.secondTrackId);
    CHECK(snapshots.back().transport.positionRevision.value == before.transport.positionRevision.value + 1);
    CHECK(snapshots.back().transport.finalSeekRevision == before.transport.finalSeekRevision);
  }

  TEST_CASE("PlaybackService - navigation without a session is a no-op", "[runtime][unit][playback][coherence]")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto snapshots = std::vector<PlaybackSnapshot>{};
    auto const subscription = fixture.playback.events().onSnapshot([&snapshots](PlaybackSnapshot const& snapshot)
                                                                   { snapshots.push_back(snapshot); });
    auto const before = fixture.playback.snapshot();

    fixture.commands().next();
    fixture.commands().previous();

    CHECK(snapshots.empty());
    CHECK(fixture.playback.snapshot() == before);
  }

  TEST_CASE("PlaybackService - snapshot observer commands run in a later executor turn",
            "[runtime][unit][playback][concurrency]")
  {
    auto fixture = ApplicationPlaybackFixtureT<QueuedExecutor>{};
    auto snapshots = std::vector<PlaybackSnapshot>{};
    bool requestedRepeat = false;
    auto const subscription = fixture.playback.events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot)
      {
        snapshots.push_back(snapshot);

        if (!requestedRepeat && snapshot.succession.shuffle == ShuffleMode::On)
        {
          requestedRepeat = true;
          fixture.commands().setRepeatMode(RepeatMode::All);
        }
      });

    fixture.commands().setShuffleMode(ShuffleMode::On);

    REQUIRE(snapshots.size() == 1);
    CHECK(fixture.playback.snapshot().succession.repeat == RepeatMode::Off);
    CHECK(fixture.executor.queuedCount() != 0);

    fixture.executor.drain();

    REQUIRE(snapshots.size() == 2);
    CHECK(snapshots.back().succession.repeat == RepeatMode::All);
  }

  TEST_CASE("PlaybackService - later commands do not overtake queued observer commands",
            "[runtime][regression][playback][concurrency]")
  {
    auto fixture = ApplicationPlaybackFixtureT<QueuedExecutor>{};
    bool queuedRepeat = false;
    auto const subscription = fixture.playback.events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot)
      {
        if (!queuedRepeat && snapshot.succession.shuffle == ShuffleMode::On)
        {
          queuedRepeat = true;
          fixture.commands().setRepeatMode(RepeatMode::All);
        }
      });

    fixture.commands().setShuffleMode(ShuffleMode::On);
    REQUIRE(queuedRepeat);

    // This later admission must remain behind the observer command that is
    // already waiting for its deferred executor turn.
    fixture.commands().setRepeatMode(RepeatMode::One);
    fixture.executor.drain();

    CHECK(fixture.playback.snapshot().succession.repeat == RepeatMode::One);
  }

  TEST_CASE("PlaybackService - rejected drain scheduling preserves queued command order",
            "[runtime][regression][playback][concurrency]")
  {
    auto fixture = ApplicationPlaybackFixtureT<RejectingDeferExecutor>{};
    bool queuedRepeat = false;
    auto const subscription = fixture.playback.events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot)
      {
        if (!queuedRepeat && snapshot.succession.shuffle == ShuffleMode::On)
        {
          queuedRepeat = true;
          fixture.executor.rejectNextDefer();
          fixture.commands().setRepeatMode(RepeatMode::All);
        }
      });

    // Model a spontaneous lower-layer settlement. Its publication admits the
    // observer command, then the controlled executor rejects that command's
    // first drain request.
    fixture.succession.setShuffleMode(ShuffleMode::On);
    REQUIRE_THROWS_AS(fixture.executor.drain(), Exception);
    REQUIRE(queuedRepeat);
    CHECK(fixture.playback.snapshot().succession.shuffle == ShuffleMode::On);
    CHECK(fixture.playback.snapshot().succession.repeat == RepeatMode::Off);

    fixture.commands().setRepeatMode(RepeatMode::One);
    fixture.executor.drain();

    CHECK(fixture.playback.snapshot().succession.repeat == RepeatMode::One);
  }

  TEST_CASE("PlaybackService - queued command exceptions do not strand later commands",
            "[runtime][regression][playback][concurrency]")
  {
    // The arm must outlive the backend that borrows it.
    auto arm = ThrowingVolumeArm{};
    auto fixture = PlaybackServiceFixture<QueuedExecutor>{};
    fixture.buildThreeTrackManualView();
    fixture.application.executor.drain();
    fixture.application.playbackBootstrap.addProvider(std::make_unique<ThrowingVolumeProvider>(arm));
    fixture.application.executor.drain();
    fixture.commands().setOutputDevice(audio::BackendId{"throwing_backend"},
                                       audio::DeviceId{"throwing_device"},
                                       audio::ProfileId{audio::kProfileShared});
    fixture.application.executor.drain();
    bool queuedCommands = false;
    auto const subscription = fixture.playback().events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot)
      {
        if (!queuedCommands && snapshot.succession.shuffle == ShuffleMode::On)
        {
          queuedCommands = true;
          fixture.commands().setVolume(0.25F);
          fixture.commands().setRepeatMode(RepeatMode::All);
        }
      });
    arm.arm();

    fixture.commands().setShuffleMode(ShuffleMode::On);
    REQUIRE(queuedCommands);
    REQUIRE_THROWS_AS(fixture.application.executor.drain(), Exception);

    fixture.application.executor.drain();

    CHECK(fixture.playback().snapshot().succession.repeat == RepeatMode::All);
  }

  TEST_CASE("PlaybackService - a newer stop supersedes an observer-queued start",
            "[runtime][unit][playback][concurrency]")
  {
    auto fixture = PlaybackServiceFixture<QueuedExecutor>{};
    fixture.buildThreeTrackManualView();
    fixture.application.executor.drain();
    auto snapshots = std::vector<PlaybackSnapshot>{};
    bool queuedCommands = false;
    bool startAdmitted = false;
    auto const subscription = fixture.playback().events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot)
      {
        snapshots.push_back(snapshot);

        if (!queuedCommands && snapshot.succession.shuffle == ShuffleMode::On)
        {
          queuedCommands = true;
          startAdmitted = fixture.commands().startFromView(fixture.viewId, fixture.firstTrackId).has_value();
          fixture.commands().stop();
        }
      });

    fixture.commands().setShuffleMode(ShuffleMode::On);
    REQUIRE(startAdmitted);

    fixture.application.executor.drain();

    REQUIRE_FALSE(snapshots.empty());

    for (auto const& snapshot : snapshots)
    {
      CHECK(snapshot.transport.nowPlaying.trackId != fixture.firstTrackId);
      CHECK(snapshot.succession.currentTrackId != fixture.firstTrackId);
    }

    CHECK(fixture.playback().snapshot().transport.nowPlaying.trackId == kInvalidTrackId);
    CHECK(fixture.playback().snapshot().succession.currentTrackId == kInvalidTrackId);
  }

  TEST_CASE("PlaybackService - destruction drops a pending observer command", "[runtime][unit][playback][concurrency]")
  {
    auto fixture = ApplicationPlaybackFixtureT<QueuedExecutor>{};
    std::size_t repeatChanges = 0;
    auto const repeatSubscription = fixture.succession.onRepeatModeChanged(
      [&repeatChanges](PlaybackSuccession::RepeatModeChanged const&) { ++repeatChanges; });
    auto snapshotSubscription = fixture.playback.events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot)
      {
        if (snapshot.succession.shuffle == ShuffleMode::On)
        {
          fixture.commands().setRepeatMode(RepeatMode::All);
        }
      });

    fixture.commands().setShuffleMode(ShuffleMode::On);
    REQUIRE(fixture.executor.queuedCount() != 0);

    snapshotSubscription.reset();
    fixture.playbackPtr.reset();
    fixture.executor.drain();

    CHECK(repeatChanges == 0);
  }
} // namespace ao::rt::test
