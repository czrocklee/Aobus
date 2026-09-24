// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include "lib/audio/NullBackend.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/ApplicationPlaybackTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Quality.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Transport.h>
#include <ao/audio/flow/Graph.h>
#include <ao/query/Parser.h>
#include <ao/query/Serializer.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/playback/output/PlaybackOutputText.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  namespace
  {
    PlaybackTransport::PlaybackRequest playbackRequest(TrackId trackId, std::string title, std::string artist = {})
    {
      auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
      return PlaybackTransport::PlaybackRequest{
        .item = NowPlayingInfo{.trackId = trackId, .title = std::move(title), .artist = std::move(artist)},
        .input = audio::PlaybackInput{.filePath = fixturePath, .duration = std::chrono::seconds{1}},
      };
    }

    struct ControlledAudioState final
    {
      audio::BackendProvider::Status status;
      audio::BackendProvider::OnDevicesChangedCallback devicesChanged{};
      audio::BackendProvider::OnGraphChangedCallback graphChanged{};
      std::string graphRouteAnchor{};

      void renameDevice(std::string displayName)
      {
        status.devices.front().displayName = std::move(displayName);

        if (devicesChanged)
        {
          devicesChanged(status.devices);
        }
      }

      void publishGraph(audio::flow::Graph const& graph) const
      {
        if (graphChanged)
        {
          graphChanged(graph);
        }
      }
    };

    class ControlledAudioBackend final : public audio::NullBackend
    {
    public:
      ControlledAudioBackend(audio::BackendId backendId, audio::ProfileId profileId)
        : _backendId{std::move(backendId)}, _profileId{std::move(profileId)}
      {
      }

      Result<audio::OpenedPcmMode> open(audio::SignalFormat const& sourceFormat, audio::RenderTarget& target) override
      {
        auto openedRes = audio::NullBackend::open(sourceFormat, target);

        if (openedRes)
        {
          target.handleRouteReady("controlled-route");
        }

        return openedRes;
      }

      audio::BackendId backendId() const override { return _backendId; }
      audio::ProfileId profileId() const override { return _profileId; }

    private:
      audio::BackendId _backendId;
      audio::ProfileId _profileId;
    };

    class ControlledAudioProvider final : public audio::BackendProvider
    {
    public:
      explicit ControlledAudioProvider(std::shared_ptr<ControlledAudioState> statePtr)
        : _statePtr{std::move(statePtr)}
      {
      }

      void shutdown() noexcept override
      {
        _statePtr->devicesChanged = {};
        _statePtr->graphChanged = {};
      }

      utility::ScopedRegistration subscribeDevices(OnDevicesChangedCallback callback) override
      {
        _statePtr->devicesChanged = std::move(callback);

        if (_statePtr->devicesChanged)
        {
          _statePtr->devicesChanged(_statePtr->status.devices);
        }

        return utility::ScopedRegistration{[statePtr = _statePtr] { statePtr->devicesChanged = {}; }};
      }

      Status status() const override { return _statePtr->status; }

      std::unique_ptr<audio::Backend> createBackend(audio::Device const& device,
                                                    audio::ProfileId const& profile) override
      {
        return std::make_unique<ControlledAudioBackend>(device.backendId, profile);
      }

      utility::ScopedRegistration subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override
      {
        _statePtr->graphRouteAnchor = routeAnchor;
        _statePtr->graphChanged = std::move(callback);
        return utility::ScopedRegistration{[statePtr = _statePtr] { statePtr->graphChanged = {}; }};
      }

    private:
      std::shared_ptr<ControlledAudioState> _statePtr;
    };

    std::shared_ptr<ControlledAudioState> addControlledAudioProvider(
      ApplicationPlaybackFixtureT<QueuedExecutor>& fixture)
    {
      auto statePtr = std::make_shared<ControlledAudioState>(ControlledAudioState{
        .status =
          {
            .descriptor =
              {
                .id = audio::BackendId{"controlled"},
                .supportedProfiles = {{.id = audio::kProfileShared}},
              },
            .devices =
              {
                audio::Device{.id = audio::DeviceId{"controlled-device"},
                              .displayName = "Controlled DAC",
                              .description = "Controlled output",
                              .isDefault = true,
                              .backendId = audio::BackendId{"controlled"}},
              },
          },
      });
      fixture.playbackBootstrap.addProvider(std::make_unique<ControlledAudioProvider>(statePtr));
      return statePtr;
    }
  } // namespace

  TEST_CASE("NowPlayingViewModel - projects idle and track metadata", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto& playback = fixture.playback;
    auto& playbackTransport = fixture.playbackTransport;
    fixture.addReadyProvider();

    auto log = ao::test::RenderLog<NowPlayingViewState>{};
    auto const viewModel =
      NowPlayingViewModel{playback, ao::test::englishMessageCatalog(), [&log](auto const& view) { log.render(view); }};

    SECTION("Initial render when idle")
    {
      REQUIRE(!log.empty());
      CHECK(log.last().isActive == false);
      CHECK(log.last().title == "Not Playing");
    }

    SECTION("Metadata formatting for a track absent from the library")
    {
      auto desc = playbackRequest(TrackId{1}, "Song", "Artist");
      desc.item.coverArtId = ResourceId{42};
      desc.item.album = "Album";

      REQUIRE(playbackTransport.play(desc, ListId{1}));
      REQUIRE(!log.empty());
      // A missing library track cannot retain its captured cover.
      CHECK(log.last().coverArtId == kInvalidResourceId);
      CHECK(log.last().title == "Song");
      CHECK(log.last().artist == "Artist");
      CHECK(log.last().coverArtPlaceholderIdentity.primaryText == "Album");
      CHECK(log.last().combinedStatus == "Artist - Song");
    }

    SECTION("A request without library identity retains its supplied cover")
    {
      auto desc = playbackRequest(kInvalidTrackId, "External source");
      desc.item.coverArtId = ResourceId{42};
      REQUIRE(playbackTransport.play(desc, kInvalidListId));
      CHECK(log.last().coverArtId == ResourceId{42});
      CHECK(log.last().title == "External source");
      CHECK(log.last().isActive);
    }

    SECTION("Metadata with empty artist shows Unknown Artist")
    {
      auto desc = playbackRequest(TrackId{1}, "Instrumental");
      REQUIRE(playbackTransport.play(desc, ListId{1}));
      CHECK(log.last().title == "Instrumental");
      CHECK(log.last().artist == "Unknown Artist");
      CHECK(log.last().combinedStatus == "Instrumental");
    }
  }

  TEST_CASE("NowPlayingViewModel - fieldText exposes supported metadata fields", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto& playbackTransport = fixture.playbackTransport;
    fixture.addReadyProvider();

    auto log = ao::test::RenderLog<NowPlayingViewState>{};
    auto const viewModel = NowPlayingViewModel{
      fixture.playback, ao::test::englishMessageCatalog(), [&log](auto const& view) { log.render(view); }};

    SECTION("idle title uses the not-playing presentation")
    {
      REQUIRE_FALSE(log.empty());
      CHECK(NowPlayingViewModel::fieldText(log.last(), rt::TrackField::Title) == "Not Playing");
    }

    SECTION("title and artist use the current metadata")
    {
      auto desc = playbackRequest(TrackId{1}, "Song", "Artist");
      REQUIRE(playbackTransport.play(desc, ListId{1}));
      CHECK(NowPlayingViewModel::fieldText(log.last(), rt::TrackField::Title) == "Song");
      CHECK(NowPlayingViewModel::fieldText(log.last(), rt::TrackField::Artist) == "Artist");
    }

    SECTION("fieldText returns empty for unrelated field")
    {
      auto desc = playbackRequest(TrackId{1}, "Song");
      REQUIRE(playbackTransport.play(desc, ListId{1}));
      CHECK(NowPlayingViewModel::fieldText(log.last(), rt::TrackField::Year).empty());
    }
  }

  TEST_CASE("NowPlayingViewModel - resolves field actions", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto& playback = fixture.playback;
    auto& playbackTransport = fixture.playbackTransport;
    fixture.addReadyProvider();

    auto log = ao::test::RenderLog<NowPlayingViewState>{};
    auto const viewModel =
      NowPlayingViewModel{playback, ao::test::englishMessageCatalog(), [&log](auto const& view) { log.render(view); }};

    SECTION("Reveal action")
    {
      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::Reveal, rt::TrackField::Title);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Reveal);
    }

    SECTION("PlayPause action resolves to resume when idle")
    {
      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::PlayPause, rt::TrackField::Title);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Resume);
    }

    SECTION("PlayPause action resolves to pause while playing")
    {
      auto desc = playbackRequest(TrackId{1}, "Song");
      REQUIRE(playbackTransport.play(desc, ListId{1}));

      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::PlayPause, rt::TrackField::Title);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Pause);
    }

    SECTION("PlayPause action resolves to resume while paused")
    {
      auto desc = playbackRequest(TrackId{1}, "Song");
      REQUIRE(playbackTransport.play(desc, ListId{1}));
      playbackTransport.pause();
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Paused);

      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::PlayPause, rt::TrackField::Title);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Resume);
    }

    SECTION("FilterByField with Title")
    {
      auto desc = playbackRequest(TrackId{1}, "Song");
      REQUIRE(playbackTransport.play(desc, ListId{1}));

      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Title);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Navigate);
      CHECK(cmd.navigateQuery == "$title = \"Song\"");
    }

    SECTION("FilterByField with empty artist produces no action")
    {
      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Artist);
      CHECK(cmd.type == NowPlayingActionCommand::Type::None);
    }

    SECTION("FilterByField preserves titles containing both quote types")
    {
      auto desc = playbackRequest(TrackId{1}, "A \"Song\" 'Name'");
      REQUIRE(playbackTransport.play(desc, ListId{1}));

      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Title);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Navigate);
      CHECK(cmd.navigateQuery == R"($title = "A \"Song\" 'Name'")");

      auto parsed = ao::test::requireValue(query::parse(cmd.navigateQuery));
      CHECK(query::serialize(parsed) == R"($title = "A \"Song\" 'Name'")");
    }

    SECTION("FilterByField with Artist")
    {
      auto desc = playbackRequest(TrackId{1}, "Song", "Artist");
      REQUIRE(playbackTransport.play(desc, ListId{1}));

      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Artist);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Navigate);
      CHECK(cmd.navigateQuery == "$artist = \"Artist\"");
    }

    SECTION("FilterByField escapes a double-quote title")
    {
      auto desc = playbackRequest(TrackId{1}, "A \"Song\"");
      REQUIRE(playbackTransport.play(desc, ListId{1}));

      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Title);
      CHECK(cmd.navigateQuery == R"($title = "A \"Song\"")");
    }
  }

  TEST_CASE("NowPlayingViewModel - reports connecting stream info when audio engine is not ready",
            "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixture{};

    auto log = ao::test::RenderLog<NowPlayingViewState>{};
    auto const viewModel = NowPlayingViewModel{
      fixture.playback, ao::test::englishMessageCatalog(), [&log](auto const& view) { log.render(view); }};

    REQUIRE(!log.empty());
    CHECK(log.last().streamInfo == "Connecting to audio engine...");

    log.clear();
    fixture.addReadyProvider();

    REQUIRE(fixture.playback.snapshot().transport.ready);
    REQUIRE_FALSE(log.empty());
    CHECK(log.last().title == "Not Playing");
    CHECK(log.last().streamInfo.empty());

    auto const readyState = fixture.playback.snapshot().transport;
    auto const selected = readyState.output.selectedDevice;
    log.clear();
    fixture.commands().setOutputDevice(
      selected.backendId, audio::DeviceId{"pending-readiness-device"}, selected.profileId);

    auto const pendingState = fixture.playback.snapshot().transport;
    REQUIRE_FALSE(pendingState.ready);
    CHECK(pendingState.nowPlaying == readyState.nowPlaying);
    CHECK(pendingState.output == readyState.output);
    CHECK(pendingState.quality == readyState.quality);
    REQUIRE_FALSE(log.empty());
    CHECK(log.last().title == "Not Playing");
    CHECK(log.last().streamInfo == "Connecting to audio engine...");

    log.clear();
    fixture.commands().setOutputDevice(selected.backendId, selected.deviceId, selected.profileId);

    auto const restoredState = fixture.playback.snapshot().transport;
    REQUIRE(restoredState.ready);
    CHECK(restoredState.nowPlaying == pendingState.nowPlaying);
    CHECK(restoredState.output == pendingState.output);
    CHECK(restoredState.quality == pendingState.quality);
    REQUIRE_FALSE(log.empty());
    CHECK(log.last().title == "Not Playing");
    CHECK(log.last().streamInfo.empty());
  }

  TEST_CASE("NowPlayingViewModel - localizes shared copy without changing track metadata",
            "[uimodel][unit][playback][localization]")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto& playback = fixture.playback;
    auto& playbackTransport = fixture.playbackTransport;
    fixture.addReadyProvider();

    auto log = ao::test::RenderLog<NowPlayingViewState>{};
    auto const catalog = ao::test::messageCatalog("de-DE");
    auto const viewModel = NowPlayingViewModel{playback, catalog, [&log](auto const& view) { log.render(view); }};

    REQUIRE(!log.empty());
    CHECK(log.last().title == "Keine Wiedergabe");

    REQUIRE(playbackTransport.play(playbackRequest(TrackId{1}, "誰か、海を。"), ListId{1}));
    CHECK(log.last().title == "誰か、海を。");
    CHECK(log.last().artist == "Unbekannter Interpret");
    CHECK(log.last().audioPipeline.plainTextFallback.starts_with("Audiokette:\n"));
  }

  TEST_CASE("NowPlayingViewModel - presents an unnamed system-default output through the catalog",
            "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto& playback = fixture.playback;
    auto& playbackTransport = fixture.playbackTransport;
    addReadyAudioProvider(
      playbackTransport,
      audio::BackendProvider::Status{
        .descriptor = {.id = audio::kBackendPipeWire, .supportedProfiles = {{.id = audio::kProfileShared}}},
        .devices = {{.id = audio::DeviceId{}, .isDefault = true, .backendId = audio::kBackendPipeWire}},
      });

    auto log = ao::test::RenderLog<NowPlayingViewState>{};
    auto const viewModel =
      NowPlayingViewModel{playback, ao::test::englishMessageCatalog(), [&log](auto const& view) { log.render(view); }};

    REQUIRE(playbackTransport.play(playbackRequest(TrackId{1}, "Song"), ListId{1}));
    REQUIRE(!log.empty());
    CHECK(log.last().audioPipeline.deviceName == "System Default");
    CHECK(log.last().audioPipeline.deviceIconKind == AudioIconKind::AudioServer);
    CHECK(log.last().audioPipeline.plainTextFallback.starts_with("Audio Pipeline:\n"));
  }

  TEST_CASE("NowPlayingViewModel - refreshes for output and quality dependencies", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixtureT<QueuedExecutor>{};
    auto& playback = fixture.playback;
    auto& playbackTransport = fixture.playbackTransport;
    auto const audioStatePtr = addControlledAudioProvider(fixture);
    REQUIRE(fixture.executor.tryDrainUntil([&playback] { return playback.snapshot().transport.ready; }));

    auto log = ao::test::RenderLog<NowPlayingViewState>{};
    auto const viewModel =
      NowPlayingViewModel{playback, ao::test::englishMessageCatalog(), [&log](auto const& view) { log.render(view); }};
    REQUIRE(playbackTransport.play(playbackRequest(TrackId{1}, "Dependency Song", "Dependency Artist"), ListId{1}));
    REQUIRE(
      fixture.executor.tryDrainUntil([&audioStatePtr] { return static_cast<bool>(audioStatePtr->graphChanged); }));
    REQUIRE(fixture.executor.tryDrainUntil(
      [&playback] { return playback.snapshot().transport.quality.overall == audio::Quality::BitwisePerfect; }));
    REQUIRE_FALSE(log.empty());

    SECTION("selected output presentation refreshes when only device inventory changes")
    {
      CHECK(log.last().audioPipeline.deviceName == "Controlled DAC");
      CHECK(log.last().audioPipeline.deviceIconKind == AudioIconKind::OutputDevice);
      auto const before = playback.snapshot().transport;
      log.clear();

      audioStatePtr->renameDevice("Renamed Controlled DAC");
      REQUIRE(fixture.executor.tryDrainUntil([&playback, &before]
                                             { return playback.snapshot().transport.output != before.output; }));

      auto const after = playback.snapshot().transport;
      CHECK(after.nowPlaying == before.nowPlaying);
      CHECK(after.quality == before.quality);
      CHECK(after.ready == before.ready);
      CHECK(after.output != before.output);
      REQUIRE_FALSE(log.empty());
      CHECK(log.last().audioPipeline.deviceName == "Renamed Controlled DAC");
      CHECK(log.last().audioPipeline.deviceIconKind == AudioIconKind::OutputDevice);
    }

    SECTION("quality presentation refreshes when only the provider graph changes")
    {
      REQUIRE(audioStatePtr->graphChanged);
      CHECK(audioStatePtr->graphRouteAnchor == "controlled-route");
      auto const before = playback.snapshot().transport;
      log.clear();
      auto const resampledFormat =
        audio::PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = audio::SampleEncoding::Signed16Le};

      audioStatePtr->publishGraph(audio::flow::Graph{
        .nodes =
          {
            audio::flow::Node{.id = "controlled-stream",
                              .type = audio::flow::NodeType::Stream,
                              .name = "Controlled Stream",
                              .optFormat = resampledFormat},
            audio::flow::Node{.id = "controlled-sink",
                              .type = audio::flow::NodeType::Sink,
                              .name = "Controlled DAC",
                              .optFormat = resampledFormat},
          },
        .connections =
          {
            audio::flow::Connection{
              .sourceId = "controlled-stream", .destinationId = "controlled-sink", .isActive = true},
          },
      });
      REQUIRE(fixture.executor.tryDrainUntil(
        [&playback]
        { return playback.snapshot().transport.quality.pipelineQuality == audio::Quality::LinearIntervention; }));

      auto const after = playback.snapshot().transport;
      CHECK(after.nowPlaying == before.nowPlaying);
      CHECK(after.output == before.output);
      CHECK(after.ready == before.ready);
      CHECK(after.quality != before.quality);
      CHECK(after.quality.sourceQuality == audio::Quality::BitwisePerfect);
      CHECK(after.quality.pipelineQuality == audio::Quality::LinearIntervention);
      CHECK(after.quality.overall == audio::Quality::LinearIntervention);
      REQUIRE_FALSE(log.empty());
      CHECK(log.last().audioPipeline.quality == after.quality);
      CHECK(log.last().qualityCategory == AudioQualityCategory::Diagnostic);
      CHECK(log.last().streamInfo == "44.1 kHz · 16-bit · Stereo");
      CHECK(log.last().audioPipeline.plainTextFallback.contains("Pipeline intervention"));
    }
  }

  TEST_CASE("NowPlayingViewModel - refreshes from playback events until destroyed", "[uimodel][unit][playback]")
  {
    auto fixture = ApplicationPlaybackFixture{};
    auto& playback = fixture.playback;
    auto& playbackTransport = fixture.playbackTransport;
    fixture.addReadyProvider();

    auto log = ao::test::RenderLog<NowPlayingViewState>{};
    auto viewModelPtr = std::make_unique<NowPlayingViewModel>(
      playback, ao::test::englishMessageCatalog(), [&log](auto const& view) { log.render(view); });

    REQUIRE(!log.empty());
    log.clear();

    fixture.commands().setShuffleMode(ShuffleMode::On);
    fixture.commands().setVolume(0.5F);
    CHECK(log.empty());

    auto const trackId =
      fixture.libraryFixture.addTrack({.title = "Event Song", .artist = "Event Artist", .album = "Event Album"});
    REQUIRE(playbackTransport.play(playbackRequest(trackId, "Event Song", "Event Artist"), ListId{1}));

    REQUIRE(!log.empty());
    CHECK(log.last().title == "Event Song");
    CHECK(log.last().combinedStatus == "Event Artist - Event Song");

    log.clear();
    viewModelPtr.reset();

    REQUIRE(playbackTransport.play(playbackRequest(trackId, "After Destroy", "Event Artist"), ListId{1}));
    CHECK(log.empty());
  }
} // namespace ao::uimodel::test
