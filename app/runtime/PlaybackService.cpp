// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Type.h>
#include <ao/audio/Backend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/Player.h>
#include <ao/audio/Types.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/ViewService.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    OutputProfileSnapshot toOutputProfileSnapshot(audio::IBackendProvider::ProfileMetadata const& src)
    {
      return OutputProfileSnapshot{.id = src.id, .name = src.name, .description = src.description};
    }

    OutputDeviceSnapshot toOutputDeviceSnapshot(audio::Device const& src)
    {
      return OutputDeviceSnapshot{.id = src.id,
                                  .displayName = src.displayName,
                                  .description = src.description,
                                  .isDefault = src.isDefault,
                                  .backendId = src.backendId,
                                  .capabilities = src.capabilities};
    }

    OutputBackendSnapshot toOutputBackendSnapshot(audio::IBackendProvider::Status const& src)
    {
      auto profiles = std::vector<OutputProfileSnapshot>{};
      profiles.reserve(src.metadata.supportedProfiles.size());

      for (auto const& prof : src.metadata.supportedProfiles)
      {
        profiles.push_back(toOutputProfileSnapshot(prof));
      }

      auto devices = std::vector<OutputDeviceSnapshot>{};
      devices.reserve(src.devices.size());

      for (auto const& dev : src.devices)
      {
        devices.push_back(toOutputDeviceSnapshot(dev));
      }

      return OutputBackendSnapshot{.id = src.metadata.id,
                                   .name = src.metadata.name,
                                   .description = src.metadata.description,
                                   .iconName = src.metadata.iconName,
                                   .supportedProfiles = std::move(profiles),
                                   .devices = std::move(devices)};
    }

    PlaybackState buildPlaybackState(audio::Player const& player)
    {
      auto const status = player.status();

      auto outputs = std::vector<OutputBackendSnapshot>{};
      outputs.reserve(status.availableBackends.size());

      for (auto const& backendStatus : status.availableBackends)
      {
        outputs.push_back(toOutputBackendSnapshot(backendStatus));
      }

      return PlaybackState{
        .transport = status.engine.transport,
        .trackId = {},
        .elapsed = status.engine.elapsed,
        .duration = status.engine.duration,
        .volume = status.volume,
        .muted = status.muted,
        .volumeAvailable = status.volumeAvailable,
        .volumeIsHardwareAssisted = status.volumeIsHardwareAssisted,
        .ready = status.isReady,
        .selectedOutput =
          OutputSelection{
            .backendId = status.engine.backendId,
            .deviceId = status.engine.currentDeviceId,
            .profileId = status.engine.profileId,
          },
        .availableOutputs = std::move(outputs),
        .flow = status.flow,
        .quality = status.quality,
        .qualityAssessments = status.qualityAssessments,
      };
    }

    std::optional<audio::TrackPlaybackDescriptor> playbackDescriptorForTrack(library::MusicLibrary& library,
                                                                             TrackId trackId)
    {
      auto const txn = library.readTransaction();
      auto reader = library.tracks().reader(txn);
      auto const optView = reader.get(trackId, library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        return std::nullopt;
      }

      auto const& view = *optView;
      auto const metadata = view.metadata();
      auto const property = view.property();
      auto const uri = std::filesystem::path{property.uri()};
      auto const optFilePath =
        uri.empty() ? std::optional<std::filesystem::path>{}
                    : std::optional<std::filesystem::path>{
                        uri.is_absolute() ? uri.lexically_normal() : (library.rootPath() / uri).lexically_normal()};

      auto descriptor = audio::TrackPlaybackDescriptor{
        .trackId = trackId,
        .title = std::string{metadata.title()},
        .artist = std::string{library.dictionary().getOrDefault(metadata.artistId())},
        .album = std::string{library.dictionary().getOrDefault(metadata.albumId())},
        .coverArtId = view.coverArt()
                        .primary()
                        .transform([](library::CoverArt cover) { return cover.resourceId; })
                        .value_or(kInvalidResourceId),
        .duration = property.duration(),
        .sampleRateHint = property.sampleRate().raw(),
        .channelsHint = property.channels().raw(),
        .bitDepthHint = property.bitDepth().raw(),
      };

      if (optFilePath)
      {
        descriptor.filePath = *optFilePath;
      }

      return descriptor;
    }
  }

  struct PlaybackService::Impl final
  {
    async::IExecutor& executor;
    PlaybackState state;
    std::unique_ptr<audio::Player> playerPtr;
    ViewService& views;
    library::MusicLibrary& library;
    TrackId currentTrackId = kInvalidTrackId;
    ListId currentSourceListId = kInvalidListId;
    ShuffleMode shuffleMode = ShuffleMode::Off;
    RepeatMode repeatMode = RepeatMode::Off;
    std::string currentTrackTitle{};
    std::string currentTrackArtist{};
    std::chrono::milliseconds currentTrackDuration{0};
    Signal<> preparingSignal;
    Signal<> startedSignal;
    Signal<> pausedSignal;
    Signal<> idleSignal;
    Signal<PlaybackService::NowPlayingChanged const&> nowPlayingChangedSignal;
    Signal<OutputSelection const&> outputChangedSignal;
    Signal<> stoppedSignal;
    Signal<> devicesChangedSignal;
    Signal<PlaybackService::QualityChanged const&> qualityChangedSignal;
    Signal<float> volumeChangedSignal;
    Signal<bool> mutedChangedSignal;
    Signal<PlaybackService::RevealTrackRequested const&> revealTrackRequestedSignal;
    Signal<PlaybackService::SeekUpdate const&> seekUpdateSignal;
    Signal<PlaybackService::ShuffleModeChanged const&> shuffleModeChangedSignal;
    Signal<PlaybackService::RepeatModeChanged const&> repeatModeChangedSignal;

    void ensureReady() const
    {
      if (playerPtr->isReady())
      {
        return;
      }

      auto const status = playerPtr->status();

      if (status.availableBackends.empty())
      {
        return;
      }

      auto const& backend = status.availableBackends.front();

      if (backend.devices.empty())
      {
        return;
      }

      auto const& device = backend.devices.front();
      auto profileId = audio::ProfileId{audio::kProfileShared};

      if (!backend.metadata.supportedProfiles.empty())
      {
        profileId = backend.metadata.supportedProfiles.front().id;
      }

      playerPtr->setOutput(backend.metadata.id, device.id, profileId);
    }

    PlaybackState buildState(audio::Player const& targetPlayer) const
    {
      auto snapshot = buildPlaybackState(targetPlayer);
      snapshot.trackId = currentTrackId;
      snapshot.sourceListId = currentSourceListId;
      snapshot.trackTitle = currentTrackTitle;
      snapshot.trackArtist = currentTrackArtist;
      snapshot.shuffleMode = shuffleMode;
      snapshot.repeatMode = repeatMode;

      if (snapshot.duration == std::chrono::milliseconds{0})
      {
        snapshot.duration = currentTrackDuration;
      }

      return snapshot;
    }

    explicit Impl(async::IExecutor& callbackExecutor, ViewService& viewService, library::MusicLibrary& musicLibrary)
      : executor{callbackExecutor}
      , playerPtr{std::make_unique<audio::Player>()}
      , views{viewService}
      , library{musicLibrary}
    {
      playerPtr->setOnTrackEnded(
        [this]
        {
          executor.dispatch(
            [this]
            {
              state = buildState(*playerPtr);
              idleSignal.emit();
            });
        });

      playerPtr->setOnDevicesChanged(
        [this](std::vector<audio::IBackendProvider::Status> const&)
        {
          executor.dispatch(
            [this]
            {
              state = buildState(*playerPtr);

              // Auto-select first available default output if none is selected yet
              if (!state.selectedOutput.backendId.empty() || state.availableOutputs.empty())
              {
                devicesChangedSignal.emit();
                return;
              }

              auto const& backend = state.availableOutputs.front();

              if (backend.devices.empty())
              {
                devicesChangedSignal.emit();
                return;
              }

              auto const& device = backend.devices.front();
              auto profileId = audio::ProfileId{audio::kProfileShared};

              if (!backend.supportedProfiles.empty())
              {
                profileId = backend.supportedProfiles.front().id;
              }

              playerPtr->setOutput(backend.id, device.id, profileId);
              state = buildState(*playerPtr);
              outputChangedSignal.emit(state.selectedOutput);
            });
        });

      playerPtr->setOnQualityChanged(
        [this](audio::Quality, bool)
        {
          executor.dispatch(
            [this]
            {
              state = buildState(*playerPtr);
              qualityChangedSignal.emit(
                PlaybackService::QualityChanged{.quality = state.quality, .ready = state.ready});
            });
        });
    }
  };

  PlaybackService::PlaybackService(async::IExecutor& executor, ViewService& views, library::MusicLibrary& library)
    : _implPtr{std::make_unique<Impl>(executor, views, library)}
  {
  }

  PlaybackService::~PlaybackService() = default;

  Subscription PlaybackService::onPreparing(std::move_only_function<void()> handler)
  {
    return _implPtr->preparingSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onStarted(std::move_only_function<void()> handler)
  {
    return _implPtr->startedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onPaused(std::move_only_function<void()> handler)
  {
    return _implPtr->pausedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onIdle(std::move_only_function<void()> handler)
  {
    return _implPtr->idleSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onNowPlayingChanged(std::move_only_function<void(NowPlayingChanged const&)> handler)
  {
    return _implPtr->nowPlayingChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onOutputChanged(std::move_only_function<void(OutputSelection const&)> handler)
  {
    return _implPtr->outputChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onStopped(std::move_only_function<void()> handler)
  {
    return _implPtr->stoppedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onDevicesChanged(std::move_only_function<void()> handler)
  {
    return _implPtr->devicesChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onQualityChanged(std::move_only_function<void(QualityChanged const&)> handler)
  {
    return _implPtr->qualityChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onVolumeChanged(std::move_only_function<void(float)> handler)
  {
    return _implPtr->volumeChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onMutedChanged(std::move_only_function<void(bool)> handler)
  {
    return _implPtr->mutedChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onRevealTrackRequested(
    std::move_only_function<void(RevealTrackRequested const&)> handler)
  {
    return _implPtr->revealTrackRequestedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onSeekUpdate(std::move_only_function<void(SeekUpdate const&)> handler)
  {
    return _implPtr->seekUpdateSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onShuffleModeChanged(std::move_only_function<void(ShuffleModeChanged const&)> handler)
  {
    return _implPtr->shuffleModeChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onRepeatModeChanged(std::move_only_function<void(RepeatModeChanged const&)> handler)
  {
    return _implPtr->repeatModeChangedSignal.connect(std::move(handler));
  }

  PlaybackState const& PlaybackService::state() const
  {
    return _implPtr->state;
  }

  void PlaybackService::play(audio::TrackPlaybackDescriptor const& descriptor, ListId const sourceListId)
  {
    auto& impl = *_implPtr;
    impl.ensureReady();

    // Signal "about to play" so the UI resets the seekbar before the
    // blocking Engine::play call freezes the main thread.
    impl.preparingSignal.emit();

    impl.playerPtr->play(descriptor);
    impl.currentTrackId = descriptor.trackId;
    impl.currentSourceListId = sourceListId;
    impl.currentTrackTitle = descriptor.title;
    impl.currentTrackArtist = descriptor.artist;
    impl.currentTrackDuration = descriptor.duration;
    impl.state = impl.buildState(*impl.playerPtr);
    impl.startedSignal.emit();

    impl.nowPlayingChangedSignal.emit(PlaybackService::NowPlayingChanged{
      .trackId = descriptor.trackId,
      .sourceListId = sourceListId,
    });
  }

  bool PlaybackService::playTrack(TrackId const trackId, ListId const sourceListId)
  {
    try
    {
      auto const optDescriptor = playbackDescriptorForTrack(_implPtr->library, trackId);

      if (!optDescriptor)
      {
        return false;
      }

      play(*optDescriptor, sourceListId);
      return true;
    }
    catch (std::exception const&)
    {
      return false;
    }
  }

  TrackId PlaybackService::playSelectionInView(ViewId const viewId)
  {
    try
    {
      auto const state = _implPtr->views.trackListState(viewId);
      auto const sel = state.selection;

      if (sel.empty())
      {
        return kInvalidTrackId;
      }

      auto const trackId = TrackId{sel.front()};
      return playTrack(trackId, state.listId) ? trackId : kInvalidTrackId;
    }
    catch (std::exception const&)
    {
      return kInvalidTrackId;
    }
  }

  void PlaybackService::pause()
  {
    _implPtr->playerPtr->pause();
    _implPtr->state = _implPtr->buildState(*_implPtr->playerPtr);
    _implPtr->pausedSignal.emit();
  }

  void PlaybackService::resume()
  {
    _implPtr->playerPtr->resume();
    _implPtr->state = _implPtr->buildState(*_implPtr->playerPtr);
    _implPtr->startedSignal.emit();
  }

  void PlaybackService::stop()
  {
    _implPtr->playerPtr->stop();
    _implPtr->currentTrackId = {};
    _implPtr->currentSourceListId = {};
    _implPtr->currentTrackTitle.clear();
    _implPtr->currentTrackArtist.clear();
    _implPtr->currentTrackDuration = std::chrono::milliseconds{0};
    _implPtr->state = _implPtr->buildState(*_implPtr->playerPtr);
    _implPtr->stoppedSignal.emit();
    _implPtr->idleSignal.emit();
    _implPtr->nowPlayingChangedSignal.emit(PlaybackService::NowPlayingChanged{
      .trackId = kInvalidTrackId,
      .sourceListId = kInvalidListId,
    });
  }

  void PlaybackService::setShuffleMode(ShuffleMode const mode)
  {
    _implPtr->shuffleMode = mode;
    _implPtr->state = _implPtr->buildState(*_implPtr->playerPtr);
    _implPtr->shuffleModeChangedSignal.emit(ShuffleModeChanged{.mode = mode});
  }

  void PlaybackService::setRepeatMode(RepeatMode const mode)
  {
    _implPtr->repeatMode = mode;
    _implPtr->state = _implPtr->buildState(*_implPtr->playerPtr);
    _implPtr->repeatModeChangedSignal.emit(RepeatModeChanged{.mode = mode});
  }

  void PlaybackService::seek(std::chrono::milliseconds const elapsed, SeekMode const mode)
  {
    if (mode == SeekMode::Final)
    {
      _implPtr->playerPtr->seek(elapsed);
      _implPtr->state = _implPtr->buildState(*_implPtr->playerPtr);
    }

    _implPtr->seekUpdateSignal.emit(SeekUpdate{.elapsed = elapsed, .mode = mode});
  }

  void PlaybackService::setOutput(audio::BackendId const& backendId,
                                  audio::DeviceId const& deviceId,
                                  audio::ProfileId const& profileId)
  {
    _implPtr->playerPtr->setOutput(backendId, deviceId, profileId);
    _implPtr->state = _implPtr->buildState(*_implPtr->playerPtr);
    _implPtr->outputChangedSignal.emit(OutputSelection{
      .backendId = backendId,
      .deviceId = deviceId,
      .profileId = profileId,
    });
  }

  void PlaybackService::setVolume(float const volume)
  {
    _implPtr->playerPtr->setVolume(volume);
    _implPtr->state = _implPtr->buildState(*_implPtr->playerPtr);
    _implPtr->volumeChangedSignal.emit(volume);
  }

  void PlaybackService::setMuted(bool const muted)
  {
    _implPtr->playerPtr->setMuted(muted);
    _implPtr->state = _implPtr->buildState(*_implPtr->playerPtr);
    _implPtr->mutedChangedSignal.emit(_implPtr->state.muted);
  }

  void PlaybackService::revealPlayingTrack()
  {
    revealTrack(_implPtr->state.trackId, kInvalidViewId, _implPtr->state.sourceListId);
  }

  void PlaybackService::revealTrack(TrackId const trackId, ViewId const preferredViewId, ListId const preferredListId)
  {
    _implPtr->revealTrackRequestedSignal.emit(PlaybackService::RevealTrackRequested{
      .trackId = trackId, .preferredListId = preferredListId, .preferredViewId = preferredViewId});
  }

  void PlaybackService::addProvider(std::unique_ptr<audio::IBackendProvider> providerPtr)
  {
    _implPtr->playerPtr->addProvider(std::move(providerPtr));
  }
} // namespace ao::rt
