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
#include <ao/audio/NullBackend.h>
#include <ao/audio/Property.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
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
    addReadyAudioProvider(runtime.playback());
    auto const alpha = addPlayableTrack(runtime, "Alpha", 2022);
    std::ignore = addPlayableTrack(runtime, "Filtered", 1990);
    std::ignore = addPlayableTrack(runtime, "Zulu", 2023);
    auto const sortBy = std::vector{TrackSortTerm{.field = TrackSortField::Title, .ascending = false}};
    auto const viewId = createView(runtime, "$year > 2000", sortBy);

    REQUIRE(runtime.playbackSequence().playFromView(viewId, alpha));
    runtime.playback().seek(std::chrono::milliseconds{500});
    runtime.playbackSequence().setShuffleMode(ShuffleMode::On);
    runtime.playbackSequence().setRepeatMode(RepeatMode::All);
    runtime.playback().setVolume(0.5F);
    runtime.playback().setMuted(true);
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

    runtime.playback().stop();
    std::uint32_t dirtyCount = 0;
    auto dirtySub = runtime.onPlaybackSessionDirty([&] { ++dirtyCount; });
    auto const restored = runtime.restorePlaybackSession();

    REQUIRE(restored);
    REQUIRE(restored->restored);
    CHECK(restored->trackId == alpha);
    CHECK(dirtyCount == 0);
    CHECK(runtime.playback().state().transport == audio::Transport::Idle);
    CHECK(runtime.playback().state().nowPlaying.trackId == alpha);
    CHECK(runtime.playback().state().elapsed == std::chrono::milliseconds{500});
    CHECK(runtime.playbackSequence().state().shuffle == ShuffleMode::On);
    CHECK(runtime.playbackSequence().state().repeat == RepeatMode::All);
  }

  TEST_CASE("PlaybackSession - persistence lifecycle debounces ordinary dirty state",
            "[runtime][unit][playback-session][timing]")
  {
    auto tempDir = ao::test::TempDir{};
    auto playbackSessionStore = ConfigStore{tempDir.path() / "application.yaml"};
    auto sleeper = ControlledSleeper{};
    auto executorPtr = std::make_unique<ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtime = makeRuntime(tempDir, std::move(executorPtr), &playbackSessionStore, &sleeper);
    auto const restored = runtime.restorePlaybackSession();
    REQUIRE(restored);
    CHECK_FALSE(restored->restored);
    REQUIRE(sleeper.waitForPendingDelay(std::chrono::seconds{10}));

    addReadyAudioProvider(runtime.playback());
    executor->runUntilIdle();
    auto const trackId = addPlayableTrack(runtime, "Debounced Track", 2020, [executor] { executor->runUntilIdle(); });
    auto const viewId = createView(runtime);
    REQUIRE(runtime.playbackSequence().playFromView(viewId, trackId));
    REQUIRE(runtime.savePlaybackSession());
    executor->runUntilIdle();

    runtime.playback().setVolume(0.4F);
    REQUIRE(sleeper.fireNext(std::chrono::seconds{1}));
    executor->checkQueued();
    REQUIRE(executor->runOne());

    CHECK(storedSession(runtime.playbackSessionConfigStore()).volume == 0.4F);

    runtime.playback().setVolume(0.6F);
    REQUIRE(sleeper.fireNext(std::chrono::seconds{10}));
    executor->checkQueued();
    REQUIRE(executor->runOne());
    CHECK(storedSession(runtime.playbackSessionConfigStore()).volume == 0.6F);
  }

  TEST_CASE("PlaybackSession - persistence lifecycle retries a failed debounced save",
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

    addReadyAudioProvider(runtime.playback());
    executor->runUntilIdle();
    auto const trackId = addPlayableTrack(runtime, "Retry Track", 2020, [executor] { executor->runUntilIdle(); });
    auto const viewId = createView(runtime);
    REQUIRE(runtime.playbackSequence().playFromView(viewId, trackId));
    REQUIRE(runtime.savePlaybackSession());
    executor->runUntilIdle();
    REQUIRE(std::filesystem::remove(configPath));
    REQUIRE(std::filesystem::create_directory(configPath));
    runtime.playback().setVolume(0.4F);

    for (auto const retryDelay : {std::chrono::seconds{1},
                                  std::chrono::seconds{1},
                                  std::chrono::seconds{2},
                                  std::chrono::seconds{4},
                                  std::chrono::seconds{8},
                                  std::chrono::seconds{16},
                                  std::chrono::seconds{32},
                                  std::chrono::seconds{60}})
    {
      REQUIRE(sleeper.fireNext(retryDelay));
      executor->checkQueued();
      REQUIRE(executor->runOne());
    }

    REQUIRE(sleeper.waitForPendingDelay(std::chrono::seconds{60}));
    REQUIRE(sleeper.waitForPendingDelay(std::chrono::seconds{10}));
  }

  TEST_CASE("PlaybackSession - launch contains preparing reentry and publishes final live intent once",
            "[runtime][regression][playback-session][launch]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto const insertedBeforeCurrent = addPlayableTrack(runtime, "Inserted before current");
    auto const current = addPlayableTrack(runtime, "Current");
    auto const removedSuccessor = addPlayableTrack(runtime, "Removed successor");
    auto const finalSuccessor = addPlayableTrack(runtime, "Final successor");
    auto const manual = createManualView(runtime, {current, removedSuccessor, finalSuccessor});
    auto const insertedIds = std::vector{insertedBeforeCurrent};
    auto const removedIds = std::vector{removedSuccessor};
    std::uint32_t dirtyCount = 0;
    std::uint32_t shuffleChangedCount = 0;
    std::uint32_t repeatChangedCount = 0;
    auto changedStates = std::vector<PlaybackSequenceState>{};
    bool callbackEntered = false;
    bool inserted = false;
    bool removed = false;
    bool nestedLaunchAccepted = false;
    auto nestedLaunchError = Error::Code::Generic;
    auto const dirtySubscription = runtime.onPlaybackSessionDirty([&] { ++dirtyCount; });
    auto const changedSubscription =
      runtime.playbackSequence().onChanged([&](PlaybackSequenceState const& state) { changedStates.push_back(state); });
    auto const shuffleSubscription = runtime.playbackSequence().onShuffleModeChanged(
      [&](PlaybackSequenceService::ShuffleModeChanged const&) { ++shuffleChangedCount; });
    auto const repeatSubscription = runtime.playbackSequence().onRepeatModeChanged(
      [&](PlaybackSequenceService::RepeatModeChanged const&) { ++repeatChangedCount; });
    auto const preparingSubscription = runtime.playback().onPreparing(
      [&]
      {
        if (callbackEntered)
        {
          return;
        }

        callbackEntered = true;
        auto const nestedLaunch = runtime.playbackSequence().playFromView(manual.viewId, current);
        nestedLaunchAccepted = nestedLaunch.has_value();

        if (!nestedLaunch)
        {
          nestedLaunchError = nestedLaunch.error().code;
        }

        runtime.playbackSequence().next();
        runtime.playbackSequence().previous();
        runtime.playbackSequence().clear();
        runtime.playbackSequence().setShuffleMode(ShuffleMode::On);
        runtime.playbackSequence().setRepeatMode(RepeatMode::All);

        if (auto const result = runtime.library().writer().insertManualListTracks(manual.listId, 0, insertedIds);
            result)
        {
          inserted = result->changed;
        }

        if (auto const result = runtime.library().writer().removeManualListTracks(manual.listId, removedIds); result)
        {
          removed = result->changed;
        }
      });

    auto const launched = runtime.playbackSequence().playFromView(manual.viewId, current);

    REQUIRE(launched);
    CHECK(callbackEntered);
    CHECK(inserted);
    CHECK(removed);
    CHECK_FALSE(nestedLaunchAccepted);
    CHECK(nestedLaunchError == Error::Code::InvalidState);
    CHECK(shuffleChangedCount == 0);
    CHECK(repeatChangedCount == 0);
    auto const accepted = runtime.playbackSequence().state();
    CHECK(accepted.sourceState == PlaybackSequenceSourceState::Live);
    CHECK(accepted.currentTrackId == current);
    CHECK(accepted.shuffle == ShuffleMode::Off);
    CHECK(accepted.repeat == RepeatMode::Off);
    CHECK(accepted.hasPrevious);
    CHECK(accepted.optResolvedSuccessor == finalSuccessor);
    REQUIRE(changedStates.size() == 1);
    CHECK(changedStates.front() == accepted);
    CHECK(dirtyCount == 1);
    CHECK(runtime.playback().state().nowPlaying.trackId == current);
    CHECK(runtime.playback().state().transport == audio::Transport::Playing);

    runtime.playbackSequence().next();

    CHECK(runtime.playbackSequence().state().currentTrackId == finalSuccessor);
    CHECK(runtime.playback().state().nowPlaying.trackId == finalSuccessor);
    CHECK(runtime.playback().state().transport == audio::Transport::Playing);
    CHECK(dirtyCount == 1);
  }

  TEST_CASE("PlaybackSession - restore contains publication observer exceptions and restores its command gates",
            "[runtime][regression][playback-session]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto const firstTrackId = addPlayableTrack(runtime, "First");
    auto const secondTrackId = addPlayableTrack(runtime, "Second");
    auto const viewId = createView(runtime, {}, {{.field = TrackSortField::Title, .ascending = true}});
    REQUIRE(runtime.playbackSequence().playFromView(viewId, firstTrackId));
    REQUIRE(runtime.savePlaybackSession());
    auto normalizedPayload = storedSession(runtime.playbackSessionConfigStore());
    normalizedPayload.anchorIndex = 999;
    normalizedPayload.positionMs = 400;
    storeSession(runtime, normalizedPayload);
    runtime.playback().stop();

    bool sequenceChanged = false;
    bool nowPlayingChanged = false;
    bool seekChanged = false;
    bool volumeChanged = false;
    bool mutedChanged = false;
    bool dirtyPublished = false;
    bool nestedRestoreAccepted = false;
    auto nestedRestoreError = Error::Code::Generic;
    bool nestedLaunchAccepted = false;
    auto nestedLaunchError = Error::Code::Generic;
    auto sequenceSubscription = runtime.playbackSequence().onChanged(
      [&](PlaybackSequenceState const&)
      {
        sequenceChanged = true;
        throwException<Exception>("scripted restored sequence observer failure");
      });
    auto nowPlayingSubscription = runtime.playback().onNowPlayingChanged(
      [&](PlaybackService::NowPlayingChanged const& event)
      {
        if (event.trackId != kInvalidTrackId)
        {
          nowPlayingChanged = true;
          throwException<Exception>("scripted restored now-playing observer failure");
        }
      });
    auto seekSubscription = runtime.playback().onSeekUpdate(
      [&](PlaybackService::SeekUpdate const&)
      {
        seekChanged = true;
        throwException<Exception>("scripted restored seek observer failure");
      });
    auto volumeSubscription = runtime.playback().onVolumeChanged(
      [&](float)
      {
        volumeChanged = true;
        throwException<Exception>("scripted restored volume observer failure");
      });
    auto mutedSubscription = runtime.playback().onMutedChanged(
      [&](bool)
      {
        mutedChanged = true;
        throwException<Exception>("scripted restored mute observer failure");
      });
    auto dirtySubscription = runtime.onPlaybackSessionDirty(
      [&]
      {
        if (!dirtyPublished)
        {
          dirtyPublished = true;
          auto const nestedRestore = runtime.restorePlaybackSession();
          nestedRestoreAccepted = nestedRestore.has_value();

          if (!nestedRestore)
          {
            nestedRestoreError = nestedRestore.error().code;
          }

          auto const nestedLaunch = runtime.playbackSequence().playFromView(viewId, secondTrackId);
          nestedLaunchAccepted = nestedLaunch.has_value();

          if (!nestedLaunch)
          {
            nestedLaunchError = nestedLaunch.error().code;
          }
        }

        dirtyPublished = true;
        throwException<Exception>("scripted normalized-session dirty observer failure");
      });

    auto const restored = runtime.restorePlaybackSession();

    REQUIRE(restored);
    REQUIRE(restored->restored);
    CHECK(sequenceChanged);
    CHECK(nowPlayingChanged);
    CHECK(seekChanged);
    CHECK(volumeChanged);
    CHECK(mutedChanged);
    CHECK(dirtyPublished);
    CHECK_FALSE(nestedRestoreAccepted);
    CHECK(nestedRestoreError == Error::Code::InvalidState);
    CHECK_FALSE(nestedLaunchAccepted);
    CHECK(nestedLaunchError == Error::Code::InvalidState);
    CHECK(runtime.playbackSequence().state().currentTrackId == firstTrackId);
    CHECK(runtime.playback().state().nowPlaying.trackId == firstTrackId);
    CHECK(runtime.playbackSequence().state().optResolvedSuccessor == secondTrackId);
    CHECK_FALSE(runtime.playbackSequence().state().hasPrevious);
    CHECK(runtime.playback().state().elapsed == std::chrono::milliseconds{400});
    CHECK(runtime.playback().state().transport == audio::Transport::Idle);

    sequenceSubscription.reset();
    nowPlayingSubscription.reset();
    seekSubscription.reset();
    volumeSubscription.reset();
    mutedSubscription.reset();
    dirtySubscription.reset();
    REQUIRE(runtime.savePlaybackSession());
    std::uint32_t dirtyCount = 0;
    auto const postRestoreDirtySubscription = runtime.onPlaybackSessionDirty([&] { ++dirtyCount; });
    runtime.playbackSequence().setRepeatMode(RepeatMode::All);

    CHECK(dirtyCount == 1);
    runtime.playback().resume();
    CHECK(runtime.playback().state().nowPlaying.trackId == firstTrackId);
    runtime.playbackSequence().next();
    CHECK(runtime.playbackSequence().state().currentTrackId == secondTrackId);
    CHECK(runtime.playback().state().nowPlaying.trackId == secondTrackId);
    CHECK(runtime.playback().state().transport == audio::Transport::Playing);
    REQUIRE(runtime.playbackSequence().playFromView(viewId, firstTrackId));
    CHECK(runtime.playbackSequence().state().currentTrackId == firstTrackId);
    CHECK(runtime.playback().state().nowPlaying.trackId == firstTrackId);
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
    CHECK(runtime.playbackSequence().state().sourceState == PlaybackSequenceSourceState::Inactive);
    CHECK(runtime.playback().state().nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("PlaybackSession - restore resolves bound, gap, and replacement rows",
            "[runtime][unit][playback-session][restore-matrix]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto const first = addPlayableTrack(runtime, "First", 1990);
    auto const second = addPlayableTrack(runtime, "Second", 2022);
    auto const third = addPlayableTrack(runtime, "Third", 2023);
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
      CHECK(runtime.playback().state().elapsed == std::chrono::milliseconds{400});
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
      CHECK(runtime.playback().state().elapsed == std::chrono::milliseconds{400});
      CHECK(runtime.playbackSequence().state().hasNext);
      CHECK(runtime.playbackSequence().state().optResolvedSuccessor == third);
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
      CHECK(runtime.playback().state().elapsed == std::chrono::milliseconds{0});
      std::uint32_t replayCount = 0;
      auto sub = runtime.onPlaybackSessionDirty([&] { ++replayCount; });
      CHECK(replayCount == 1);
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
      CHECK(runtime.playback().state().elapsed == std::chrono::milliseconds{0});
    }

    SECTION("missing current without deterministic successor is discarded")
    {
      payload.currentTrackId = TrackId{999'999};
      payload.anchorIndex = 3;
      storeSession(runtime, payload);
      auto const restored = runtime.restorePlaybackSession();
      REQUIRE(restored);
      CHECK_FALSE(restored->restored);
      CHECK(runtime.playbackSequence().state().sourceState == PlaybackSequenceSourceState::Inactive);
    }
  }

  TEST_CASE("PlaybackSession - missing-source fallback preserves order and clears filter",
            "[runtime][unit][playback-session][restore-matrix]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
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
    std::uint32_t replayCount = 0;
    auto sub = runtime.onPlaybackSessionDirty([&] { ++replayCount; });
    CHECK(replayCount == 1);
    REQUIRE(runtime.savePlaybackSession());
    auto const corrected = storedSession(runtime.playbackSessionConfigStore());
    CHECK(corrected.sourceListId == kAllTracksListId);
    CHECK(corrected.quickFilterExpression.empty());
    CHECK(corrected.sortBy == sortBy);

    runtime.playback().stop();
    storeSession(runtime,
                 PlaybackSessionState{
                   .sourceListId = ListId{999'999},
                   .currentTrackId = TrackId{888'888},
                 });
    auto const discarded = runtime.restorePlaybackSession();
    REQUIRE(discarded);
    CHECK_FALSE(discarded->restored);
  }

  TEST_CASE("PlaybackSession - duration clamping restores zero and starts dirty",
            "[runtime][unit][playback-session][restore-matrix]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
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
    CHECK(runtime.playback().state().elapsed == std::chrono::milliseconds{0});
    std::uint32_t replayCount = 0;
    auto sub = runtime.onPlaybackSessionDirty([&] { ++replayCount; });
    CHECK(replayCount == 1);
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
    runtime.playback().addProvider(std::make_unique<PropertyFailProvider>(arm));
    auto const current = addPlayableTrack(runtime, "Current");
    runtime.reloadAllTracks();

    // Establish a baseline live volume/mute while the backend still accepts writes.
    runtime.playback().setVolume(0.25F);
    runtime.playback().setMuted(false);
    auto const baseline = runtime.playback().state().volume;
    REQUIRE(baseline.level == 0.25F);
    REQUIRE(baseline.muted == false);
    auto const sequenceBefore = runtime.playbackSequence().state();
    auto const playbackBefore = runtime.playback().state();
    std::uint32_t dirtyCount = 0;
    auto dirtySub = runtime.onPlaybackSessionDirty([&] { ++dirtyCount; });

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
    CHECK(runtime.playbackSequence().state() == sequenceBefore);
    auto const& playbackAfter = runtime.playback().state();
    CHECK(playbackAfter.transport == playbackBefore.transport);
    CHECK(playbackAfter.elapsed == playbackBefore.elapsed);
    CHECK(playbackAfter.duration == playbackBefore.duration);
    CHECK(playbackAfter.ready == playbackBefore.ready);
    CHECK(playbackAfter.nowPlaying == playbackBefore.nowPlaying);
    CHECK(playbackAfter.volume == playbackBefore.volume);
    CHECK(playbackAfter.output == playbackBefore.output);
    CHECK(playbackAfter.quality == playbackBefore.quality);
    CHECK(playbackAfter.revision == playbackBefore.revision);
    CHECK(storedSession(runtime.playbackSessionConfigStore()) == payload);
    CHECK(dirtyCount == 0);
    std::uint32_t lateReplayCount = 0;
    auto lateSub = runtime.onPlaybackSessionDirty([&] { ++lateReplayCount; });
    CHECK(lateReplayCount == 0);
  }

  TEST_CASE("PlaybackSession - freezes invalidated and exhausted cursors as last-restorable intent",
            "[runtime][unit][playback-session][lifecycle]")
  {
    SECTION("source invalidation is non-dirty and stop retains its frozen cursor")
    {
      auto tempDir = ao::test::TempDir{};
      auto runtime = makeRuntime(tempDir);
      addReadyAudioProvider(runtime.playback());
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
      REQUIRE(runtime.playbackSequence().playFromView(view->viewId, first));
      REQUIRE(runtime.savePlaybackSession());
      std::uint32_t dirtyCount = 0;
      auto sub = runtime.onPlaybackSessionDirty([&] { ++dirtyCount; });

      auto const selected = runtime.playback().state().output.selectedDevice;
      runtime.playback().setOutputDevice(selected.backendId, selected.deviceId, selected.profileId);
      CHECK(dirtyCount == 0);
      REQUIRE(runtime.library().writer().deleteList(listId));
      CHECK(runtime.playbackSequence().state().sourceState == PlaybackSequenceSourceState::Invalidated);
      CHECK(runtime.playback().state().nowPlaying.trackId == first);
      CHECK(dirtyCount == 0);
      runtime.playback().pause();
      CHECK(runtime.playback().state().transport == audio::Transport::Paused);
      runtime.playback().resume();
      REQUIRE(runtime.savePlaybackSession());
      CHECK(storedSession(runtime.playbackSessionConfigStore()).sourceListId == listId);

      runtime.playback().stop();
      CHECK(runtime.playbackSequence().state().sourceState == PlaybackSequenceSourceState::Inactive);
      REQUIRE(runtime.savePlaybackSession());
      auto const frozen = storedSession(runtime.playbackSessionConfigStore());
      CHECK(frozen.sourceListId == listId);
      CHECK(frozen.currentTrackId == first);
    }

    SECTION("terminal exhaustion preserves the final current")
    {
      auto tempDir = ao::test::TempDir{};
      auto runtime = makeRuntime(tempDir);
      addReadyAudioProvider(runtime.playback());
      auto const only = addPlayableTrack(runtime, "Only");
      auto const viewId = createView(runtime);
      REQUIRE(runtime.playbackSequence().playFromView(viewId, only));
      runtime.playback().seek(std::chrono::milliseconds{350});
      REQUIRE(runtime.savePlaybackSession());

      runtime.playbackSequence().next();

      CHECK(runtime.playbackSequence().state().sourceState == PlaybackSequenceSourceState::Inactive);
      CHECK(runtime.playback().state().transport == audio::Transport::Idle);
      REQUIRE(runtime.savePlaybackSession());
      auto const frozen = storedSession(runtime.playbackSessionConfigStore());
      CHECK(frozen.currentTrackId == only);
      CHECK(frozen.positionMs == 350);
      auto const restored = runtime.restorePlaybackSession();
      REQUIRE(restored);
      REQUIRE(restored->restored);
      CHECK(restored->trackId == only);
      CHECK(runtime.playback().state().elapsed == std::chrono::milliseconds{350});
    }
  }

  TEST_CASE("PlaybackSession - paused seek and live anchor mutation each become saveable",
            "[runtime][unit][playback-session][dirty]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto const first = addPlayableTrack(runtime, "First");
    auto const current = addPlayableTrack(runtime, "Second");
    std::ignore = addPlayableTrack(runtime, "Third");
    auto const viewId = createView(runtime);
    REQUIRE(runtime.playbackSequence().playFromView(viewId, current));
    runtime.playback().pause();
    REQUIRE(runtime.savePlaybackSession());
    std::uint32_t dirtyCount = 0;
    auto sub = runtime.onPlaybackSessionDirty([&] { ++dirtyCount; });

    runtime.playback().seek(std::chrono::milliseconds{450});
    CHECK(dirtyCount == 1);
    REQUIRE(runtime.savePlaybackSession());
    CHECK(storedSession(runtime.playbackSessionConfigStore()).positionMs == 450);

    dirtyCount = 0;
    REQUIRE(runtime.library().writer().deleteTrack(first));
    CHECK(dirtyCount == 1);
    REQUIRE(runtime.savePlaybackSession());
    auto const moved = storedSession(runtime.playbackSessionConfigStore());
    CHECK(moved.currentTrackId == current);
    CHECK(moved.anchorIndex == 0);
    CHECK(runtime.playback().state().transport == audio::Transport::Paused);
  }

  TEST_CASE("PlaybackSession - clean periodic saves sample transport without dirty churn",
            "[runtime][unit][playback-session][dirty]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto const track = addPlayableTrack(runtime, "Track");
    auto const viewId = createView(runtime);
    REQUIRE(runtime.playbackSequence().playFromView(viewId, track));
    runtime.playback().seek(std::chrono::milliseconds{600});
    REQUIRE(runtime.savePlaybackSession());
    std::uint32_t dirtyCount = 0;
    auto sub = runtime.onPlaybackSessionDirty([&] { ++dirtyCount; });

    CHECK(runtime.playback().elapsed() == std::chrono::milliseconds{600});
    CHECK(runtime.playback().elapsed() == std::chrono::milliseconds{600});
    REQUIRE(runtime.savePlaybackSession());

    CHECK(dirtyCount == 0);
    CHECK(storedSession(runtime.playbackSessionConfigStore()).positionMs == 600);
  }

  TEST_CASE("PlaybackSession - sorted manual Gap ignores stored reorders with identical projected order",
            "[runtime][regression][playback-session][manual-list]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto const current = addPlayableTrack(runtime, "Bravo");
    auto const alpha = addPlayableTrack(runtime, "Alpha");
    auto const charlie = addPlayableTrack(runtime, "Charlie");
    auto const titleSort = std::vector{TrackSortTerm{.field = TrackSortField::Title, .ascending = true}};
    auto const manual = createManualView(runtime, {current, alpha, charlie}, titleSort);
    REQUIRE(runtime.playbackSequence().playFromView(manual.viewId, current));

    auto const currentIds = std::vector{current};
    auto const removed = runtime.library().writer().removeManualListTracks(manual.listId, currentIds);
    REQUIRE(removed);
    REQUIRE(removed->changed);
    runtime.playback().pause();
    REQUIRE(runtime.savePlaybackSession());

    auto const beforeState = runtime.playbackSequence().state();
    REQUIRE(beforeState.optResolvedSuccessor == charlie);
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
    std::uint32_t dirtyCount = 0;
    auto const dirtySubscription = runtime.onPlaybackSessionDirty([&] { ++dirtyCount; });

    auto const movedIds = std::vector{charlie};
    auto const moved = runtime.library().writer().moveManualListTracks(manual.listId, movedIds, 0);
    REQUIRE(moved);
    REQUIRE(moved->changed);

    CHECK(projectionPtr->revision() == projectionRevision);
    CHECK(projectionBatchCount == 0);
    CHECK(runtime.playbackSequence().state() == beforeState);
    CHECK(runtime.playback().state().nowPlaying.trackId == current);
    CHECK(runtime.playback().state().transport == audio::Transport::Paused);
    CHECK(dirtyCount == 0);
    REQUIRE(runtime.savePlaybackSession());
    CHECK(storedSession(runtime.playbackSessionConfigStore()) == beforePayload);
  }

  TEST_CASE("PlaybackSession - prepared replacement and retirement remain transient",
            "[runtime][regression][playback-session][token]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto const first = addPlayableTrack(runtime, "First");
    auto const insertedTrack = addPlayableTrack(runtime, "Inserted successor");
    auto const originalSuccessor = addPlayableTrack(runtime, "Original successor");
    auto const manual = createManualView(runtime, {first, originalSuccessor});
    REQUIRE(runtime.playbackSequence().playFromView(manual.viewId, first));
    runtime.playback().pause();
    REQUIRE(runtime.savePlaybackSession());

    auto const beforeState = runtime.playbackSequence().state();
    REQUIRE(beforeState.optResolvedSuccessor == originalSuccessor);
    auto const beforePayload = storedSession(runtime.playbackSessionConfigStore());
    std::uint32_t dirtyCount = 0;
    auto const dirtySubscription = runtime.onPlaybackSessionDirty([&] { ++dirtyCount; });
    auto const optOldToken = runtime.playback().clearPreparedNext();
    REQUIRE(optOldToken);

    auto const insertedIds = std::vector{insertedTrack};
    auto const inserted = runtime.library().writer().insertManualListTracks(manual.listId, 1, insertedIds);
    REQUIRE(inserted);
    REQUIRE(inserted->changed);

    auto const afterState = runtime.playbackSequence().state();
    CHECK(afterState.currentTrackId == first);
    CHECK(afterState.optResolvedSuccessor == insertedTrack);
    CHECK(afterState.semanticRevision == beforeState.semanticRevision + 1);
    auto const optReplacementToken = runtime.playback().clearPreparedNext();
    REQUIRE(optReplacementToken);
    CHECK(*optReplacementToken != *optOldToken);
    CHECK(runtime.playback().state().transport == audio::Transport::Paused);
    CHECK(dirtyCount == 0);
    REQUIRE(runtime.savePlaybackSession());
    CHECK(storedSession(runtime.playbackSessionConfigStore()) == beforePayload);
  }

  TEST_CASE("PlaybackSession - sticky shuffle reroll remains transient",
            "[runtime][regression][playback-session][shuffle]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto const current = addPlayableTrack(runtime, "Current");
    auto const second = addPlayableTrack(runtime, "Second");
    auto const third = addPlayableTrack(runtime, "Third");
    auto const fourth = addPlayableTrack(runtime, "Fourth");
    auto const manual = createManualView(runtime, {current, second, third, fourth});
    REQUIRE(runtime.playbackSequence().playFromView(manual.viewId, current));
    runtime.playbackSequence().setShuffleMode(ShuffleMode::On);
    runtime.playback().pause();
    REQUIRE(runtime.savePlaybackSession());

    auto const beforeState = runtime.playbackSequence().state();
    REQUIRE(beforeState.optResolvedSuccessor);
    auto const oldCandidate = *beforeState.optResolvedSuccessor;
    CHECK(oldCandidate != current);
    auto const beforePayload = storedSession(runtime.playbackSessionConfigStore());
    std::uint32_t dirtyCount = 0;
    auto const dirtySubscription = runtime.onPlaybackSessionDirty([&] { ++dirtyCount; });

    auto const removedIds = std::vector{oldCandidate};
    auto const removed = runtime.library().writer().removeManualListTracks(manual.listId, removedIds);
    REQUIRE(removed);
    REQUIRE(removed->changed);

    auto const afterState = runtime.playbackSequence().state();
    CHECK(afterState.currentTrackId == current);
    REQUIRE(afterState.optResolvedSuccessor);
    CHECK(*afterState.optResolvedSuccessor != current);
    CHECK(*afterState.optResolvedSuccessor != oldCandidate);
    CHECK(afterState.semanticRevision == beforeState.semanticRevision + 1);
    CHECK(runtime.playback().state().transport == audio::Transport::Paused);
    CHECK(dirtyCount == 0);
    REQUIRE(runtime.savePlaybackSession());
    CHECK(storedSession(runtime.playbackSessionConfigStore()) == beforePayload);
  }

  TEST_CASE("PlaybackSession - stale shuffle-history pop remains transient",
            "[runtime][regression][playback-session][shuffle]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto const historyTrack = addPlayableTrack(runtime, "History track");
    auto const second = addPlayableTrack(runtime, "Second");
    auto const third = addPlayableTrack(runtime, "Third");
    auto const manual = createManualView(runtime, {historyTrack, second, third});
    REQUIRE(runtime.playbackSequence().playFromView(manual.viewId, historyTrack));
    runtime.playbackSequence().setShuffleMode(ShuffleMode::On);
    runtime.playbackSequence().next();
    auto const current = runtime.playbackSequence().state().currentTrackId;
    REQUIRE(current != historyTrack);
    REQUIRE(runtime.playbackSequence().hasPrevious());
    runtime.playback().pause();

    auto const removedIds = std::vector{historyTrack};
    auto const removed = runtime.library().writer().removeManualListTracks(manual.listId, removedIds);
    REQUIRE(removed);
    REQUIRE(removed->changed);
    REQUIRE_FALSE(runtime.playbackSequence().hasPrevious());
    REQUIRE(runtime.savePlaybackSession());

    auto const beforeState = runtime.playbackSequence().state();
    auto const beforePayload = storedSession(runtime.playbackSessionConfigStore());
    std::uint32_t dirtyCount = 0;
    auto dirtySubscription = runtime.onPlaybackSessionDirty([&] { ++dirtyCount; });

    runtime.playbackSequence().previous();

    CHECK(runtime.playbackSequence().state() == beforeState);
    CHECK(runtime.playback().state().nowPlaying.trackId == current);
    CHECK(runtime.playback().state().transport == audio::Transport::Paused);
    CHECK(dirtyCount == 0);
    REQUIRE(runtime.savePlaybackSession());
    CHECK(storedSession(runtime.playbackSessionConfigStore()) == beforePayload);

    dirtySubscription.reset();
    auto const reinsertedIds = std::vector{historyTrack};
    auto const reinserted = runtime.library().writer().insertManualListTracks(manual.listId, 0, reinsertedIds);
    REQUIRE(reinserted);
    REQUIRE(reinserted->changed);
    CHECK_FALSE(runtime.playbackSequence().hasPrevious());
  }

  TEST_CASE("PlaybackSession - dirty lifecycle acknowledges exact saves and replays late subscription",
            "[runtime][unit][playback-session][dirty]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto const track = addPlayableTrack(runtime, "Track");
    auto const viewId = createView(runtime);

    REQUIRE(runtime.playbackSequence().playFromView(viewId, track));
    std::uint32_t dirtyCount = 0;
    auto sub = runtime.onPlaybackSessionDirty([&] { ++dirtyCount; });
    CHECK(dirtyCount == 1);
    runtime.playbackSequence().setRepeatMode(RepeatMode::All);
    runtime.playback().seek(std::chrono::milliseconds{200});
    runtime.playback().setVolume(0.5F);
    CHECK(dirtyCount == 1);
    REQUIRE(runtime.savePlaybackSession());

    std::uint32_t postSaveCount = 0;
    auto postSaveSub = runtime.onPlaybackSessionDirty([&] { ++postSaveCount; });
    CHECK(postSaveCount == 0);
    runtime.playbackSequence().setRepeatMode(RepeatMode::All);
    runtime.playback().seek(std::chrono::milliseconds{200});
    runtime.playback().setVolume(0.5F);
    CHECK(postSaveCount == 0);
    runtime.playback().seek(std::chrono::milliseconds{300});
    runtime.playback().setMuted(true);
    CHECK(postSaveCount == 1);
  }

  TEST_CASE("PlaybackSession - forget suppresses periodic recreation until active intent changes",
            "[runtime][unit][playback-session][forget]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto const track = addPlayableTrack(runtime, "Track");
    auto const viewId = createView(runtime);
    REQUIRE(runtime.playbackSequence().playFromView(viewId, track));
    REQUIRE(runtime.savePlaybackSession());
    REQUIRE(runtime.discardRestorablePlaybackSession());
    CHECK_FALSE(*runtime.playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));

    REQUIRE(runtime.savePlaybackSession());
    CHECK_FALSE(*runtime.playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));
    runtime.playbackSequence().setRepeatMode(RepeatMode::All);
    REQUIRE(runtime.savePlaybackSession());
    CHECK(*runtime.playbackSessionConfigStore().contains(kPlaybackSessionConfigGroup));
  }

  TEST_CASE("PlaybackSession - failures preserve live state and dirty intent",
            "[runtime][unit][playback-session][error]")
  {
    SECTION("restore preparation failure is atomic")
    {
      auto tempDir = ao::test::TempDir{};
      auto runtime = makeRuntime(tempDir);
      addReadyAudioProvider(runtime.playback());
      auto const live = addPlayableTrack(runtime, "Live");
      auto const viewId = createView(runtime);
      REQUIRE(runtime.playbackSequence().playFromView(viewId, live));
      runtime.playbackSequence().setRepeatMode(RepeatMode::All);
      runtime.playback().setVolume(0.25F);
      REQUIRE(runtime.savePlaybackSession());
      std::uint32_t dirtyCount = 0;
      auto dirtySub = runtime.onPlaybackSessionDirty([&] { ++dirtyCount; });
      auto const sequenceBefore = runtime.playbackSequence().state();
      auto const playbackBefore = runtime.playback().state();
      storeSession(runtime,
                   PlaybackSessionState{
                     .sourceListId = ListId{999'999},
                     .quickFilterExpression = "$year >",
                     .currentTrackId = live,
                     .volume = 0.75F,
                   });

      auto const restored = runtime.restorePlaybackSession();
      REQUIRE_FALSE(restored);
      CHECK(runtime.playbackSequence().state() == sequenceBefore);
      CHECK(runtime.playback().state().nowPlaying == playbackBefore.nowPlaying);
      CHECK(runtime.playback().state().transport == playbackBefore.transport);
      CHECK(runtime.playback().state().volume.level == playbackBefore.volume.level);
      CHECK(dirtyCount == 0);
      std::uint32_t lateReplayCount = 0;
      auto lateSub = runtime.onPlaybackSessionDirty([&] { ++lateReplayCount; });
      CHECK(lateReplayCount == 0);
    }

    SECTION("cursor and transport mismatch rejects save")
    {
      auto tempDir = ao::test::TempDir{};
      auto runtime = makeRuntime(tempDir);
      addReadyAudioProvider(runtime.playback());
      auto const cursorTrack = addPlayableTrack(runtime, "Cursor");
      auto const otherTrack = addPlayableTrack(runtime, "Other");
      auto const viewId = createView(runtime);
      REQUIRE(runtime.playbackSequence().playFromView(viewId, cursorTrack));
      REQUIRE(runtime.playback().playTrack(otherTrack, ListId{777}));
      auto const saved = runtime.savePlaybackSession();
      REQUIRE_FALSE(saved);
      CHECK(saved.error().code == Error::Code::InvalidState);
    }

    SECTION("flush failure remains dirty for late subscribers")
    {
      auto tempDir = ao::test::TempDir{};
      REQUIRE(std::filesystem::create_directory(tempDir.path() / "workspace.yaml"));
      auto runtime = makeRuntime(tempDir);
      addReadyAudioProvider(runtime.playback());
      auto const track = addPlayableTrack(runtime, "Track");
      auto const viewId = createView(runtime);
      REQUIRE(runtime.playbackSequence().playFromView(viewId, track));
      auto const saved = runtime.savePlaybackSession();
      REQUIRE_FALSE(saved);
      CHECK(saved.error().code == Error::Code::IoError);
      std::uint32_t replayCount = 0;
      auto sub = runtime.onPlaybackSessionDirty([&] { ++replayCount; });
      CHECK(replayCount == 1);
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
