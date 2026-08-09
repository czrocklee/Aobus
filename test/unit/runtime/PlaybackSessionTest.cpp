// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/PlaybackSessionState.h"
#include "runtime/PlaybackSessionYamlSchema.h"
#include "runtime/playback/PlaybackCursorSession.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Sleeper.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/PlaybackLaunchSpec.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_message.hpp>
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
#include <span>
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

    void settlePublication(QueuedExecutor& executor)
    {
      executor.drain();
    }
    void settlePublication(ManualExecutor& executor)
    {
      executor.runUntilIdle();
    }

    template<typename ExecutorT>
    TrackId addPlayableTrack(AppRuntime& runtime,
                             ExecutorT& executor,
                             std::string title,
                             std::uint16_t const year = 2020)
    {
      return addPlayableTrack(runtime, std::move(title), year, [&executor] { settlePublication(executor); });
    }

    ViewId createView(AppRuntime& runtime, std::string filterExpression = {}, std::vector<TrackSortTerm> sortBy = {})
    {
      runtime.reloadAllTracks();
      auto request = NavigationRequest{
        .target =
          FilteredListTarget{
            .listId = kAllTracksListId,
            .filterExpression = std::move(filterExpression),
          },
      };

      if (!sortBy.empty())
      {
        request.optPresentation = NavigationPresentation{.spec = TrackPresentationSpec{.sortBy = std::move(sortBy)}};
      }

      auto const createdRes = runtime.workspace().navigate(request);
      REQUIRE(createdRes);
      return *createdRes;
    }

    auto makePlaybackSessionRuntime(ao::test::TempDir const& tempDir,
                                    QueuedExecutor*& executor,
                                    ConfigStore* playbackSessionConfigStore = nullptr,
                                    async::Sleeper* sleeper = nullptr)
    {
      auto executorPtr = std::make_unique<QueuedExecutor>();
      executor = executorPtr.get();
      return makeRuntime(tempDir, std::move(executorPtr), playbackSessionConfigStore, sleeper);
    }

    template<typename ExecutorT>
    Result<> startFromViewAndWait(AppRuntime& runtime, ExecutorT& executor, ViewId const viewId, TrackId const trackId)
    {
      std::ignore = executor.drainUntil([&] { return runtime.playback().snapshot().transport.ready; });
      return admitPlaybackAndWait(
        executor,
        [&] { return runtime.playback().commands().startFromView(viewId, trackId); },
        [&] { return runtime.playback().snapshot().transport.positionRevision; });
    }

    PlaybackSessionState storedSession(ConfigStore& store)
    {
      auto session = PlaybackSessionState{};
      auto const loadedRes = store.load(kPlaybackSessionConfigGroup, session, PlaybackSessionYamlSchema{});
      REQUIRE(loadedRes);
      REQUIRE(*loadedRes);
      return session;
    }

    void storeSession(AppRuntime& runtime, PlaybackSessionState const& session)
    {
      REQUIRE(
        runtime.playbackSessionConfigStore().save(kPlaybackSessionConfigGroup, session, PlaybackSessionYamlSchema{}));
    }

    struct OrderedListView final
    {
      ListId listId = kInvalidListId;
      ViewId viewId = kInvalidViewId;
    };

    OrderedListView createOrderedListView(AppRuntime& runtime,
                                          QueuedExecutor& executor,
                                          std::vector<TrackId> trackIds,
                                          std::vector<TrackSortTerm> sortBy = {})
    {
      auto const membershipTag = std::array{std::string{"playbacksessionorder"}};
      auto targetsRes = runtime.library().bindTrackTargets(trackIds);
      INFO((targetsRes ? "initial membership targets bound" : targetsRes.error().message));
      REQUIRE(targetsRes);
      auto membershipResultValueRes = runtime.library().writer().editTags(*targetsRes, membershipTag, {});
      INFO((membershipResultValueRes ? "initial membership updated" : membershipResultValueRes.error().message));
      REQUIRE(membershipResultValueRes);
      auto const& membershipRes = *membershipResultValueRes;
      REQUIRE(
        (membershipRes.status == TrackAuthoringStatus::Applied || membershipRes.status == TrackAuthoringStatus::NoOp));
      executor.drain();
      runtime.reloadAllTracks();
      auto listRes = runtime.library().writer().createList(LibraryWriter::ListDraft{
        .name = "Playback session order",
        .expression = "#playbacksessionorder",
      });
      INFO((listRes ? "playback List created" : listRes.error().message));
      REQUIRE(listRes);
      auto const listId = *listRes;
      executor.drain();
      auto request = NavigationRequest{
        .target = FilteredListTarget{.listId = listId, .filterExpression = {}},
        .optPresentation =
          NavigationPresentation{.spec = TrackPresentationSpec{.id = std::string{kListOrderTrackPresentationId}}},
      };

      if (!sortBy.empty())
      {
        request.optPresentation = NavigationPresentation{.spec = TrackPresentationSpec{.sortBy = std::move(sortBy)}};
      }

      auto const createdRes = runtime.workspace().navigate(request);
      REQUIRE(createdRes);
      return OrderedListView{.listId = listId, .viewId = *createdRes};
    }

    void setOrderedListViewMembership(AppRuntime& runtime, std::span<TrackId const> const trackIds, bool const included)
    {
      auto const membershipTag = std::array{std::string{"playbacksessionorder"}};
      auto targetsRes = runtime.library().bindTrackTargets(trackIds);
      INFO((targetsRes ? "membership targets bound" : targetsRes.error().message));
      REQUIRE(targetsRes);
      auto resultValueRes = included ? runtime.library().writer().editTags(*targetsRes, membershipTag, {})
                                     : runtime.library().writer().editTags(*targetsRes, {}, membershipTag);
      INFO((resultValueRes ? "membership updated" : resultValueRes.error().message));
      REQUIRE(resultValueRes);
      auto const& result = *resultValueRes;
      REQUIRE((result.status == TrackAuthoringStatus::Applied || result.status == TrackAuthoringStatus::NoOp));
    }

    LibraryWriter::MoveOrderAuthoringResult moveOrderedListViewOrder(AppRuntime& runtime,
                                                                     OrderedListView const& view,
                                                                     std::span<TrackId const> const selectedTrackIds,
                                                                     std::optional<TrackId> const optBeforeTrackId)
    {
      auto const effectiveTrackIds = ao::test::requireValue(runtime.views().listSourceTrackIds(view.viewId));
      auto binding = ao::test::requireValue(runtime.library().bindListOrder(view.listId, effectiveTrackIds));
      return ao::test::requireValue(
        runtime.library().writer().moveListOrder(binding, selectedTrackIds, optBeforeTrackId));
    }

    std::vector<TrackId> projectionTrackIds(TrackListProjection const& projection)
    {
      auto trackIds = std::vector<TrackId>{};
      trackIds.reserve(projection.size());

      for (std::size_t index = 0; index < projection.size(); ++index)
      {
        trackIds.push_back(projection.trackIdAt(index));
      }

      return trackIds;
    }

    std::vector<TrackId> playbackProjectionTrackIds(AppRuntime& runtime, ViewId const viewId)
    {
      auto launchSpec = ao::test::requireValue(runtime.views().capturePlaybackLaunchSpec(viewId));
      auto const viewProjectionPtr = ao::test::requireValue(runtime.views().findTrackListProjection(viewId));
      REQUIRE(viewProjectionPtr->size() > 0);
      auto sessionPtr = ao::test::requireValue(
        PlaybackCursorSession::create(std::move(launchSpec),
                                      viewProjectionPtr->trackIdAt(0),
                                      runtime.sources(),
                                      runtime.musicLibrary(),
                                      RepeatMode::Off,
                                      ShuffleMode::Off,
                                      [](std::span<TrackId const> const candidates)
                                      { return candidates.empty() ? kInvalidTrackId : candidates.front(); }));

      auto trackIds = std::vector<TrackId>{};
      trackIds.reserve(sessionPtr->projectionSize());

      for (std::size_t index = 0; index < sessionPtr->projectionSize(); ++index)
      {
        trackIds.push_back(sessionPtr->trackIdAt(index));
      }

      return trackIds;
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
    // one property. Session restore applies volume then mute, so the fixture
    // exposes the resulting sequential partial state through the public restore
    // workflow. The arm outlives the backends that borrow it.
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

      Result<audio::OpenedPcmMode> open(audio::SignalFormat const& sourceFormat,
                                        audio::RenderTarget* renderTarget) override
      {
        _statePtr->renderTarget = renderTarget;
        return audio::NullBackend::open(sourceFormat, renderTarget);
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

  TEST_CASE("PlaybackCursorSession - invalid stored source filter rejects launch and restore",
            "[runtime][unit][playback-session][source]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Not playable through invalid source");
    auto listId = kInvalidListId;

    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      auto builder = library::ListBuilder::makeEmpty().name("Invalid source").filter("(");
      listId = ao::test::requireValue(
        transaction.apply([&builder](library::LibraryWrite& write) { return write.lists().create(builder); }));
      REQUIRE(transaction.commit());
    }

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto sources = TrackSourceCache{libraryFixture.library(), changes};
    sources.reloadAllTracks();

    SECTION("fresh launch")
    {
      auto const result = PlaybackCursorSession::create(PlaybackLaunchSpec{.sourceListId = listId},
                                                        trackId,
                                                        sources,
                                                        libraryFixture.library(),
                                                        RepeatMode::Off,
                                                        ShuffleMode::Off,
                                                        {});

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message.contains("List " + std::to_string(listId.raw()) + " stored filter"));
    }

    SECTION("restored cursor")
    {
      auto const result = PlaybackCursorSession::createForRestore(PlaybackLaunchSpec{.sourceListId = listId},
                                                                  trackId,
                                                                  0,
                                                                  sources,
                                                                  libraryFixture.library(),
                                                                  RepeatMode::Off,
                                                                  ShuffleMode::Off,
                                                                  {});

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message.contains("List " + std::to_string(listId.raw()) + " stored filter"));
    }
  }

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

    auto const decodedRes = PlaybackSessionYamlSchema{}.deserialize(tree.rootref(), PlaybackSessionState{});
    REQUIRE(decodedRes);
    CHECK(*decodedRes == state);
  }

  TEST_CASE("PlaybackSessionYamlSchema - rejects future and unknown YAML structure",
            "[runtime][unit][playback-session][schema]")
  {
    SECTION("Future version is reported before interpreting its payload")
    {
      auto const* source = "schemaVersion: 99\nsortBy: malformed\nfuture: true\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes = PlaybackSessionYamlSchema{}.deserialize(tree.rootref(), PlaybackSessionState{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::NotSupported);
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
      auto const decodedRes = PlaybackSessionYamlSchema{}.deserialize(tree.rootref(), PlaybackSessionState{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("future"));
    }
  }

  TEST_CASE("PlaybackSession - cursor payload round-trips without autoplay", "[runtime][unit][playback-session]")
  {
    auto tempDir = ao::test::TempDir{};
    auto playbackSessionStore = ConfigStore{tempDir.path() / "application.yaml"};
    auto* executor = static_cast<QueuedExecutor*>(nullptr);
    auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor, &playbackSessionStore);
    addReadyAudioProvider(*runtimePtr);
    auto const alpha = addPlayableTrack(*runtimePtr, *executor, "Alpha", 2022);
    std::ignore = addPlayableTrack(*runtimePtr, *executor, "Filtered", 1990);
    std::ignore = addPlayableTrack(*runtimePtr, *executor, "Zulu", 2023);
    auto const sortBy = std::vector{TrackSortTerm{.field = TrackSortField::Title, .ascending = false}};
    auto const viewId = createView(*runtimePtr, "$year > 2000", sortBy);

    REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, alpha));
    runtimePtr->playback().commands().seek(std::chrono::milliseconds{500});
    runtimePtr->playback().commands().setShuffleMode(ShuffleMode::On);
    runtimePtr->playback().commands().setRepeatMode(RepeatMode::All);
    runtimePtr->playback().commands().setVolume(0.5F);
    runtimePtr->playback().commands().setMuted(true);
    REQUIRE(runtimePtr->savePlaybackSession());

    CHECK_FALSE(*runtimePtr->workspaceConfigStore().contains(kPlaybackSessionConfigGroup));
    CHECK(*runtimePtr->playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));

    auto const saved = storedSession(runtimePtr->playbackSessionConfigStore());
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

    runtimePtr->playback().commands().stop();
    auto const restoredRes = runtimePtr->restorePlaybackSession();

    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    CHECK(restoredRes->trackId == alpha);
    CHECK(runtimePtr->playback().snapshot().transport.transport == audio::Transport::Idle);
    CHECK(runtimePtr->playback().snapshot().transport.nowPlaying.trackId == alpha);
    CHECK(runtimePtr->playback().snapshot().transport.elapsed == std::chrono::milliseconds{500});
    CHECK(runtimePtr->playback().snapshot().succession.shuffle == ShuffleMode::On);
    CHECK(runtimePtr->playback().snapshot().succession.repeat == RepeatMode::All);
  }

  TEST_CASE("PlaybackSession - explicit checkpoint starts event-driven debounce",
            "[runtime][unit][playback-session][timing]")
  {
    auto tempDir = ao::test::TempDir{};
    auto playbackSessionStore = ConfigStore{tempDir.path() / "application.yaml"};
    auto sleeper = ControlledSleeper{};
    auto executorPtr = std::make_unique<ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(tempDir, std::move(executorPtr), &playbackSessionStore, &sleeper);

    addReadyAudioProvider(*runtimePtr);
    executor->runUntilIdle();
    auto const trackId = addPlayableTrack(*runtimePtr, *executor, "Debounced Track", 2020);
    auto const viewId = createView(*runtimePtr);
    REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, trackId));
    REQUIRE(runtimePtr->savePlaybackSession());
    executor->runUntilIdle();

    runtimePtr->playback().commands().setVolume(0.4F);
    REQUIRE(sleeper.fireNext(std::chrono::seconds{1}));
    executor->checkQueued();
    REQUIRE(executor->runOne());

    CHECK(storedSession(runtimePtr->playbackSessionConfigStore()).volume == 0.4F);

    runtimePtr->playback().commands().setVolume(0.6F);
    REQUIRE(sleeper.fireNext(std::chrono::seconds{1}));
    executor->checkQueued();
    REQUIRE(executor->runOne());
    CHECK(storedSession(runtimePtr->playbackSessionConfigStore()).volume == 0.6F);
  }

  TEST_CASE("PlaybackSession - replacing a debounce suppresses its queued callback",
            "[runtime][regression][playback-session][concurrency]")
  {
    auto tempDir = ao::test::TempDir{};
    auto playbackSessionStore = ConfigStore{tempDir.path() / "application.yaml"};
    auto sleeper = ControlledSleeper{};
    auto executorPtr = std::make_unique<ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(tempDir, std::move(executorPtr), &playbackSessionStore, &sleeper);

    addReadyAudioProvider(*runtimePtr);
    executor->runUntilIdle();
    auto const trackId = addPlayableTrack(*runtimePtr, *executor, "Replaced debounce", 2020);
    auto const viewId = createView(*runtimePtr);
    REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, trackId));
    REQUIRE(runtimePtr->savePlaybackSession());
    executor->runUntilIdle();

    runtimePtr->playback().commands().setVolume(0.4F);
    REQUIRE(sleeper.fireNext(std::chrono::seconds{1}));
    executor->checkQueued();

    runtimePtr->playback().commands().setVolume(0.6F);
    REQUIRE(executor->runOne());
    CHECK(storedSession(playbackSessionStore).volume == 1.0F);

    REQUIRE(sleeper.fireNext(std::chrono::seconds{1}));
    executor->checkQueued();
    REQUIRE(executor->runOne());
    CHECK(storedSession(playbackSessionStore).volume == 0.6F);
  }

  TEST_CASE("PlaybackSession - shuffle debounce samples the latest elapsed position",
            "[runtime][regression][playback-session][timing]")
  {
    auto tempDir = ao::test::TempDir{};
    auto playbackSessionStore = ConfigStore{tempDir.path() / "application.yaml"};
    auto sleeper = ControlledSleeper{};
    auto executorPtr = std::make_unique<ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(tempDir, std::move(executorPtr), &playbackSessionStore, &sleeper);
    REQUIRE(runtimePtr->restorePlaybackSession());

    auto captureStatePtr = std::make_shared<RenderCaptureState>();
    runtimePtr->addAudioProvider(std::make_unique<RenderCaptureProvider>(captureStatePtr));
    executor->runUntilIdle();
    auto const trackId = addPlayableTrack(*runtimePtr, *executor, "Latent elapsed", 2020);
    auto const viewId = createView(*runtimePtr);
    REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, trackId));
    executor->runUntilIdle();
    REQUIRE(captureStatePtr->renderTarget != nullptr);

    auto output = std::array<std::byte, 4096>{};
    auto const renderRes = captureStatePtr->renderTarget->renderPcm(output);
    REQUIRE(renderRes.bytesWritten > 0);
    captureStatePtr->renderTarget->handlePositionAdvanced(renderRes.positionFrames);
    REQUIRE(runtimePtr->savePlaybackSession());

    auto sentinel = storedSession(playbackSessionStore);
    sentinel.positionMs = 0;
    sentinel.shuffleMode = ShuffleMode::Off;
    storeSession(*runtimePtr, sentinel);

    runtimePtr->playback().commands().setShuffleMode(ShuffleMode::On);

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

  TEST_CASE("PlaybackSession - next restorable state change saves after a failed debounce",
            "[runtime][unit][playback-session][timing]")
  {
    auto tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "application.yaml";
    auto playbackSessionStore = ConfigStore{configPath};
    auto sleeper = ControlledSleeper{};
    auto executorPtr = std::make_unique<ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(tempDir, std::move(executorPtr), &playbackSessionStore, &sleeper);
    auto const restoredRes = runtimePtr->restorePlaybackSession();
    REQUIRE(restoredRes);
    CHECK_FALSE(restoredRes->restored);

    addReadyAudioProvider(*runtimePtr);
    executor->runUntilIdle();
    auto const trackId = addPlayableTrack(*runtimePtr, *executor, "Deferred Save", 2020);
    auto const viewId = createView(*runtimePtr);
    REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, trackId));
    REQUIRE(runtimePtr->savePlaybackSession());
    executor->runUntilIdle();
    REQUIRE(std::filesystem::remove(configPath));
    REQUIRE(std::filesystem::create_directory(configPath));
    runtimePtr->playback().commands().setVolume(0.4F);

    REQUIRE(sleeper.fireNext(std::chrono::seconds{1}));
    executor->checkQueued();
    REQUIRE(executor->runOne());

    REQUIRE(std::filesystem::remove(configPath));
    runtimePtr->playback().commands().setVolume(0.6F);
    REQUIRE(sleeper.fireNext(std::chrono::seconds{1}));
    executor->checkQueued();
    REQUIRE(executor->runOne());
    CHECK(storedSession(runtimePtr->playbackSessionConfigStore()).volume == 0.6F);
  }

  TEST_CASE("PlaybackSession - launch publishes one coherent final live state",
            "[runtime][regression][playback-session][launch]")
  {
    auto tempDir = ao::test::TempDir{};
    auto* executor = static_cast<QueuedExecutor*>(nullptr);
    auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor);
    addReadyAudioProvider(*runtimePtr);
    auto const insertedBeforeCurrent = addPlayableTrack(*runtimePtr, *executor, "Inserted before current");
    auto const current = addPlayableTrack(*runtimePtr, *executor, "Current");
    auto const removedSuccessor = addPlayableTrack(*runtimePtr, *executor, "Removed successor");
    auto const finalSuccessor = addPlayableTrack(*runtimePtr, *executor, "Final successor");
    auto const orderedList = createOrderedListView(*runtimePtr, *executor, {current, removedSuccessor, finalSuccessor});
    executor->drain();
    auto const insertedIds = std::vector{insertedBeforeCurrent};
    auto const removedIds = std::vector{removedSuccessor};
    auto changedStates = std::vector<PlaybackSnapshot>{};
    auto const changedSubscription = runtimePtr->playback().events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot) noexcept { changedStates.push_back(snapshot); });

    setOrderedListViewMembership(*runtimePtr, insertedIds, true);
    executor->drain();
    setOrderedListViewMembership(*runtimePtr, removedIds, false);

    auto const launchedRes = startFromViewAndWait(*runtimePtr, *executor, orderedList.viewId, current);

    REQUIRE(launchedRes);
    auto const accepted = runtimePtr->playback().snapshot().succession;
    CHECK(accepted.sourceState == PlaybackSourceState::Live);
    CHECK(accepted.currentTrackId == current);
    CHECK(accepted.shuffle == ShuffleMode::Off);
    CHECK(accepted.repeat == RepeatMode::Off);
    CHECK(accepted.hasPrevious);
    CHECK(accepted.hasNext);
    REQUIRE(changedStates.size() == 1);
    CHECK(changedStates.front() == runtimePtr->playback().snapshot());
    CHECK(runtimePtr->playback().snapshot().transport.nowPlaying.trackId == current);
    CHECK(runtimePtr->playback().snapshot().transport.transport == audio::Transport::Playing);

    runtimePtr->playback().commands().next();

    CHECK(runtimePtr->playback().snapshot().succession.currentTrackId == finalSuccessor);
    CHECK(runtimePtr->playback().snapshot().transport.nowPlaying.trackId == finalSuccessor);
    CHECK(runtimePtr->playback().snapshot().transport.transport == audio::Transport::Playing);
  }

  TEST_CASE("PlaybackSession - restore defers nested playback commands issued by a snapshot observer",
            "[runtime][regression][playback-session][concurrency]")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(tempDir, std::move(executorPtr));
    addReadyAudioProvider(*runtimePtr);
    executor->drain();
    auto const firstTrackId = addPlayableTrack(*runtimePtr, *executor, "First", 2020);
    auto const secondTrackId = addPlayableTrack(*runtimePtr, *executor, "Second", 2020);
    auto const viewId = createView(*runtimePtr, {}, {{.field = TrackSortField::Title, .ascending = true}});
    executor->drain();
    REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, firstTrackId));
    REQUIRE(runtimePtr->savePlaybackSession());
    runtimePtr->playback().commands().stop();
    executor->drain();
    auto normalizedPayload = storedSession(runtimePtr->playbackSessionConfigStore());
    normalizedPayload.anchorIndex = 999;
    normalizedPayload.positionMs = 400;
    storeSession(*runtimePtr, normalizedPayload);

    std::uint32_t snapshotCount = 0;
    bool nestedCommandRequested = false;
    bool nestedRestoreAccepted = false;
    auto nestedRestoreError = Error::Code::Generic;
    bool nestedLaunchAccepted = false;
    auto observedSuccessionTrackId = kInvalidTrackId;
    auto observedTransportTrackId = kInvalidTrackId;
    auto observedElapsed = std::chrono::milliseconds{};
    auto snapshotSubscription = runtimePtr->playback().events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot) noexcept
      {
        ++snapshotCount;
        observedSuccessionTrackId = snapshot.succession.currentTrackId;
        observedTransportTrackId = snapshot.transport.nowPlaying.trackId;
        observedElapsed = snapshot.transport.elapsed;

        if (!nestedCommandRequested)
        {
          nestedCommandRequested = true;
          auto const nestedRestoreRes = runtimePtr->restorePlaybackSession();
          nestedRestoreAccepted = nestedRestoreRes.has_value();

          if (!nestedRestoreRes)
          {
            nestedRestoreError = nestedRestoreRes.error().code;
          }

          auto const nestedLaunchRes = runtimePtr->playback().commands().startFromView(viewId, secondTrackId);
          nestedLaunchAccepted = nestedLaunchRes.has_value();
        }
      });

    auto const restoredRes = runtimePtr->restorePlaybackSession();

    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    CHECK(snapshotCount == 1);
    CHECK(observedSuccessionTrackId == firstTrackId);
    CHECK(observedTransportTrackId == firstTrackId);
    CHECK(observedElapsed == std::chrono::milliseconds{400});
    CHECK(nestedCommandRequested);
    CHECK_FALSE(nestedRestoreAccepted);
    CHECK(nestedRestoreError == Error::Code::InvalidState);
    CHECK(nestedLaunchAccepted);
    CHECK(runtimePtr->playback().snapshot().succession.currentTrackId == firstTrackId);
    CHECK(runtimePtr->playback().snapshot().transport.nowPlaying.trackId == firstTrackId);
    CHECK(runtimePtr->playback().snapshot().succession.hasNext);
    CHECK_FALSE(runtimePtr->playback().snapshot().succession.hasPrevious);
    CHECK(runtimePtr->playback().snapshot().transport.elapsed == std::chrono::milliseconds{400});
    CHECK(runtimePtr->playback().snapshot().transport.transport == audio::Transport::Idle);

    snapshotSubscription.reset();
    REQUIRE(executor->drainUntil(
      [&]
      {
        return runtimePtr->playback().snapshot().succession.currentTrackId == secondTrackId &&
               runtimePtr->playback().snapshot().transport.nowPlaying.trackId == secondTrackId &&
               runtimePtr->playback().snapshot().transport.transport == audio::Transport::Playing;
      }));
  }

  TEST_CASE("PlaybackSession - restore does not overtake a pending observer command",
            "[runtime][regression][playback-session][concurrency]")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(tempDir, std::move(executorPtr));
    addReadyAudioProvider(*runtimePtr);
    executor->drain();
    auto const trackId = addPlayableTrack(*runtimePtr, *executor, "Pending restore", 2020);
    auto const viewId = createView(*runtimePtr);
    executor->drain();
    REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, trackId));
    REQUIRE(runtimePtr->savePlaybackSession());

    bool queuedRepeat = false;
    auto const snapshotSubscription = runtimePtr->playback().events().onSnapshot(
      [&](PlaybackSnapshot const& snapshot) noexcept
      {
        if (!queuedRepeat && snapshot.succession.shuffle == ShuffleMode::On)
        {
          queuedRepeat = true;
          runtimePtr->playback().commands().setRepeatMode(RepeatMode::All);
        }
      });

    runtimePtr->playback().commands().setShuffleMode(ShuffleMode::On);
    REQUIRE(queuedRepeat);

    auto const restoredRes = runtimePtr->restorePlaybackSession();

    REQUIRE_FALSE(restoredRes);
    CHECK(restoredRes.error().code == Error::Code::InvalidState);
    executor->drain();
    CHECK(runtimePtr->playback().snapshot().succession.repeat == RepeatMode::All);
  }

  TEST_CASE("PlaybackSession - same-subject restore publishes a changed offset",
            "[runtime][regression][playback-session][restore]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtimePtr = makeStateOnlyRuntime(tempDir);
    addReadyAudioProvider(*runtimePtr);
    auto const trackId = addPlayableTrack(*runtimePtr, "Restored offset");
    runtimePtr->reloadAllTracks();
    auto session = PlaybackSessionState{
      .sourceListId = kAllTracksListId,
      .currentTrackId = trackId,
      .positionMs = 250,
    };
    storeSession(*runtimePtr, session);
    REQUIRE(runtimePtr->restorePlaybackSession());
    auto const before = runtimePtr->playback().snapshot();
    REQUIRE(before.transport.elapsed == std::chrono::milliseconds{250});
    session.positionMs = 750;
    storeSession(*runtimePtr, session);

    auto const restoredRes = runtimePtr->restorePlaybackSession();

    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    auto const after = runtimePtr->playback().snapshot();
    CHECK(after.transport.positionRevision.value == before.transport.positionRevision.value + 1);
    CHECK(after.transport.finalSeekRevision == before.transport.finalSeekRevision);
    CHECK(after.transport.elapsed == std::chrono::milliseconds{750});
  }

  TEST_CASE("PlaybackSession - validates the complete serialized payload before lookup",
            "[runtime][unit][playback-session][error]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtimePtr = makeStateOnlyRuntime(tempDir);
    auto const trackId = addPlayableTrack(*runtimePtr, "Current");
    runtimePtr->reloadAllTracks();
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

    storeSession(*runtimePtr, payload);
    auto const restoredRes = runtimePtr->restorePlaybackSession();
    REQUIRE_FALSE(restoredRes);
    CHECK(restoredRes.error().code == expectedError);
  }

  TEST_CASE("PlaybackSession - exact schema rejects missing and malformed raw YAML fields",
            "[runtime][regression][playback-session][schema]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtimePtr = makeStateOnlyRuntime(tempDir);
    auto const trackId = addPlayableTrack(*runtimePtr, "Current");
    runtimePtr->reloadAllTracks();
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
    auto const restoredRes = runtimePtr->restorePlaybackSession();

    REQUIRE_FALSE(restoredRes);
    CHECK(restoredRes.error().code == Error::Code::FormatRejected);
    CHECK(runtimePtr->playback().snapshot().succession.sourceState == PlaybackSourceState::Inactive);
    CHECK(runtimePtr->playback().snapshot().transport.nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("PlaybackSession - restore resolves bound, gap, and replacement rows",
            "[runtime][unit][playback-session][restore-matrix]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtimePtr = makeStateOnlyRuntime(tempDir);
    addReadyAudioProvider(*runtimePtr);
    auto const first = addPlayableTrack(*runtimePtr, "First", 1990);
    auto const second = addPlayableTrack(*runtimePtr, "Second", 2022);
    std::ignore = addPlayableTrack(*runtimePtr, "Third", 2023);
    runtimePtr->reloadAllTracks();

    auto payload = PlaybackSessionState{
      .sourceListId = kAllTracksListId,
      .currentTrackId = first,
      .positionMs = 400,
    };

    SECTION("projected current is bound and retains position")
    {
      payload.anchorIndex = 0;
      storeSession(*runtimePtr, payload);
      auto const restoredRes = runtimePtr->restorePlaybackSession();
      REQUIRE(restoredRes);
      REQUIRE(restoredRes->restored);
      CHECK(restoredRes->trackId == first);
      CHECK(runtimePtr->playback().snapshot().transport.elapsed == std::chrono::milliseconds{400});
    }

    SECTION("existing filtered current remains a gap and retains position")
    {
      payload.quickFilterExpression = "$year > 2000";
      payload.anchorIndex = 1;
      storeSession(*runtimePtr, payload);
      auto const restoredRes = runtimePtr->restorePlaybackSession();
      REQUIRE(restoredRes);
      REQUIRE(restoredRes->restored);
      CHECK(restoredRes->trackId == first);
      CHECK(runtimePtr->playback().snapshot().transport.elapsed == std::chrono::milliseconds{400});
      CHECK(runtimePtr->playback().snapshot().succession.hasNext);
    }

    SECTION("missing current promotes the row at its saved anchor before shuffle")
    {
      payload.currentTrackId = TrackId{999'999};
      payload.anchorIndex = 1;
      payload.shuffleMode = ShuffleMode::On;
      storeSession(*runtimePtr, payload);
      auto const restoredRes = runtimePtr->restorePlaybackSession();
      REQUIRE(restoredRes);
      REQUIRE(restoredRes->restored);
      CHECK(restoredRes->trackId == second);
      CHECK(runtimePtr->playback().snapshot().transport.elapsed == std::chrono::milliseconds{0});
    }

    SECTION("missing current at end wraps only for repeat all")
    {
      payload.currentTrackId = TrackId{999'999};
      payload.anchorIndex = 3;
      payload.repeatMode = RepeatMode::All;
      storeSession(*runtimePtr, payload);
      auto const restoredRes = runtimePtr->restorePlaybackSession();
      REQUIRE(restoredRes);
      REQUIRE(restoredRes->restored);
      CHECK(restoredRes->trackId == first);
      CHECK(runtimePtr->playback().snapshot().transport.elapsed == std::chrono::milliseconds{0});
    }

    SECTION("missing current without deterministic successor is discarded")
    {
      payload.currentTrackId = TrackId{999'999};
      payload.anchorIndex = 3;
      storeSession(*runtimePtr, payload);
      auto const restoredRes = runtimePtr->restorePlaybackSession();
      REQUIRE(restoredRes);
      CHECK_FALSE(restoredRes->restored);
      CHECK(runtimePtr->playback().snapshot().succession.sourceState == PlaybackSourceState::Inactive);
    }
  }

  TEST_CASE("PlaybackSession - missing-source fallback preserves order and clears filter",
            "[runtime][unit][playback-session][restore-matrix]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtimePtr = makeStateOnlyRuntime(tempDir);
    addReadyAudioProvider(*runtimePtr);
    auto const current = addPlayableTrack(*runtimePtr, "Current", 1990);
    runtimePtr->reloadAllTracks();
    auto const sortBy = std::vector{TrackSortTerm{.field = TrackSortField::Title, .ascending = false}};
    storeSession(*runtimePtr,
                 PlaybackSessionState{
                   .sourceListId = ListId{999'999},
                   .quickFilterExpression = "$year > 2000",
                   .sortBy = sortBy,
                   .currentTrackId = current,
                   .positionMs = 250,
                 });

    auto const restoredRes = runtimePtr->restorePlaybackSession();
    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    CHECK(restoredRes->sourceListId == kAllTracksListId);
    REQUIRE(runtimePtr->savePlaybackSession());
    auto const corrected = storedSession(runtimePtr->playbackSessionConfigStore());
    CHECK(corrected.sourceListId == kAllTracksListId);
    CHECK(corrected.quickFilterExpression.empty());
    CHECK(corrected.sortBy == sortBy);

    runtimePtr->playback().commands().stop();
    storeSession(*runtimePtr,
                 PlaybackSessionState{
                   .sourceListId = ListId{999'999},
                   .currentTrackId = TrackId{888'888},
                 });
    auto const discardedRes = runtimePtr->restorePlaybackSession();
    REQUIRE(discardedRes);
    CHECK_FALSE(discardedRes->restored);
  }

  TEST_CASE("PlaybackSession - duration clamping restores zero", "[runtime][unit][playback-session][restore-matrix]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtimePtr = makeStateOnlyRuntime(tempDir);
    addReadyAudioProvider(*runtimePtr);
    auto const current = addPlayableTrack(*runtimePtr, "Current");
    runtimePtr->reloadAllTracks();
    storeSession(*runtimePtr,
                 PlaybackSessionState{
                   .sourceListId = kAllTracksListId,
                   .currentTrackId = current,
                   .positionMs = 10'000,
                 });

    auto const restoredRes = runtimePtr->restorePlaybackSession();
    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    CHECK(runtimePtr->playback().snapshot().transport.elapsed == std::chrono::milliseconds{0});
    REQUIRE(runtimePtr->savePlaybackSession());
    CHECK(storedSession(runtimePtr->playbackSessionConfigStore()).positionMs == 0);
  }

  TEST_CASE("PlaybackSession - volume and mute restore reports the first failure and publishes actual state",
            "[runtime][unit][playback-session][error]")
  {
    // The arm is declared before the runtime so the backends that borrow it stay
    // valid for the runtime's whole lifetime.
    auto arm = PropertyFailArm{};
    auto tempDir = ao::test::TempDir{};
    auto runtimePtr = makeStateOnlyRuntime(tempDir);
    runtimePtr->addAudioProvider(std::make_unique<PropertyFailProvider>(arm));
    auto const current = addPlayableTrack(*runtimePtr, "Current");
    runtimePtr->reloadAllTracks();

    // Establish a baseline live volume/mute while the backend still accepts writes.
    runtimePtr->playback().commands().setVolume(0.25F);
    runtimePtr->playback().commands().setMuted(false);
    auto const baseline = runtimePtr->playback().snapshot().transport.volume;
    REQUIRE(baseline.level == 0.25F);
    REQUIRE(baseline.muted == false);
    auto const snapshotBefore = runtimePtr->playback().snapshot();
    auto const sequenceBefore = snapshotBefore.succession;
    auto const playbackBefore = snapshotBefore.transport;

    auto const payload = PlaybackSessionState{
      .sourceListId = kAllTracksListId,
      .currentTrackId = current,
      .volume = 0.75F,
      .muted = true,
    };
    storeSession(*runtimePtr, payload);

    bool muteAttempted = false;

    SECTION("volume rejection publishes the requested volume and skips mute")
    {
      arm.arm(audio::PropertyId::Volume);
    }

    SECTION("mute rejection publishes both requested properties")
    {
      arm.arm(audio::PropertyId::Muted);
      muteAttempted = true;
    }

    auto const restoredRes = runtimePtr->restorePlaybackSession();

    REQUIRE_FALSE(restoredRes);
    CHECK(restoredRes.error().code == Error::Code::IoError);
    CHECK(runtimePtr->playback().snapshot().succession == sequenceBefore);
    auto const snapshotAfter = runtimePtr->playback().snapshot();
    auto const& playbackAfter = snapshotAfter.transport;
    CHECK(playbackAfter.transport == playbackBefore.transport);
    CHECK(playbackAfter.elapsed == playbackBefore.elapsed);
    CHECK(playbackAfter.duration == playbackBefore.duration);
    CHECK(playbackAfter.ready == playbackBefore.ready);
    CHECK(playbackAfter.nowPlaying == playbackBefore.nowPlaying);
    CHECK(playbackAfter.volume.level == payload.volume);
    CHECK(playbackAfter.volume.muted == (muteAttempted ? payload.muted : playbackBefore.volume.muted));
    CHECK(playbackAfter.volume.available == playbackBefore.volume.available);
    CHECK(playbackAfter.volume.hardwareAssisted == playbackBefore.volume.hardwareAssisted);
    CHECK(playbackAfter.output == playbackBefore.output);
    CHECK(playbackAfter.quality == playbackBefore.quality);
    CHECK(storedSession(runtimePtr->playbackSessionConfigStore()) == payload);
  }

  TEST_CASE("PlaybackSession - freezes invalidated and exhausted cursors as last-restorable state",
            "[runtime][unit][playback-session][lifecycle]")
  {
    SECTION("source invalidation and stop retain the frozen cursor")
    {
      auto tempDir = ao::test::TempDir{};
      auto* executor = static_cast<QueuedExecutor*>(nullptr);
      auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor);
      addReadyAudioProvider(*runtimePtr);
      auto const first = addPlayableTrack(*runtimePtr, *executor, "First");
      addPlayableTrack(*runtimePtr, *executor, "Second");
      runtimePtr->reloadAllTracks();
      auto const listId = ao::test::requireValue(runtimePtr->library().writer().createList(LibraryWriter::ListDraft{
        .name = "Temporary source",
      }));
      auto const viewRes = runtimePtr->workspace().navigate({.target = listId});
      REQUIRE(viewRes);
      REQUIRE(startFromViewAndWait(*runtimePtr, *executor, *viewRes, first));
      REQUIRE(runtimePtr->savePlaybackSession());

      auto const selected = runtimePtr->playback().snapshot().transport.output.selectedDevice;
      runtimePtr->playback().commands().setOutputDevice(selected.backendId, selected.deviceId, selected.profileId);
      REQUIRE(runtimePtr->library().writer().deleteList(listId));
      executor->drain();
      CHECK(runtimePtr->playback().snapshot().succession.sourceState == PlaybackSourceState::Invalidated);
      CHECK(runtimePtr->playback().snapshot().transport.nowPlaying.trackId == first);
      runtimePtr->playback().commands().pause();
      CHECK(runtimePtr->playback().snapshot().transport.transport == audio::Transport::Paused);
      runtimePtr->playback().commands().resume();
      REQUIRE(runtimePtr->savePlaybackSession());
      CHECK(storedSession(runtimePtr->playbackSessionConfigStore()).sourceListId == listId);

      runtimePtr->playback().commands().stop();
      CHECK(runtimePtr->playback().snapshot().succession.sourceState == PlaybackSourceState::Inactive);
      REQUIRE(runtimePtr->savePlaybackSession());
      auto const frozen = storedSession(runtimePtr->playbackSessionConfigStore());
      CHECK(frozen.sourceListId == listId);
      CHECK(frozen.currentTrackId == first);
    }

    SECTION("terminal exhaustion preserves the final current")
    {
      auto tempDir = ao::test::TempDir{};
      auto* executor = static_cast<QueuedExecutor*>(nullptr);
      auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor);
      addReadyAudioProvider(*runtimePtr);
      auto const only = addPlayableTrack(*runtimePtr, *executor, "Only");
      auto const viewId = createView(*runtimePtr);
      REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, only));
      runtimePtr->playback().commands().seek(std::chrono::milliseconds{350});
      REQUIRE(runtimePtr->savePlaybackSession());

      runtimePtr->playback().commands().next();

      CHECK(runtimePtr->playback().snapshot().succession.sourceState == PlaybackSourceState::Inactive);
      CHECK(runtimePtr->playback().snapshot().transport.transport == audio::Transport::Idle);
      REQUIRE(runtimePtr->savePlaybackSession());
      auto const frozen = storedSession(runtimePtr->playbackSessionConfigStore());
      CHECK(frozen.currentTrackId == only);
      CHECK(frozen.positionMs == 350);
      auto const restoredRes = runtimePtr->restorePlaybackSession();
      REQUIRE(restoredRes);
      REQUIRE(restoredRes->restored);
      CHECK(restoredRes->trackId == only);
      CHECK(runtimePtr->playback().snapshot().transport.elapsed == std::chrono::milliseconds{350});
    }
  }

  TEST_CASE("PlaybackSession - paused seek and live anchor mutation each become saveable",
            "[runtime][unit][playback-session]")
  {
    auto tempDir = ao::test::TempDir{};
    auto* executor = static_cast<QueuedExecutor*>(nullptr);
    auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor);
    addReadyAudioProvider(*runtimePtr);
    auto const first = addPlayableTrack(*runtimePtr, *executor, "First");
    auto const current = addPlayableTrack(*runtimePtr, *executor, "Second");
    std::ignore = addPlayableTrack(*runtimePtr, *executor, "Third");
    auto const viewId = createView(*runtimePtr);
    REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, current));
    runtimePtr->playback().commands().pause();
    REQUIRE(runtimePtr->savePlaybackSession());

    runtimePtr->playback().commands().seek(std::chrono::milliseconds{450});
    REQUIRE(runtimePtr->savePlaybackSession());
    CHECK(storedSession(runtimePtr->playbackSessionConfigStore()).positionMs == 450);

    REQUIRE(runtimePtr->library().writer().deleteTrack(first));
    executor->drain();
    REQUIRE(runtimePtr->savePlaybackSession());
    auto const moved = storedSession(runtimePtr->playbackSessionConfigStore());
    CHECK(moved.currentTrackId == current);
    CHECK(moved.anchorIndex == 0);
    CHECK(runtimePtr->playback().snapshot().transport.transport == audio::Transport::Paused);
  }

  TEST_CASE("PlaybackSession - List views and playback share Manual Order and sorted projections",
            "[runtime][regression][playback-session][list-order]")
  {
    auto tempDir = ao::test::TempDir{};
    auto* executor = static_cast<QueuedExecutor*>(nullptr);
    auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor);
    auto const alpha = addPlayableTrack(*runtimePtr, *executor, "Alpha");
    auto const bravo = addPlayableTrack(*runtimePtr, *executor, "Bravo");
    auto const charlie = addPlayableTrack(*runtimePtr, *executor, "Charlie");
    auto const orderedList = createOrderedListView(*runtimePtr, *executor, {alpha, bravo, charlie});
    auto const movedIds = std::vector{charlie};
    auto const moved = moveOrderedListViewOrder(*runtimePtr, orderedList, movedIds, alpha);
    REQUIRE(moved.status == ListOrderAuthoringStatus::Applied);
    executor->drain();

    auto viewProjectionPtr = ao::test::requireValue(runtimePtr->views().findTrackListProjection(orderedList.viewId));
    auto const manualOrder = std::vector{charlie, alpha, bravo};
    CHECK(projectionTrackIds(*viewProjectionPtr) == manualOrder);
    CHECK(playbackProjectionTrackIds(*runtimePtr, orderedList.viewId) == manualOrder);

    auto const titleSort = std::vector{TrackSortTerm{.field = TrackSortField::Title, .ascending = true}};
    REQUIRE(runtimePtr->views().setPresentation(orderedList.viewId, TrackPresentationSpec{.sortBy = titleSort}));
    auto const titleOrder = std::vector{alpha, bravo, charlie};
    CHECK(projectionTrackIds(*viewProjectionPtr) == titleOrder);
    CHECK(playbackProjectionTrackIds(*runtimePtr, orderedList.viewId) == titleOrder);
  }

  TEST_CASE("PlaybackSession - sorted List Gap ignores stored order changes with identical projected order",
            "[runtime][regression][playback-session][list-order]")
  {
    auto tempDir = ao::test::TempDir{};
    auto* executor = static_cast<QueuedExecutor*>(nullptr);
    auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor);
    addReadyAudioProvider(*runtimePtr);
    auto const current = addPlayableTrack(*runtimePtr, *executor, "Bravo");
    auto const alpha = addPlayableTrack(*runtimePtr, *executor, "Alpha");
    auto const charlie = addPlayableTrack(*runtimePtr, *executor, "Charlie");
    auto const titleSort = std::vector{TrackSortTerm{.field = TrackSortField::Title, .ascending = true}};
    auto const orderedList = createOrderedListView(*runtimePtr, *executor, {current, alpha, charlie}, titleSort);
    REQUIRE(startFromViewAndWait(*runtimePtr, *executor, orderedList.viewId, current));

    auto const currentIds = std::vector{current};
    setOrderedListViewMembership(*runtimePtr, currentIds, false);
    executor->drain();
    runtimePtr->playback().commands().pause();
    REQUIRE(runtimePtr->savePlaybackSession());

    auto const beforeState = runtimePtr->playback().snapshot().succession;
    REQUIRE(beforeState.hasNext);
    CHECK(beforeState.hasPrevious);
    auto const beforePayload = storedSession(runtimePtr->playbackSessionConfigStore());
    CHECK(beforePayload.currentTrackId == current);
    CHECK(beforePayload.anchorIndex == 1);
    CHECK(beforePayload.sortBy == titleSort);
    auto const projectionRes = runtimePtr->views().findTrackListProjection(orderedList.viewId);
    REQUIRE(projectionRes);
    auto const& projectionPtr = *projectionRes;
    REQUIRE(projectionPtr->size() == 2);
    CHECK(projectionPtr->trackIdAt(0) == alpha);
    CHECK(projectionPtr->trackIdAt(1) == charlie);
    std::uint32_t projectionBatchCount = 0;
    auto const projectionSubscription =
      projectionPtr->subscribe([&](TrackListProjectionDeltaBatch const&) noexcept { ++projectionBatchCount; });
    // The subscription synchronously publishes its initial snapshot; measure only the reorder below.
    projectionBatchCount = 0;

    auto const movedIds = std::vector{charlie};
    auto const moved = moveOrderedListViewOrder(*runtimePtr, orderedList, movedIds, alpha);
    REQUIRE(moved.status == ListOrderAuthoringStatus::Applied);
    executor->drain();

    CHECK(projectionBatchCount == 0);
    CHECK(runtimePtr->playback().snapshot().succession == beforeState);
    CHECK(runtimePtr->playback().snapshot().transport.nowPlaying.trackId == current);
    CHECK(runtimePtr->playback().snapshot().transport.transport == audio::Transport::Paused);
    REQUIRE(runtimePtr->savePlaybackSession());
    CHECK(storedSession(runtimePtr->playbackSessionConfigStore()) == beforePayload);
  }

  TEST_CASE("PlaybackSession - prepared replacement remains outside public playback and session state",
            "[runtime][regression][playback-session][snapshot]")
  {
    auto tempDir = ao::test::TempDir{};
    auto* executor = static_cast<QueuedExecutor*>(nullptr);
    auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor);
    addReadyAudioProvider(*runtimePtr);
    auto const first = addPlayableTrack(*runtimePtr, *executor, "First");
    auto const insertedTrack = addPlayableTrack(*runtimePtr, *executor, "Inserted successor");
    auto const originalSuccessor = addPlayableTrack(*runtimePtr, *executor, "Original successor");
    auto const orderedList = createOrderedListView(*runtimePtr, *executor, {first, originalSuccessor});
    REQUIRE(startFromViewAndWait(*runtimePtr, *executor, orderedList.viewId, first));
    runtimePtr->playback().commands().pause();
    REQUIRE(runtimePtr->savePlaybackSession());

    auto const beforeSnapshot = runtimePtr->playback().snapshot();
    REQUIRE(beforeSnapshot.succession.hasNext);
    auto const beforePayload = storedSession(runtimePtr->playbackSessionConfigStore());

    auto const insertedIds = std::vector{insertedTrack};
    setOrderedListViewMembership(*runtimePtr, insertedIds, true);
    executor->drain();

    auto const afterSnapshot = runtimePtr->playback().snapshot();
    CHECK(afterSnapshot.succession.currentTrackId == first);
    CHECK(afterSnapshot.succession.hasNext);
    CHECK(afterSnapshot == beforeSnapshot);
    CHECK(runtimePtr->playback().snapshot().transport.transport == audio::Transport::Paused);
    REQUIRE(runtimePtr->savePlaybackSession());
    CHECK(storedSession(runtimePtr->playbackSessionConfigStore()) == beforePayload);
  }

  TEST_CASE("PlaybackSession - shuffle source mutation remains transient when public state is unchanged",
            "[runtime][regression][playback-session][shuffle]")
  {
    auto tempDir = ao::test::TempDir{};
    auto* executor = static_cast<QueuedExecutor*>(nullptr);
    auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor);
    addReadyAudioProvider(*runtimePtr);
    auto const current = addPlayableTrack(*runtimePtr, *executor, "Current");
    auto const second = addPlayableTrack(*runtimePtr, *executor, "Second");
    auto const third = addPlayableTrack(*runtimePtr, *executor, "Third");
    auto const fourth = addPlayableTrack(*runtimePtr, *executor, "Fourth");
    auto const orderedList = createOrderedListView(*runtimePtr, *executor, {current, second, third, fourth});
    REQUIRE(startFromViewAndWait(*runtimePtr, *executor, orderedList.viewId, current));
    runtimePtr->playback().commands().setShuffleMode(ShuffleMode::On);
    runtimePtr->playback().commands().pause();
    REQUIRE(runtimePtr->savePlaybackSession());

    auto const beforeSnapshot = runtimePtr->playback().snapshot();
    REQUIRE(beforeSnapshot.succession.hasNext);
    auto const beforePayload = storedSession(runtimePtr->playbackSessionConfigStore());

    auto const removedIds = std::vector{second};
    setOrderedListViewMembership(*runtimePtr, removedIds, false);
    executor->drain();

    auto const afterSnapshot = runtimePtr->playback().snapshot();
    CHECK(afterSnapshot.succession.currentTrackId == current);
    CHECK(afterSnapshot.succession.hasNext);
    CHECK(afterSnapshot == beforeSnapshot);
    CHECK(runtimePtr->playback().snapshot().transport.transport == audio::Transport::Paused);
    REQUIRE(runtimePtr->savePlaybackSession());
    CHECK(storedSession(runtimePtr->playbackSessionConfigStore()) == beforePayload);
  }

  TEST_CASE("PlaybackSession - stale shuffle-history pop remains transient",
            "[runtime][regression][playback-session][shuffle]")
  {
    auto tempDir = ao::test::TempDir{};
    auto* executor = static_cast<QueuedExecutor*>(nullptr);
    auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor);
    addReadyAudioProvider(*runtimePtr);
    auto const historyTrack = addPlayableTrack(*runtimePtr, *executor, "History track");
    auto const second = addPlayableTrack(*runtimePtr, *executor, "Second");
    auto const third = addPlayableTrack(*runtimePtr, *executor, "Third");
    auto const orderedList = createOrderedListView(*runtimePtr, *executor, {historyTrack, second, third});
    REQUIRE(startFromViewAndWait(*runtimePtr, *executor, orderedList.viewId, historyTrack));
    runtimePtr->playback().commands().setShuffleMode(ShuffleMode::On);
    runtimePtr->playback().commands().next();
    auto const current = runtimePtr->playback().snapshot().succession.currentTrackId;
    REQUIRE(current != historyTrack);
    REQUIRE(runtimePtr->playback().snapshot().succession.hasPrevious);
    runtimePtr->playback().commands().pause();

    auto const removedIds = std::vector{historyTrack};
    setOrderedListViewMembership(*runtimePtr, removedIds, false);
    executor->drain();
    REQUIRE_FALSE(runtimePtr->playback().snapshot().succession.hasPrevious);
    REQUIRE(runtimePtr->savePlaybackSession());

    auto const beforeState = runtimePtr->playback().snapshot().succession;
    auto const beforePayload = storedSession(runtimePtr->playbackSessionConfigStore());

    runtimePtr->playback().commands().previous();

    CHECK(runtimePtr->playback().snapshot().succession == beforeState);
    CHECK(runtimePtr->playback().snapshot().transport.nowPlaying.trackId == current);
    CHECK(runtimePtr->playback().snapshot().transport.transport == audio::Transport::Paused);
    REQUIRE(runtimePtr->savePlaybackSession());
    CHECK(storedSession(runtimePtr->playbackSessionConfigStore()) == beforePayload);

    auto const reinsertedIds = std::vector{historyTrack};
    setOrderedListViewMembership(*runtimePtr, reinsertedIds, true);
    executor->drain();
    CHECK_FALSE(runtimePtr->playback().snapshot().succession.hasPrevious);
  }

  TEST_CASE("PlaybackSession - discard suppresses recreation until active state changes",
            "[runtime][unit][playback-session][forget]")
  {
    auto tempDir = ao::test::TempDir{};
    auto* executor = static_cast<QueuedExecutor*>(nullptr);
    auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor);
    addReadyAudioProvider(*runtimePtr);
    auto const track = addPlayableTrack(*runtimePtr, *executor, "Track");
    auto const viewId = createView(*runtimePtr);
    REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, track));
    REQUIRE(runtimePtr->savePlaybackSession());
    REQUIRE(runtimePtr->discardRestorablePlaybackSession());
    CHECK_FALSE(*runtimePtr->playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));

    SECTION("explicit checkpoint stays suppressed until a mode changes")
    {
      REQUIRE(runtimePtr->savePlaybackSession());
      CHECK_FALSE(*runtimePtr->playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));
      runtimePtr->playback().commands().setRepeatMode(RepeatMode::All);
      REQUIRE(runtimePtr->savePlaybackSession());
      CHECK(*runtimePtr->playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));
    }

    SECTION("final seek admits and checkpoints the active session immediately")
    {
      runtimePtr->playback().commands().seek(std::chrono::milliseconds{450});

      REQUIRE(*runtimePtr->playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));
      CHECK(storedSession(runtimePtr->playbackSessionConfigStore()).positionMs == 450);
    }
  }

  TEST_CASE("PlaybackSession - output capability changes do not revive a discarded session",
            "[runtime][regression][playback-session]")
  {
    auto tempDir = ao::test::TempDir{};
    auto* executor = static_cast<QueuedExecutor*>(nullptr);
    auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor);
    runtimePtr->addAudioProvider(std::make_unique<VolumeCapabilityProvider>());
    auto const track = addPlayableTrack(*runtimePtr, *executor, "Track");
    auto const viewId = createView(*runtimePtr);
    REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, track));
    REQUIRE(runtimePtr->savePlaybackSession());

    auto const volumeBefore = runtimePtr->playback().snapshot().transport.volume;
    REQUIRE(runtimePtr->discardRestorablePlaybackSession());
    runtimePtr->playback().commands().setOutputDevice(
      audio::BackendId{"test_backend"}, audio::DeviceId{"hardware_volume_device"}, audio::kProfileShared);
    auto const volumeAfter = runtimePtr->playback().snapshot().transport.volume;

    CHECK(volumeAfter.level == volumeBefore.level);
    CHECK(volumeAfter.muted == volumeBefore.muted);
    REQUIRE((volumeAfter.available != volumeBefore.available ||
             volumeAfter.hardwareAssisted != volumeBefore.hardwareAssisted));
    REQUIRE(runtimePtr->savePlaybackSession());
    CHECK_FALSE(*runtimePtr->playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));
  }

  TEST_CASE("PlaybackSession - failures preserve live state and diagnostics",
            "[runtime][unit][playback-session][error]")
  {
    SECTION("restore preparation failure is atomic")
    {
      auto tempDir = ao::test::TempDir{};
      auto* executor = static_cast<QueuedExecutor*>(nullptr);
      auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor);
      addReadyAudioProvider(*runtimePtr);
      auto const live = addPlayableTrack(*runtimePtr, *executor, "Live");
      auto const viewId = createView(*runtimePtr);
      REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, live));
      runtimePtr->playback().commands().setRepeatMode(RepeatMode::All);
      runtimePtr->playback().commands().setVolume(0.25F);
      REQUIRE(runtimePtr->savePlaybackSession());
      auto const sequenceBefore = runtimePtr->playback().snapshot().succession;
      auto const playbackBefore = runtimePtr->playback().snapshot().transport;
      storeSession(*runtimePtr,
                   PlaybackSessionState{
                     .sourceListId = ListId{999'999},
                     .quickFilterExpression = "$year >",
                     .currentTrackId = live,
                     .volume = 0.75F,
                   });

      auto const restoredRes = runtimePtr->restorePlaybackSession();
      REQUIRE_FALSE(restoredRes);
      CHECK(runtimePtr->playback().snapshot().succession == sequenceBefore);
      CHECK(runtimePtr->playback().snapshot().transport.nowPlaying == playbackBefore.nowPlaying);
      CHECK(runtimePtr->playback().snapshot().transport.transport == playbackBefore.transport);
      CHECK(runtimePtr->playback().snapshot().transport.volume.level == playbackBefore.volume.level);
    }

    SECTION("public commands keep cursor and transport matched for save")
    {
      auto tempDir = ao::test::TempDir{};
      auto* executor = static_cast<QueuedExecutor*>(nullptr);
      auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor);
      addReadyAudioProvider(*runtimePtr);
      auto const cursorTrack = addPlayableTrack(*runtimePtr, *executor, "Cursor");
      auto const otherTrack = addPlayableTrack(*runtimePtr, *executor, "Other");
      auto const viewId = createView(*runtimePtr);
      REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, cursorTrack));
      REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, otherTrack));
      auto const snapshot = runtimePtr->playback().snapshot();
      REQUIRE(snapshot.succession.currentTrackId == otherTrack);
      REQUIRE(snapshot.transport.nowPlaying.trackId == otherTrack);
      auto const savedRes = runtimePtr->savePlaybackSession();
      REQUIRE(savedRes);
    }

    SECTION("flush failure returns an I/O diagnostic")
    {
      auto tempDir = ao::test::TempDir{};
      REQUIRE(std::filesystem::create_directory(tempDir.path() / "workspace.yaml"));
      auto* executor = static_cast<QueuedExecutor*>(nullptr);
      auto runtimePtr = makePlaybackSessionRuntime(tempDir, executor);
      addReadyAudioProvider(*runtimePtr);
      auto const track = addPlayableTrack(*runtimePtr, *executor, "Track");
      auto const viewId = createView(*runtimePtr);
      REQUIRE(startFromViewAndWait(*runtimePtr, *executor, viewId, track));
      auto const savedRes = runtimePtr->savePlaybackSession();
      REQUIRE_FALSE(savedRes);
      CHECK(savedRes.error().code == Error::Code::IoError);
    }

    SECTION("malformed config load retains diagnostics")
    {
      auto tempDir = ao::test::TempDir{};
      std::ofstream{tempDir.path() / "workspace.yaml"} << "playback-session: [not, a, map]\n";
      auto runtimePtr = makeStateOnlyRuntime(tempDir);
      auto const restoredRes = runtimePtr->restorePlaybackSession();
      REQUIRE_FALSE(restoredRes);
      CHECK(restoredRes.error().code == Error::Code::FormatRejected);
    }
  }
} // namespace ao::rt::test
