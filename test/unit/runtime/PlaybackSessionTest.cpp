// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/PlaybackSessionState.h"
#include "runtime/PlaybackSessionYamlSchema.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Format.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kPlaybackSessionV3SortFieldOrdinals = std::to_array<std::pair<TrackSortField, std::int32_t>>({
      {TrackSortField::Artist, 0},
      {TrackSortField::Album, 1},
      {TrackSortField::AlbumArtist, 2},
      {TrackSortField::Genre, 3},
      {TrackSortField::Composer, 4},
      {TrackSortField::Conductor, 5},
      {TrackSortField::Ensemble, 6},
      {TrackSortField::Work, 7},
      {TrackSortField::Movement, 8},
      {TrackSortField::Soloist, 9},
      {TrackSortField::Year, 10},
      {TrackSortField::DiscNumber, 11},
      {TrackSortField::TrackNumber, 12},
      {TrackSortField::Title, 13},
      {TrackSortField::Duration, 14},
    });

    TrackId addPlayableTrack(AppRuntime& runtime,
                             std::string title,
                             std::uint16_t const year = 2020,
                             std::move_only_function<void()> settlePublication = {})
    {
      auto const uri = audio::test::installAudioFixture(
        runtime.musicLibrary().rootPath(), "basic_metadata.flac", "session-playable.flac");
      return addRuntimeTrack(runtime,
                             library::test::TrackSpec{
                               .title = std::move(title),
                               .uri = uri,
                               .year = year,
                               .duration = std::chrono::seconds{10},
                             },
                             std::move(settlePublication));
    }

    ViewId createView(AppRuntime& runtime, std::string filterExpression = {}, std::vector<TrackSortTerm> sortBy = {})
    {
      runtime.reloadAllTracks();
      auto const created = runtime.views().createView(TrackListViewConfig{
        .listId = kAllTracksListId,
        .filterExpression = std::move(filterExpression),
        .sortBy = std::move(sortBy),
      });
      REQUIRE(created);
      return created->viewId;
    }

    PlaybackSessionState storedSession(ConfigStore& store)
    {
      auto session = PlaybackSessionState{};
      auto const loaded = store.load(kPlaybackSessionConfigGroup, session, PlaybackSessionYamlSchema{});
      REQUIRE(loaded);
      REQUIRE(*loaded);
      return session;
    }

    void storeSession(AppRuntime& runtime, PlaybackSessionState const& session)
    {
      REQUIRE(
        runtime.playbackSessionConfigStore().save(kPlaybackSessionConfigGroup, session, PlaybackSessionYamlSchema{}));
    }

    struct ManualView final
    {
      ListId listId = kInvalidListId;
      ViewId viewId = kInvalidViewId;
    };

    ManualView createManualView(AppRuntime& runtime,
                                std::vector<TrackId> trackIds,
                                std::vector<TrackSortTerm> sortBy = {})
    {
      runtime.reloadAllTracks();
      auto const listId = ao::test::requireValue(runtime.library().writer().createList(LibraryWriter::ListDraft{
        .kind = LibraryWriter::ListKind::Manual,
        .name = "Playback session order",
        .trackIds = std::move(trackIds),
      }));
      auto const created = runtime.views().createView(TrackListViewConfig{
        .listId = listId,
        .sortBy = std::move(sortBy),
      });
      REQUIRE(created);
      return ManualView{.listId = listId, .viewId = created->viewId};
    }

    std::string rawPlaybackSessionYaml(TrackId const trackId,
                                       std::string_view const schemaLine,
                                       std::string_view const sortBy)
    {
      return std::format("playback-session:\n"
                         "{}"
                         "  sourceListId: {}\n"
                         "  quickFilterExpression: ''\n"
                         "  sortBy: {}\n"
                         "  currentTrackId: {}\n"
                         "  anchorIndex: 0\n"
                         "  positionMs: 0\n"
                         "  shuffleMode: 0\n"
                         "  repeatMode: 0\n"
                         "  volume: 1\n"
                         "  muted: false\n",
                         schemaLine,
                         kAllTracksListId.raw(),
                         sortBy,
                         trackId.raw());
    }

    void writeWorkspaceYaml(ao::test::TempDir const& tempDir, std::string_view const yaml)
    {
      auto output = std::ofstream{tempDir.path() / "workspace.yaml", std::ios::binary};
      REQUIRE(output);
      output << yaml;
      REQUIRE(output.good());
    }

    // A ready audio provider whose backend can be armed to reject setProperty for
    // one property. Session restore applies the restored volume then mute to the
    // active backend; arming a rejection exercises the atomic rollback path
    // through the public restore workflow. The arm outlives the backends that
    // borrow it, so declare it before the runtime it feeds.
    class PropertyFailArm final
    {
    public:
      void arm(audio::PropertyId const id) { _optFailing = id; }
      std::optional<audio::PropertyId> failing() const { return _optFailing; }

    private:
      std::optional<audio::PropertyId> _optFailing{};
    };

    class ArmedFailBackend final : public audio::NullBackend
    {
    public:
      ArmedFailBackend(audio::BackendId backendId, audio::ProfileId profileId, PropertyFailArm const& arm)
        : _backendId{std::move(backendId)}, _profileId{std::move(profileId)}, _arm{&arm}
      {
      }

      audio::BackendId backendId() const override { return _backendId; }
      audio::ProfileId profileId() const override { return _profileId; }

      Result<> setProperty(audio::PropertyId const id, audio::PropertyValue const& value) override
      {
        if (_arm->failing() == id)
        {
          return makeError(Error::Code::IoError, "property rejected");
        }

        return NullBackend::setProperty(id, value);
      }

    private:
      audio::BackendId _backendId;
      audio::ProfileId _profileId;
      PropertyFailArm const* _arm;
    };

    class PropertyFailProvider final : public audio::BackendProvider
    {
    public:
      explicit PropertyFailProvider(PropertyFailArm const& arm)
        : _arm{&arm}
      {
      }

      void shutdown() noexcept override {}

      audio::Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        if (callback)
        {
          callback(_status.devices);
        }

        return audio::Subscription{};
      }

      Status status() const override { return _status; }

      std::unique_ptr<audio::Backend> createBackend(audio::Device const& device,
                                                    audio::ProfileId const& profile) override
      {
        return std::make_unique<ArmedFailBackend>(device.backendId, profile, *_arm);
      }

      audio::Subscription subscribeGraph(std::string_view /*routeAnchor*/, OnGraphChangedCallback /*callback*/) override
      {
        return audio::Subscription{};
      }

    private:
      PropertyFailArm const* _arm;
      Status _status = makeReadyAudioStatus();
    };

    class VolumeCapabilityBackend final : public audio::NullBackend
    {
    public:
      VolumeCapabilityBackend(audio::BackendId backendId, audio::ProfileId profileId, bool const hardwareAssisted)
        : _backendId{std::move(backendId)}, _profileId{std::move(profileId)}, _hardwareAssisted{hardwareAssisted}
      {
      }

      audio::BackendId backendId() const override { return _backendId; }
      audio::ProfileId profileId() const override { return _profileId; }

      audio::PropertyInfo queryProperty(audio::PropertyId const id) const noexcept override
      {
        auto info = NullBackend::queryProperty(id);

        if (id == audio::PropertyId::Volume)
        {
          info.isHardwareAssisted = _hardwareAssisted;
        }

        return info;
      }

    private:
      audio::BackendId _backendId;
      audio::ProfileId _profileId;
      bool _hardwareAssisted = false;
    };

    class VolumeCapabilityProvider final : public audio::BackendProvider
    {
    public:
      VolumeCapabilityProvider()
        : _status{makeReadyAudioStatus()}
      {
        _status.devices.push_back(audio::Device{
          .id = audio::DeviceId{"hardware_volume_device"},
          .displayName = "Hardware volume device",
          .description = "Test output with hardware-assisted volume",
          .backendId = _status.descriptor.id,
        });
      }

      void shutdown() noexcept override {}

      audio::Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        if (callback)
        {
          callback(_status.devices);
        }

        return {};
      }

      Status status() const override { return _status; }

      std::unique_ptr<audio::Backend> createBackend(audio::Device const& device,
                                                    audio::ProfileId const& profile) override
      {
        return std::make_unique<VolumeCapabilityBackend>(
          device.backendId, profile, device.id == audio::DeviceId{"hardware_volume_device"});
      }

      audio::Subscription subscribeGraph(std::string_view /*routeAnchor*/, OnGraphChangedCallback /*callback*/) override
      {
        return {};
      }

    private:
      Status _status;
    };

    struct RenderCaptureState final
    {
      audio::RenderTarget* renderTarget = nullptr;
    };

    class RenderCaptureBackend final : public audio::NullBackend
    {
    public:
      explicit RenderCaptureBackend(std::shared_ptr<RenderCaptureState> statePtr)
        : _statePtr{std::move(statePtr)}
      {
      }

      Result<> open(audio::Format const& /*format*/, audio::RenderTarget* renderTarget) override
      {
        _statePtr->renderTarget = renderTarget;
        return {};
      }

      audio::BackendId backendId() const override { return audio::BackendId{"test_backend"}; }
      audio::ProfileId profileId() const override { return audio::kProfileShared; }

    private:
      std::shared_ptr<RenderCaptureState> _statePtr;
    };

    class RenderCaptureProvider final : public audio::BackendProvider
    {
    public:
      explicit RenderCaptureProvider(std::shared_ptr<RenderCaptureState> statePtr)
        : _statePtr{std::move(statePtr)}
      {
      }

      void shutdown() noexcept override {}

      audio::Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        if (callback)
        {
          callback(_status.devices);
        }

        return {};
      }

      Status status() const override { return _status; }

      std::unique_ptr<audio::Backend> createBackend(audio::Device const& /*device*/,
                                                    audio::ProfileId const& /*profile*/) override
      {
        return std::make_unique<RenderCaptureBackend>(_statePtr);
      }

      audio::Subscription subscribeGraph(std::string_view /*routeAnchor*/, OnGraphChangedCallback /*callback*/) override
      {
        return {};
      }

    private:
      std::shared_ptr<RenderCaptureState> _statePtr;
      Status _status = makeReadyAudioStatus();
    };
  } // namespace

  TEST_CASE("PlaybackSession - schema v3 freezes numeric sort-field ordinals", "[runtime][unit][playback-session]")
  {
    static_assert(kPlaybackSessionV3SortFieldOrdinals.size() == kTrackSortFieldCount);
    CHECK(kPlaybackSessionSchemaVersion == 3);

    for (auto const& [field, ordinal] : kPlaybackSessionV3SortFieldOrdinals)
    {
      CHECK(static_cast<std::int32_t>(field) == ordinal);
    }
  }

  TEST_CASE("PlaybackSessionYamlSchema - owns the exact YAML mapping", "[runtime][unit][playback-session][schema]")
  {
    auto const state = PlaybackSessionState{
      .sourceListId = kAllTracksListId,
      .quickFilterExpression = "$year > 2000",
      .sortBy = {{.field = TrackSortField::Title, .ascending = false}},
      .currentTrackId = TrackId{42},
      .anchorIndex = 3,
      .positionMs = 900,
      .shuffleMode = ShuffleMode::On,
      .repeatMode = RepeatMode::All,
      .volume = 0.75F,
      .muted = true,
    };
    auto tree = ryml::Tree{yaml::callbacks()};

    REQUIRE(PlaybackSessionYamlSchema{}.serialize(tree.rootref(), state));
    CHECK(yaml::scalarView(tree.rootref()["schemaVersion"]) == "3");
    CHECK(yaml::scalarView(tree.rootref()["sortBy"][0]["field"]) == "13");

    auto const decoded = PlaybackSessionYamlSchema{}.deserialize(tree.rootref(), PlaybackSessionState{});
    REQUIRE(decoded);
    CHECK(*decoded == state);
  }

  TEST_CASE("PlaybackSessionYamlSchema - rejects future and unknown YAML structure",
            "[runtime][unit][playback-session][schema]")
  {
    SECTION("Future version is reported before interpreting its payload")
    {
      auto const* source = "schemaVersion: 99\nsortBy: malformed\nfuture: true\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decoded = PlaybackSessionYamlSchema{}.deserialize(tree.rootref(), PlaybackSessionState{});

      REQUIRE_FALSE(decoded);
      CHECK(decoded.error().code == Error::Code::NotSupported);
    }

    SECTION("Unknown structural keys are rejected")
    {
      auto const* source = R"(
        schemaVersion: 3
        sourceListId: 1
        quickFilterExpression: ""
        sortBy: []
        currentTrackId: 42
        anchorIndex: 0
        positionMs: 0
        shuffleMode: 0
        repeatMode: 0
        volume: 1
        muted: false
        future: true
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decoded = PlaybackSessionYamlSchema{}.deserialize(tree.rootref(), PlaybackSessionState{});

      REQUIRE_FALSE(decoded);
      CHECK(decoded.error().code == Error::Code::FormatRejected);
      CHECK(decoded.error().message.contains("future"));
    }
  }

  TEST_CASE("PlaybackSession - cursor payload round-trips without autoplay", "[runtime][unit][playback-session]")
  {
    auto tempDir = ao::test::TempDir{};
    auto playbackSessionStore = ConfigStore{tempDir.path() / "application.yaml"};
    auto runtime = makeRuntime(tempDir, &playbackSessionStore);
    addReadyAudioProvider(runtime);
    auto const alpha = addPlayableTrack(runtime, "Alpha", 2022);
    std::ignore = addPlayableTrack(runtime, "Filtered", 1990);
    std::ignore = addPlayableTrack(runtime, "Zulu", 2023);
    auto const sortBy = std::vector{TrackSortTerm{.field = TrackSortField::Title, .ascending = false}};
    auto const viewId = createView(runtime, "$year > 2000", sortBy);

    REQUIRE(runtime.playback().commands().startFromView(viewId, alpha));
    runtime.playback().commands().seek(std::chrono::milliseconds{500});
    runtime.playback().commands().setShuffleMode(ShuffleMode::On);
    runtime.playback().commands().setRepeatMode(RepeatMode::All);
    runtime.playback().commands().setVolume(0.5F);
    runtime.playback().commands().setMuted(true);
    REQUIRE(runtime.savePlaybackSession());

    CHECK_FALSE(*runtime.workspaceConfigStore().contains(kPlaybackSessionConfigGroup));
    CHECK(*runtime.playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));

    auto const saved = storedSession(runtime.playbackSessionConfigStore());
    CHECK(saved.schemaVersion == 3);
    CHECK(saved.sourceListId == kAllTracksListId);
    CHECK(saved.quickFilterExpression == "$year > 2000");
    CHECK(saved.sortBy == sortBy);
    CHECK(saved.currentTrackId == alpha);
    CHECK(saved.anchorIndex == 1);
    CHECK(saved.positionMs == 500);
    CHECK(saved.shuffleMode == ShuffleMode::On);
    CHECK(saved.repeatMode == RepeatMode::All);
    CHECK(saved.volume == 0.5F);
    CHECK(saved.muted);

    runtime.playback().commands().stop();
    auto const restored = runtime.restorePlaybackSession();

    REQUIRE(restored);
    REQUIRE(restored->restored);
    CHECK(restored->trackId == alpha);
    CHECK(runtime.playback().snapshot().transport.transport == audio::Transport::Idle);
    CHECK(runtime.playback().snapshot().transport.nowPlaying.trackId == alpha);
    CHECK(runtime.playback().snapshot().transport.elapsed == std::chrono::milliseconds{500});
    CHECK(runtime.playback().snapshot().succession.shuffle == ShuffleMode::On);
    CHECK(runtime.playback().snapshot().succession.repeat == RepeatMode::All);
  }

  TEST_CASE("PlaybackSession - explicit checkpoint starts event-driven debounce",
            "[runtime][unit][playback-session][timing]")
  {
    auto tempDir = ao::test::TempDir{};
    auto playbackSessionStore = ConfigStore{tempDir.path() / "application.yaml"};
    auto sleeper = ControlledSleeper{};
    auto executorPtr = std::make_unique<ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtime = makeRuntime(tempDir, std::move(executorPtr), &playbackSessionStore, &sleeper);

    addReadyAudioProvider(runtime);
    executor->runUntilIdle();
    auto const trackId = addPlayableTrack(runtime, "Debounced Track", 2020, [executor] { executor->runUntilIdle(); });
    auto const viewId = createView(runtime);
    REQUIRE(runtime.playback().commands().startFromView(viewId, trackId));
    REQUIRE(runtime.savePlaybackSession());
    executor->runUntilIdle();

    runtime.playback().commands().setVolume(0.4F);
    REQUIRE(sleeper.fireNext(std::chrono::seconds{1}));
    executor->checkQueued();
    REQUIRE(executor->runOne());

    CHECK(storedSession(runtime.playbackSessionConfigStore()).volume == 0.4F);

    runtime.playback().commands().setVolume(0.6F);
    REQUIRE(sleeper.fireNext(std::chrono::seconds{1}));
    executor->checkQueued();
    REQUIRE(executor->runOne());
    CHECK(storedSession(runtime.playbackSessionConfigStore()).volume == 0.6F);
  }

  TEST_CASE("PlaybackSession - shuffle debounce samples the latest elapsed position",
            "[runtime][regression][playback-session][timing]")
  {
    auto tempDir = ao::test::TempDir{};
    auto playbackSessionStore = ConfigStore{tempDir.path() / "application.yaml"};
    auto sleeper = ControlledSleeper{};
    auto executorPtr = std::make_unique<ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtime = makeRuntime(tempDir, std::move(executorPtr), &playbackSessionStore, &sleeper);
    REQUIRE(runtime.restorePlaybackSession());

    auto captureStatePtr = std::make_shared<RenderCaptureState>();
    runtime.addAudioProvider(std::make_unique<RenderCaptureProvider>(captureStatePtr));
    executor->runUntilIdle();
    auto const trackId = addPlayableTrack(runtime, "Latent elapsed", 2020, [executor] { executor->runUntilIdle(); });
    auto const viewId = createView(runtime);
    REQUIRE(runtime.playback().commands().startFromView(viewId, trackId));
    executor->runUntilIdle();
    REQUIRE(captureStatePtr->renderTarget != nullptr);

    auto output = std::array<std::byte, 4096>{};
    auto const renderResult = captureStatePtr->renderTarget->renderPcm(output);
    REQUIRE(renderResult.bytesWritten > 0);
    captureStatePtr->renderTarget->handlePositionAdvanced(renderResult.positionFrames);
    REQUIRE(runtime.savePlaybackSession());

    auto sentinel = storedSession(playbackSessionStore);
    sentinel.positionMs = 0;
    sentinel.shuffleMode = ShuffleMode::Off;
    storeSession(runtime, sentinel);

    runtime.playback().commands().setShuffleMode(ShuffleMode::On);

    auto const beforeDebounce = storedSession(playbackSessionStore);
    CHECK(beforeDebounce.positionMs == 0);
    CHECK(beforeDebounce.shuffleMode == ShuffleMode::Off);

    REQUIRE(sleeper.fireNext(std::chrono::seconds{1}));
    executor->checkQueued();
    REQUIRE(executor->runOne());
    auto const afterDebounce = storedSession(playbackSessionStore);
    CHECK(afterDebounce.positionMs > 0);
    CHECK(afterDebounce.shuffleMode == ShuffleMode::On);
  }

  TEST_CASE("PlaybackSession - next persistence intent saves after a failed debounce",
            "[runtime][unit][playback-session][timing]")
  {
    auto tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "application.yaml";
    auto playbackSessionStore = ConfigStore{configPath};
    auto sleeper = ControlledSleeper{};
    auto executorPtr = std::make_unique<ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtime = makeRuntime(tempDir, std::move(executorPtr), &playbackSessionStore, &sleeper);
    auto const restored = runtime.restorePlaybackSession();
    REQUIRE(restored);
    CHECK_FALSE(restored->restored);

    addReadyAudioProvider(runtime);
    executor->runUntilIdle();
    auto const trackId = addPlayableTrack(runtime, "Deferred Save", 2020, [executor] { executor->runUntilIdle(); });
    auto const viewId = createView(runtime);
    REQUIRE(runtime.playback().commands().startFromView(viewId, trackId));
    REQUIRE(runtime.savePlaybackSession());
    executor->runUntilIdle();
    REQUIRE(std::filesystem::remove(configPath));
    REQUIRE(std::filesystem::create_directory(configPath));
    runtime.playback().commands().setVolume(0.4F);

    REQUIRE(sleeper.fireNext(std::chrono::seconds{1}));
    executor->checkQueued();
    REQUIRE(executor->runOne());

    REQUIRE(std::filesystem::remove(configPath));
    runtime.playback().commands().setVolume(0.6F);
    REQUIRE(sleeper.fireNext(std::chrono::seconds{1}));
    executor->checkQueued();
    REQUIRE(executor->runOne());
    CHECK(storedSession(runtime.playbackSessionConfigStore()).volume == 0.6F);
  }

  TEST_CASE("PlaybackSession - launch publishes one coherent final live intent",
            "[runtime][regression][playback-session][launch]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime);
    auto const insertedBeforeCurrent = addPlayableTrack(runtime, "Inserted before current");
    auto const current = addPlayableTrack(runtime, "Current");
    auto const removedSuccessor = addPlayableTrack(runtime, "Removed successor");
    auto const finalSuccessor = addPlayableTrack(runtime, "Final successor");
    auto const manual = createManualView(runtime, {current, removedSuccessor, finalSuccessor});
    auto const insertedIds = std::vector{insertedBeforeCurrent};
    auto const removedIds = std::vector{removedSuccessor};
    auto changedStates = std::vector<PlaybackSnapshot>{};
    auto const changedSubscription = runtime.playback().events().onSnapshot([&](PlaybackSnapshot const& snapshot)
                                                                            { changedStates.push_back(snapshot); });

    auto const inserted = runtime.library().writer().insertManualListTracks(manual.listId, 0, insertedIds);
    REQUIRE(inserted);
    REQUIRE(inserted->changed);
    auto const removed = runtime.library().writer().removeManualListTracks(manual.listId, removedIds);
    REQUIRE(removed);
    REQUIRE(removed->changed);

    auto const launched = runtime.playback().commands().startFromView(manual.viewId, current);

    REQUIRE(launched);
    auto const accepted = runtime.playback().snapshot().succession;
    CHECK(accepted.sourceState == PlaybackSourceState::Live);
    CHECK(accepted.currentTrackId == current);
    CHECK(accepted.shuffle == ShuffleMode::Off);
    CHECK(accepted.repeat == RepeatMode::Off);
    CHECK(accepted.hasPrevious);
    CHECK(accepted.hasNext);
    REQUIRE(changedStates.size() == 1);
    CHECK(changedStates.front() == runtime.playback().snapshot());
    CHECK(runtime.playback().snapshot().transport.nowPlaying.trackId == current);
    CHECK(runtime.playback().snapshot().transport.transport == audio::Transport::Playing);

    runtime.playback().commands().next();

    CHECK(runtime.playback().snapshot().succession.currentTrackId == finalSuccessor);
    CHECK(runtime.playback().snapshot().transport.nowPlaying.trackId == finalSuccessor);
    CHECK(runtime.playback().snapshot().transport.transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSession - restore contains observer exceptions and defers nested playback commands",
            "[runtime][regression][playback-session][concurrency]")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtime = makeRuntime(tempDir, std::move(executorPtr));
    addReadyAudioProvider(runtime);
    executor->drain();
    auto const firstTrackId = addPlayableTrack(runtime, "First", 2020, [executor] { executor->drain(); });
    auto const secondTrackId = addPlayableTrack(runtime, "Second", 2020, [executor] { executor->drain(); });
    auto const viewId = createView(runtime, {}, {{.field = TrackSortField::Title, .ascending = true}});
    executor->drain();
    REQUIRE(runtime.playback().commands().startFromView(viewId, firstTrackId));
    executor->drain();
    REQUIRE(runtime.savePlaybackSession());
    runtime.playback().commands().stop();
    executor->drain();
    auto normalizedPayload = storedSession(runtime.playbackSessionConfigStore());
    normalizedPayload.anchorIndex = 999;
    normalizedPayload.positionMs = 400;
    storeSession(runtime, normalizedPayload);

    std::uint32_t snapshotCount = 0;
    bool nestedIntentRequested = false;
    bool nestedRestoreAccepted = false;
    auto nestedRestoreError = Error::Code::Generic;
    bool nestedLaunchAccepted = false;
    auto snapshotSubscription = runtime.playback().events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot)
      {
        ++snapshotCount;
        CHECK(snapshot.succession.currentTrackId == firstTrackId);
        CHECK(snapshot.transport.nowPlaying.trackId == firstTrackId);
        CHECK(snapshot.transport.elapsed == std::chrono::milliseconds{400});

        if (!nestedIntentRequested)
        {
          nestedIntentRequested = true;
          auto const nestedRestore = runtime.restorePlaybackSession();
          nestedRestoreAccepted = nestedRestore.has_value();

          if (!nestedRestore)
          {
            nestedRestoreError = nestedRestore.error().code;
          }

          auto const nestedLaunch = runtime.playback().commands().startFromView(viewId, secondTrackId);
          nestedLaunchAccepted = nestedLaunch.has_value();
        }

        throwException<Exception>("scripted restored snapshot observer failure");
      });

    auto const restored = runtime.restorePlaybackSession();

    REQUIRE(restored);
    REQUIRE(restored->restored);
    CHECK(snapshotCount == 1);
    CHECK(nestedIntentRequested);
    CHECK_FALSE(nestedRestoreAccepted);
    CHECK(nestedRestoreError == Error::Code::InvalidState);
    CHECK(nestedLaunchAccepted);
    CHECK(runtime.playback().snapshot().succession.currentTrackId == firstTrackId);
    CHECK(runtime.playback().snapshot().transport.nowPlaying.trackId == firstTrackId);
    CHECK(runtime.playback().snapshot().succession.hasNext);
    CHECK_FALSE(runtime.playback().snapshot().succession.hasPrevious);
    CHECK(runtime.playback().snapshot().transport.elapsed == std::chrono::milliseconds{400});
    CHECK(runtime.playback().snapshot().transport.transport == audio::Transport::Idle);

    snapshotSubscription.reset();
    REQUIRE(executor->drainUntil(
      [&]
      {
        return runtime.playback().snapshot().succession.currentTrackId == secondTrackId &&
               runtime.playback().snapshot().transport.nowPlaying.trackId == secondTrackId &&
               runtime.playback().snapshot().transport.transport == audio::Transport::Playing;
      }));
  }

  TEST_CASE("PlaybackSession - restore does not overtake a pending observer intent",
            "[runtime][regression][playback-session][concurrency]")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtime = makeRuntime(tempDir, std::move(executorPtr));
    addReadyAudioProvider(runtime);
    executor->drain();
    auto const trackId = addPlayableTrack(runtime, "Pending restore", 2020, [executor] { executor->drain(); });
    auto const viewId = createView(runtime);
    executor->drain();
    REQUIRE(runtime.playback().commands().startFromView(viewId, trackId));
    executor->drain();
    REQUIRE(runtime.savePlaybackSession());

    bool queuedRepeat = false;
    auto const snapshotSubscription = runtime.playback().events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot)
      {
        if (!queuedRepeat && snapshot.succession.shuffle == ShuffleMode::On)
        {
          queuedRepeat = true;
          runtime.playback().commands().setRepeatMode(RepeatMode::All);
        }
      });

    runtime.playback().commands().setShuffleMode(ShuffleMode::On);
    REQUIRE(queuedRepeat);

    auto const restored = runtime.restorePlaybackSession();

    REQUIRE_FALSE(restored);
    CHECK(restored.error().code == Error::Code::InvalidState);
    executor->drain();
    CHECK(runtime.playback().snapshot().succession.repeat == RepeatMode::All);
  }

  TEST_CASE("PlaybackSession - same-subject restore publishes a changed offset",
            "[runtime][regression][playback-session][restore]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime);
    auto const trackId = addPlayableTrack(runtime, "Restored offset");
    runtime.reloadAllTracks();
    auto session = PlaybackSessionState{
      .sourceListId = kAllTracksListId,
      .currentTrackId = trackId,
      .positionMs = 250,
    };
    storeSession(runtime, session);
    REQUIRE(runtime.restorePlaybackSession());
    auto const before = runtime.playback().snapshot();
    REQUIRE(before.transport.elapsed == std::chrono::milliseconds{250});
    session.positionMs = 750;
    storeSession(runtime, session);

    auto const restored = runtime.restorePlaybackSession();

    REQUIRE(restored);
    REQUIRE(restored->restored);
    auto const after = runtime.playback().snapshot();
    CHECK(after.revision.value == before.revision.value + 1);
    CHECK(after.transport.positionRevision.value == before.transport.positionRevision.value + 1);
    CHECK(after.transport.finalSeekRevision == before.transport.finalSeekRevision);
    CHECK(after.transport.elapsed == std::chrono::milliseconds{750});
  }

  TEST_CASE("PlaybackSession - validates the complete serialized payload before lookup",
            "[runtime][unit][playback-session][error]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    auto const trackId = addPlayableTrack(runtime, "Current");
    runtime.reloadAllTracks();
    auto payload = PlaybackSessionState{
      .sourceListId = kAllTracksListId,
      .currentTrackId = trackId,
    };
    auto expectedError = Error::Code::CorruptData;

    SECTION("schema v2 is rejected")
    {
      payload.schemaVersion = 2;
      expectedError = Error::Code::NotSupported;
    }

    SECTION("invalid identities are rejected")
    {
      payload.sourceListId = kInvalidListId;
    }

    SECTION("anchor overflow is rejected")
    {
      payload.anchorIndex = std::numeric_limits<std::uint64_t>::max();
    }

    SECTION("position overflow is rejected")
    {
      payload.positionMs = std::numeric_limits<std::uint64_t>::max();
    }

    SECTION("duplicate sort fields are rejected")
    {
      payload.sortBy = {{.field = TrackSortField::Title}, {.field = TrackSortField::Title}};
    }

    SECTION("invalid sort fields are rejected")
    {
      payload.sortBy = {{.field = static_cast<TrackSortField>(255)}};
    }

    SECTION("excess sort fields are rejected")
    {
      payload.sortBy.assign(kPlaybackSessionMaxSortTerms + 1, TrackSortTerm{});
    }

    SECTION("malformed filter is rejected before missing-source fallback")
    {
      payload.sourceListId = ListId{999'999};
      payload.quickFilterExpression = "$year >";
      expectedError = Error::Code::FormatRejected;
    }

    SECTION("invalid shuffle mode is rejected")
    {
      payload.shuffleMode = static_cast<ShuffleMode>(99);
    }

    SECTION("invalid repeat mode is rejected")
    {
      payload.repeatMode = static_cast<RepeatMode>(99);
    }

    SECTION("non-finite volume is rejected")
    {
      payload.volume = std::numeric_limits<float>::infinity();
      expectedError = Error::Code::FormatRejected;
    }

    SECTION("out-of-range volume is rejected")
    {
      payload.volume = 5.0F;
    }

    storeSession(runtime, payload);
    auto const restored = runtime.restorePlaybackSession();
    REQUIRE_FALSE(restored);
    CHECK(restored.error().code == expectedError);
  }

  TEST_CASE("PlaybackSession - exact schema rejects missing and malformed raw YAML fields",
            "[runtime][regression][playback-session][schema]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    auto const trackId = addPlayableTrack(runtime, "Current");
    runtime.reloadAllTracks();
    auto schemaLine = std::string_view{"  schemaVersion: 3\n"};
    auto sortBy = std::string_view{"[]"};

    SECTION("missing schemaVersion")
    {
      schemaLine = {};
    }

    SECTION("scalar sort element")
    {
      sortBy = "[broken]";
    }

    SECTION("empty map sort element")
    {
      sortBy = "[{}]";
    }

    SECTION("valid and malformed sort elements mixed")
    {
      sortBy = "\n    - field: 13\n      ascending: true\n    - broken";
    }

    writeWorkspaceYaml(tempDir, rawPlaybackSessionYaml(trackId, schemaLine, sortBy));
    auto const restored = runtime.restorePlaybackSession();

    REQUIRE_FALSE(restored);
    CHECK(restored.error().code == Error::Code::FormatRejected);
    CHECK(runtime.playback().snapshot().succession.sourceState == PlaybackSourceState::Inactive);
    CHECK(runtime.playback().snapshot().transport.nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("PlaybackSession - restore resolves bound, gap, and replacement rows",
            "[runtime][unit][playback-session][restore-matrix]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime);
    auto const first = addPlayableTrack(runtime, "First", 1990);
    auto const second = addPlayableTrack(runtime, "Second", 2022);
    std::ignore = addPlayableTrack(runtime, "Third", 2023);
    runtime.reloadAllTracks();

    auto payload = PlaybackSessionState{
      .sourceListId = kAllTracksListId,
      .currentTrackId = first,
      .positionMs = 400,
    };

    SECTION("projected current is bound and retains position")
    {
      payload.anchorIndex = 0;
      storeSession(runtime, payload);
      auto const restored = runtime.restorePlaybackSession();
      REQUIRE(restored);
      REQUIRE(restored->restored);
      CHECK(restored->trackId == first);
      CHECK(runtime.playback().snapshot().transport.elapsed == std::chrono::milliseconds{400});
    }

    SECTION("existing filtered current remains a gap and retains position")
    {
      payload.quickFilterExpression = "$year > 2000";
      payload.anchorIndex = 1;
      storeSession(runtime, payload);
      auto const restored = runtime.restorePlaybackSession();
      REQUIRE(restored);
      REQUIRE(restored->restored);
      CHECK(restored->trackId == first);
      CHECK(runtime.playback().snapshot().transport.elapsed == std::chrono::milliseconds{400});
      CHECK(runtime.playback().snapshot().succession.hasNext);
    }

    SECTION("missing current promotes the row at its saved anchor before shuffle")
    {
      payload.currentTrackId = TrackId{999'999};
      payload.anchorIndex = 1;
      payload.shuffleMode = ShuffleMode::On;
      storeSession(runtime, payload);
      auto const restored = runtime.restorePlaybackSession();
      REQUIRE(restored);
      REQUIRE(restored->restored);
      CHECK(restored->trackId == second);
      CHECK(runtime.playback().snapshot().transport.elapsed == std::chrono::milliseconds{0});
    }

    SECTION("missing current at end wraps only for repeat all")
    {
      payload.currentTrackId = TrackId{999'999};
      payload.anchorIndex = 3;
      payload.repeatMode = RepeatMode::All;
      storeSession(runtime, payload);
      auto const restored = runtime.restorePlaybackSession();
      REQUIRE(restored);
      REQUIRE(restored->restored);
      CHECK(restored->trackId == first);
      CHECK(runtime.playback().snapshot().transport.elapsed == std::chrono::milliseconds{0});
    }

    SECTION("missing current without deterministic successor is discarded")
    {
      payload.currentTrackId = TrackId{999'999};
      payload.anchorIndex = 3;
      storeSession(runtime, payload);
      auto const restored = runtime.restorePlaybackSession();
      REQUIRE(restored);
      CHECK_FALSE(restored->restored);
      CHECK(runtime.playback().snapshot().succession.sourceState == PlaybackSourceState::Inactive);
    }
  }

  TEST_CASE("PlaybackSession - missing-source fallback preserves order and clears filter",
            "[runtime][unit][playback-session][restore-matrix]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime);
    auto const current = addPlayableTrack(runtime, "Current", 1990);
    runtime.reloadAllTracks();
    auto const sortBy = std::vector{TrackSortTerm{.field = TrackSortField::Title, .ascending = false}};
    storeSession(runtime,
                 PlaybackSessionState{
                   .sourceListId = ListId{999'999},
                   .quickFilterExpression = "$year > 2000",
                   .sortBy = sortBy,
                   .currentTrackId = current,
                   .positionMs = 250,
                 });

    auto const restored = runtime.restorePlaybackSession();
    REQUIRE(restored);
    REQUIRE(restored->restored);
    CHECK(restored->sourceListId == kAllTracksListId);
    REQUIRE(runtime.savePlaybackSession());
    auto const corrected = storedSession(runtime.playbackSessionConfigStore());
    CHECK(corrected.sourceListId == kAllTracksListId);
    CHECK(corrected.quickFilterExpression.empty());
    CHECK(corrected.sortBy == sortBy);

    runtime.playback().commands().stop();
    storeSession(runtime,
                 PlaybackSessionState{
                   .sourceListId = ListId{999'999},
                   .currentTrackId = TrackId{888'888},
                 });
    auto const discarded = runtime.restorePlaybackSession();
    REQUIRE(discarded);
    CHECK_FALSE(discarded->restored);
  }

  TEST_CASE("PlaybackSession - duration clamping restores zero", "[runtime][unit][playback-session][restore-matrix]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime);
    auto const current = addPlayableTrack(runtime, "Current");
    runtime.reloadAllTracks();
    storeSession(runtime,
                 PlaybackSessionState{
                   .sourceListId = kAllTracksListId,
                   .currentTrackId = current,
                   .positionMs = 10'000,
                 });

    auto const restored = runtime.restorePlaybackSession();
    REQUIRE(restored);
    REQUIRE(restored->restored);
    CHECK(runtime.playback().snapshot().transport.elapsed == std::chrono::milliseconds{0});
    REQUIRE(runtime.savePlaybackSession());
    CHECK(storedSession(runtime.playbackSessionConfigStore()).positionMs == 0);
  }

  TEST_CASE("PlaybackSession - backend property failure rolls restored volume and mute back atomically",
            "[runtime][unit][playback-session][error]")
  {
    // The arm is declared before the runtime so the backends that borrow it stay
    // valid for the runtime's whole lifetime.
    auto arm = PropertyFailArm{};
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    runtime.addAudioProvider(std::make_unique<PropertyFailProvider>(arm));
    auto const current = addPlayableTrack(runtime, "Current");
    runtime.reloadAllTracks();

    // Establish a baseline live volume/mute while the backend still accepts writes.
    runtime.playback().commands().setVolume(0.25F);
    runtime.playback().commands().setMuted(false);
    auto const baseline = runtime.playback().snapshot().transport.volume;
    REQUIRE(baseline.level == 0.25F);
    REQUIRE(baseline.muted == false);
    auto const snapshotBefore = runtime.playback().snapshot();
    auto const sequenceBefore = snapshotBefore.succession;
    auto const playbackBefore = snapshotBefore.transport;

    auto const payload = PlaybackSessionState{
      .sourceListId = kAllTracksListId,
      .currentTrackId = current,
      .volume = 0.75F,
      .muted = true,
    };
    storeSession(runtime, payload);

    SECTION("volume rejection leaves the baseline untouched")
    {
      arm.arm(audio::PropertyId::Volume);
    }

    SECTION("mute rejection rolls back the staged volume")
    {
      arm.arm(audio::PropertyId::Muted);
    }

    auto const restored = runtime.restorePlaybackSession();

    REQUIRE_FALSE(restored);
    CHECK(restored.error().code == Error::Code::IoError);
    CHECK(runtime.playback().snapshot().succession == sequenceBefore);
    auto const snapshotAfter = runtime.playback().snapshot();
    auto const& playbackAfter = snapshotAfter.transport;
    CHECK(playbackAfter.transport == playbackBefore.transport);
    CHECK(playbackAfter.elapsed == playbackBefore.elapsed);
    CHECK(playbackAfter.duration == playbackBefore.duration);
    CHECK(playbackAfter.ready == playbackBefore.ready);
    CHECK(playbackAfter.nowPlaying == playbackBefore.nowPlaying);
    CHECK(playbackAfter.volume == playbackBefore.volume);
    CHECK(playbackAfter.output == playbackBefore.output);
    CHECK(playbackAfter.quality == playbackBefore.quality);
    CHECK(snapshotAfter.revision == snapshotBefore.revision);
    CHECK(storedSession(runtime.playbackSessionConfigStore()) == payload);
  }

  TEST_CASE("PlaybackSession - freezes invalidated and exhausted cursors as last-restorable intent",
            "[runtime][unit][playback-session][lifecycle]")
  {
    SECTION("source invalidation and stop retain the frozen cursor")
    {
      auto tempDir = ao::test::TempDir{};
      auto runtime = makeRuntime(tempDir);
      addReadyAudioProvider(runtime);
      auto const first = addPlayableTrack(runtime, "First");
      auto const second = addPlayableTrack(runtime, "Second");
      runtime.reloadAllTracks();
      auto const listId = ao::test::requireValue(runtime.library().writer().createList(LibraryWriter::ListDraft{
        .kind = LibraryWriter::ListKind::Manual,
        .name = "Temporary source",
        .trackIds = {first, second},
      }));
      auto const view = runtime.views().createView({.listId = listId});
      REQUIRE(view);
      REQUIRE(runtime.playback().commands().startFromView(view->viewId, first));
      REQUIRE(runtime.savePlaybackSession());

      auto const selected = runtime.playback().snapshot().transport.output.selectedDevice;
      runtime.playback().commands().setOutputDevice(selected.backendId, selected.deviceId, selected.profileId);
      REQUIRE(runtime.library().writer().deleteList(listId));
      CHECK(runtime.playback().snapshot().succession.sourceState == PlaybackSourceState::Invalidated);
      CHECK(runtime.playback().snapshot().transport.nowPlaying.trackId == first);
      runtime.playback().commands().pause();
      CHECK(runtime.playback().snapshot().transport.transport == audio::Transport::Paused);
      runtime.playback().commands().resume();
      REQUIRE(runtime.savePlaybackSession());
      CHECK(storedSession(runtime.playbackSessionConfigStore()).sourceListId == listId);

      runtime.playback().commands().stop();
      CHECK(runtime.playback().snapshot().succession.sourceState == PlaybackSourceState::Inactive);
      REQUIRE(runtime.savePlaybackSession());
      auto const frozen = storedSession(runtime.playbackSessionConfigStore());
      CHECK(frozen.sourceListId == listId);
      CHECK(frozen.currentTrackId == first);
    }

    SECTION("terminal exhaustion preserves the final current")
    {
      auto tempDir = ao::test::TempDir{};
      auto runtime = makeRuntime(tempDir);
      addReadyAudioProvider(runtime);
      auto const only = addPlayableTrack(runtime, "Only");
      auto const viewId = createView(runtime);
      REQUIRE(runtime.playback().commands().startFromView(viewId, only));
      runtime.playback().commands().seek(std::chrono::milliseconds{350});
      REQUIRE(runtime.savePlaybackSession());

      runtime.playback().commands().next();

      CHECK(runtime.playback().snapshot().succession.sourceState == PlaybackSourceState::Inactive);
      CHECK(runtime.playback().snapshot().transport.transport == audio::Transport::Idle);
      REQUIRE(runtime.savePlaybackSession());
      auto const frozen = storedSession(runtime.playbackSessionConfigStore());
      CHECK(frozen.currentTrackId == only);
      CHECK(frozen.positionMs == 350);
      auto const restored = runtime.restorePlaybackSession();
      REQUIRE(restored);
      REQUIRE(restored->restored);
      CHECK(restored->trackId == only);
      CHECK(runtime.playback().snapshot().transport.elapsed == std::chrono::milliseconds{350});
    }
  }

  TEST_CASE("PlaybackSession - paused seek and live anchor mutation each become saveable",
            "[runtime][unit][playback-session]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime);
    auto const first = addPlayableTrack(runtime, "First");
    auto const current = addPlayableTrack(runtime, "Second");
    std::ignore = addPlayableTrack(runtime, "Third");
    auto const viewId = createView(runtime);
    REQUIRE(runtime.playback().commands().startFromView(viewId, current));
    runtime.playback().commands().pause();
    REQUIRE(runtime.savePlaybackSession());

    runtime.playback().commands().seek(std::chrono::milliseconds{450});
    REQUIRE(runtime.savePlaybackSession());
    CHECK(storedSession(runtime.playbackSessionConfigStore()).positionMs == 450);

    REQUIRE(runtime.library().writer().deleteTrack(first));
    REQUIRE(runtime.savePlaybackSession());
    auto const moved = storedSession(runtime.playbackSessionConfigStore());
    CHECK(moved.currentTrackId == current);
    CHECK(moved.anchorIndex == 0);
    CHECK(runtime.playback().snapshot().transport.transport == audio::Transport::Paused);
  }

  TEST_CASE("PlaybackSession - sorted manual Gap ignores stored reorders with identical projected order",
            "[runtime][regression][playback-session][manual-list]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime);
    auto const current = addPlayableTrack(runtime, "Bravo");
    auto const alpha = addPlayableTrack(runtime, "Alpha");
    auto const charlie = addPlayableTrack(runtime, "Charlie");
    auto const titleSort = std::vector{TrackSortTerm{.field = TrackSortField::Title, .ascending = true}};
    auto const manual = createManualView(runtime, {current, alpha, charlie}, titleSort);
    REQUIRE(runtime.playback().commands().startFromView(manual.viewId, current));

    auto const currentIds = std::vector{current};
    auto const removed = runtime.library().writer().removeManualListTracks(manual.listId, currentIds);
    REQUIRE(removed);
    REQUIRE(removed->changed);
    runtime.playback().commands().pause();
    REQUIRE(runtime.savePlaybackSession());

    auto const beforeState = runtime.playback().snapshot().succession;
    REQUIRE(beforeState.hasNext);
    CHECK(beforeState.hasPrevious);
    auto const beforePayload = storedSession(runtime.playbackSessionConfigStore());
    CHECK(beforePayload.currentTrackId == current);
    CHECK(beforePayload.anchorIndex == 1);
    CHECK(beforePayload.sortBy == titleSort);
    auto const projectionPtr = runtime.views().trackListProjection(manual.viewId);
    REQUIRE(projectionPtr);
    REQUIRE(projectionPtr->size() == 2);
    CHECK(projectionPtr->trackIdAt(0) == alpha);
    CHECK(projectionPtr->trackIdAt(1) == charlie);
    auto const projectionRevision = projectionPtr->revision();
    std::uint32_t projectionBatchCount = 0;
    auto const projectionSubscription =
      projectionPtr->subscribe([&](TrackListProjectionDeltaBatch const&) { ++projectionBatchCount; });
    // The subscription synchronously publishes its initial snapshot; measure only the reorder below.
    projectionBatchCount = 0;

    auto const movedIds = std::vector{charlie};
    auto const moved = runtime.library().writer().moveManualListTracks(manual.listId, movedIds, 0);
    REQUIRE(moved);
    REQUIRE(moved->changed);

    CHECK(projectionPtr->revision() == projectionRevision);
    CHECK(projectionBatchCount == 0);
    CHECK(runtime.playback().snapshot().succession == beforeState);
    CHECK(runtime.playback().snapshot().transport.nowPlaying.trackId == current);
    CHECK(runtime.playback().snapshot().transport.transport == audio::Transport::Paused);
    REQUIRE(runtime.savePlaybackSession());
    CHECK(storedSession(runtime.playbackSessionConfigStore()) == beforePayload);
  }

  TEST_CASE("PlaybackSession - prepared replacement remains outside the public session payload",
            "[runtime][regression][playback-session][snapshot]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime);
    auto const first = addPlayableTrack(runtime, "First");
    auto const insertedTrack = addPlayableTrack(runtime, "Inserted successor");
    auto const originalSuccessor = addPlayableTrack(runtime, "Original successor");
    auto const manual = createManualView(runtime, {first, originalSuccessor});
    REQUIRE(runtime.playback().commands().startFromView(manual.viewId, first));
    runtime.playback().commands().pause();
    REQUIRE(runtime.savePlaybackSession());

    auto const beforeSnapshot = runtime.playback().snapshot();
    REQUIRE(beforeSnapshot.succession.hasNext);
    REQUIRE(beforeSnapshot.preparation.hasPreparedNext);
    auto const beforePayload = storedSession(runtime.playbackSessionConfigStore());

    auto const insertedIds = std::vector{insertedTrack};
    auto const inserted = runtime.library().writer().insertManualListTracks(manual.listId, 1, insertedIds);
    REQUIRE(inserted);
    REQUIRE(inserted->changed);

    auto const afterSnapshot = runtime.playback().snapshot();
    CHECK(afterSnapshot.succession.currentTrackId == first);
    CHECK(afterSnapshot.succession.hasNext);
    CHECK(afterSnapshot.preparation.hasPreparedNext);
    CHECK(afterSnapshot.sameContentAs(beforeSnapshot));
    CHECK(afterSnapshot.revision == beforeSnapshot.revision);
    CHECK(runtime.playback().snapshot().transport.transport == audio::Transport::Paused);
    REQUIRE(runtime.savePlaybackSession());
    CHECK(storedSession(runtime.playbackSessionConfigStore()) == beforePayload);
  }

  TEST_CASE("PlaybackSession - shuffle source mutation remains transient when public state is unchanged",
            "[runtime][regression][playback-session][shuffle]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime);
    auto const current = addPlayableTrack(runtime, "Current");
    auto const second = addPlayableTrack(runtime, "Second");
    auto const third = addPlayableTrack(runtime, "Third");
    auto const fourth = addPlayableTrack(runtime, "Fourth");
    auto const manual = createManualView(runtime, {current, second, third, fourth});
    REQUIRE(runtime.playback().commands().startFromView(manual.viewId, current));
    runtime.playback().commands().setShuffleMode(ShuffleMode::On);
    runtime.playback().commands().pause();
    REQUIRE(runtime.savePlaybackSession());

    auto const beforeSnapshot = runtime.playback().snapshot();
    REQUIRE(beforeSnapshot.succession.hasNext);
    auto const beforePayload = storedSession(runtime.playbackSessionConfigStore());

    auto const removedIds = std::vector{second};
    auto const removed = runtime.library().writer().removeManualListTracks(manual.listId, removedIds);
    REQUIRE(removed);
    REQUIRE(removed->changed);

    auto const afterSnapshot = runtime.playback().snapshot();
    CHECK(afterSnapshot.succession.currentTrackId == current);
    CHECK(afterSnapshot.succession.hasNext);
    CHECK(afterSnapshot.preparation.hasPreparedNext);
    CHECK(runtime.playback().snapshot().transport.transport == audio::Transport::Paused);
    REQUIRE(runtime.savePlaybackSession());
    CHECK(storedSession(runtime.playbackSessionConfigStore()) == beforePayload);
  }

  TEST_CASE("PlaybackSession - stale shuffle-history pop remains transient",
            "[runtime][regression][playback-session][shuffle]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime);
    auto const historyTrack = addPlayableTrack(runtime, "History track");
    auto const second = addPlayableTrack(runtime, "Second");
    auto const third = addPlayableTrack(runtime, "Third");
    auto const manual = createManualView(runtime, {historyTrack, second, third});
    REQUIRE(runtime.playback().commands().startFromView(manual.viewId, historyTrack));
    runtime.playback().commands().setShuffleMode(ShuffleMode::On);
    runtime.playback().commands().next();
    auto const current = runtime.playback().snapshot().succession.currentTrackId;
    REQUIRE(current != historyTrack);
    REQUIRE(runtime.playback().snapshot().succession.hasPrevious);
    runtime.playback().commands().pause();

    auto const removedIds = std::vector{historyTrack};
    auto const removed = runtime.library().writer().removeManualListTracks(manual.listId, removedIds);
    REQUIRE(removed);
    REQUIRE(removed->changed);
    REQUIRE_FALSE(runtime.playback().snapshot().succession.hasPrevious);
    REQUIRE(runtime.savePlaybackSession());

    auto const beforeState = runtime.playback().snapshot().succession;
    auto const beforePayload = storedSession(runtime.playbackSessionConfigStore());

    runtime.playback().commands().previous();

    CHECK(runtime.playback().snapshot().succession == beforeState);
    CHECK(runtime.playback().snapshot().transport.nowPlaying.trackId == current);
    CHECK(runtime.playback().snapshot().transport.transport == audio::Transport::Paused);
    REQUIRE(runtime.savePlaybackSession());
    CHECK(storedSession(runtime.playbackSessionConfigStore()) == beforePayload);

    auto const reinsertedIds = std::vector{historyTrack};
    auto const reinserted = runtime.library().writer().insertManualListTracks(manual.listId, 0, reinsertedIds);
    REQUIRE(reinserted);
    REQUIRE(reinserted->changed);
    CHECK_FALSE(runtime.playback().snapshot().succession.hasPrevious);
  }

  TEST_CASE("PlaybackSession - discard suppresses recreation until active intent changes",
            "[runtime][unit][playback-session][forget]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime);
    auto const track = addPlayableTrack(runtime, "Track");
    auto const viewId = createView(runtime);
    REQUIRE(runtime.playback().commands().startFromView(viewId, track));
    REQUIRE(runtime.savePlaybackSession());
    REQUIRE(runtime.discardRestorablePlaybackSession());
    CHECK_FALSE(*runtime.playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));

    SECTION("explicit checkpoint stays suppressed until a mode changes")
    {
      REQUIRE(runtime.savePlaybackSession());
      CHECK_FALSE(*runtime.playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));
      runtime.playback().commands().setRepeatMode(RepeatMode::All);
      REQUIRE(runtime.savePlaybackSession());
      CHECK(*runtime.playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));
    }

    SECTION("final seek admits and checkpoints the active session immediately")
    {
      runtime.playback().commands().seek(std::chrono::milliseconds{450});

      REQUIRE(*runtime.playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));
      CHECK(storedSession(runtime.playbackSessionConfigStore()).positionMs == 450);
    }
  }

  TEST_CASE("PlaybackSession - output capability changes do not revive a discarded session",
            "[runtime][regression][playback-session]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    runtime.addAudioProvider(std::make_unique<VolumeCapabilityProvider>());
    auto const track = addPlayableTrack(runtime, "Track");
    auto const viewId = createView(runtime);
    REQUIRE(runtime.playback().commands().startFromView(viewId, track));
    REQUIRE(runtime.savePlaybackSession());

    auto const volumeBefore = runtime.playback().snapshot().transport.volume;
    REQUIRE(runtime.discardRestorablePlaybackSession());
    runtime.playback().commands().setOutputDevice(
      audio::BackendId{"test_backend"}, audio::DeviceId{"hardware_volume_device"}, audio::kProfileShared);
    auto const volumeAfter = runtime.playback().snapshot().transport.volume;

    CHECK(volumeAfter.level == volumeBefore.level);
    CHECK(volumeAfter.muted == volumeBefore.muted);
    REQUIRE((volumeAfter.available != volumeBefore.available ||
             volumeAfter.hardwareAssisted != volumeBefore.hardwareAssisted));
    REQUIRE(runtime.savePlaybackSession());
    CHECK_FALSE(*runtime.playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));
  }

  TEST_CASE("PlaybackSession - failures preserve live state and diagnostics",
            "[runtime][unit][playback-session][error]")
  {
    SECTION("restore preparation failure is atomic")
    {
      auto tempDir = ao::test::TempDir{};
      auto runtime = makeRuntime(tempDir);
      addReadyAudioProvider(runtime);
      auto const live = addPlayableTrack(runtime, "Live");
      auto const viewId = createView(runtime);
      REQUIRE(runtime.playback().commands().startFromView(viewId, live));
      runtime.playback().commands().setRepeatMode(RepeatMode::All);
      runtime.playback().commands().setVolume(0.25F);
      REQUIRE(runtime.savePlaybackSession());
      auto const sequenceBefore = runtime.playback().snapshot().succession;
      auto const playbackBefore = runtime.playback().snapshot().transport;
      storeSession(runtime,
                   PlaybackSessionState{
                     .sourceListId = ListId{999'999},
                     .quickFilterExpression = "$year >",
                     .currentTrackId = live,
                     .volume = 0.75F,
                   });

      auto const restored = runtime.restorePlaybackSession();
      REQUIRE_FALSE(restored);
      CHECK(runtime.playback().snapshot().succession == sequenceBefore);
      CHECK(runtime.playback().snapshot().transport.nowPlaying == playbackBefore.nowPlaying);
      CHECK(runtime.playback().snapshot().transport.transport == playbackBefore.transport);
      CHECK(runtime.playback().snapshot().transport.volume.level == playbackBefore.volume.level);
    }

    SECTION("public commands keep cursor and transport matched for save")
    {
      auto tempDir = ao::test::TempDir{};
      auto runtime = makeRuntime(tempDir);
      addReadyAudioProvider(runtime);
      auto const cursorTrack = addPlayableTrack(runtime, "Cursor");
      auto const otherTrack = addPlayableTrack(runtime, "Other");
      auto const viewId = createView(runtime);
      REQUIRE(runtime.playback().commands().startFromView(viewId, cursorTrack));
      REQUIRE(runtime.playback().commands().startFromView(viewId, otherTrack));
      auto const snapshot = runtime.playback().snapshot();
      REQUIRE(snapshot.succession.currentTrackId == otherTrack);
      REQUIRE(snapshot.transport.nowPlaying.trackId == otherTrack);
      auto const saved = runtime.savePlaybackSession();
      REQUIRE(saved);
    }

    SECTION("flush failure returns an I/O diagnostic")
    {
      auto tempDir = ao::test::TempDir{};
      REQUIRE(std::filesystem::create_directory(tempDir.path() / "workspace.yaml"));
      auto runtime = makeRuntime(tempDir);
      addReadyAudioProvider(runtime);
      auto const track = addPlayableTrack(runtime, "Track");
      auto const viewId = createView(runtime);
      REQUIRE(runtime.playback().commands().startFromView(viewId, track));
      auto const saved = runtime.savePlaybackSession();
      REQUIRE_FALSE(saved);
      CHECK(saved.error().code == Error::Code::IoError);
    }

    SECTION("malformed config load retains diagnostics")
    {
      auto tempDir = ao::test::TempDir{};
      std::ofstream{tempDir.path() / "workspace.yaml"} << "playback-session: [not, a, map]\n";
      auto runtime = makeRuntime(tempDir);
      auto const restored = runtime.restorePlaybackSession();
      REQUIRE_FALSE(restored);
      CHECK(restored.error().code == Error::Code::FormatRejected);
    }
  }
} // namespace ao::rt::test
