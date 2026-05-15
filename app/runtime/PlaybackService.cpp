// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackService.h"

#include "ViewService.h"

#include <ao/Type.h>
#include <ao/audio/Backend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/Player.h>
#include <ao/audio/Types.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <runtime/CorePrimitives.h>
#include <runtime/StateTypes.h>

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cstdint>

namespace ao::rt
{
  namespace
  {
    PlaybackState buildPlaybackState(audio::Player const& player)
    {
      auto const status = player.status();

      auto outputs = std::vector<OutputBackendSnapshot>{};
      outputs.reserve(status.availableBackends.size());

      for (auto const& backendStatus : status.availableBackends)
      {
        auto devices = std::vector<OutputDeviceSnapshot>{};
        devices.reserve(backendStatus.devices.size());

        for (auto const& dev : backendStatus.devices)
        {
          devices.push_back(OutputDeviceSnapshot{
            .id = dev.id,
            .displayName = dev.displayName,
            .description = dev.description,
            .isDefault = dev.isDefault,
            .backendId = dev.backendId,
            .capabilities = dev.capabilities,
          });
        }

        auto profiles = std::vector<OutputProfileSnapshot>{};
        profiles.reserve(backendStatus.metadata.supportedProfiles.size());

        for (auto const& prof : backendStatus.metadata.supportedProfiles)
        {
          profiles.push_back(OutputProfileSnapshot{
            .id = prof.id,
            .name = prof.name,
            .description = prof.description,
          });
        }

        outputs.push_back(OutputBackendSnapshot{
          .id = backendStatus.metadata.id,
          .name = backendStatus.metadata.name,
          .description = backendStatus.metadata.description,
          .iconName = backendStatus.metadata.iconName,
          .supportedProfiles = std::move(profiles),
          .devices = std::move(devices),
        });
      }

      return PlaybackState{
        .transport = status.engine.transport,
        .trackId = {},
        .positionMs = status.engine.positionMs,
        .durationMs = status.engine.durationMs,
        .volume = status.volume,
        .muted = status.muted,
        .volumeAvailable = status.volumeAvailable,
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
      };
    }
  }

  struct PlaybackService::Impl final
  {
    IControlExecutor& executor;
    PlaybackState state;
    std::unique_ptr<audio::Player> player;
    ViewService& views;
    library::MusicLibrary& library;
    TrackId currentTrackId{};
    ListId currentSourceListId{};
    std::string currentTrackTitle{};
    std::string currentTrackArtist{};
    Signal<> preparingSignal;
    Signal<> startedSignal;
    Signal<> pausedSignal;
    Signal<> idleSignal;
    Signal<PlaybackService::NowPlayingChanged const&> nowPlayingChangedSignal;
    Signal<OutputSelection const&> outputChangedSignal;
    Signal<> stoppedSignal;
    Signal<> devicesChangedSignal;
    Signal<PlaybackService::QualityChanged const&> qualityChangedSignal;
    Signal<PlaybackService::RevealTrackRequested const&> revealTrackRequestedSignal;

    void ensureReady() const
    {
      if (player->isReady())
      {
        return;
      }

      auto const status = player->status();

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

      player->setOutput(backend.metadata.id, device.id, profileId);
    }

    PlaybackState buildState(audio::Player const& targetPlayer) const
    {
      auto snapshot = buildPlaybackState(targetPlayer);
      snapshot.trackId = currentTrackId;
      snapshot.sourceListId = currentSourceListId;
      snapshot.trackTitle = currentTrackTitle;
      snapshot.trackArtist = currentTrackArtist;
      return snapshot;
    }

    explicit Impl(IControlExecutor& controlExecutor, ViewService& viewService, library::MusicLibrary& musicLibrary)
      : executor{controlExecutor}, player{std::make_unique<audio::Player>()}, views{viewService}, library{musicLibrary}
    {
      player->setOnTrackEnded(
        [this]
        {
          executor.dispatch(
            [this]
            {
              state = buildState(*player);
              idleSignal.emit();
            });
        });

      player->setOnDevicesChanged(
        [this](std::vector<audio::IBackendProvider::Status> const&)
        {
          executor.dispatch(
            [this]
            {
              state = buildState(*player);

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

              player->setOutput(backend.id, device.id, profileId);
              state = buildState(*player);
              outputChangedSignal.emit(state.selectedOutput);
            });
        });

      player->setOnQualityChanged(
        [this](audio::Quality quality, bool const ready)
        {
          executor.dispatch(
            [this, quality, ready]
            {
              state.quality = quality;
              state.ready = ready;
              qualityChangedSignal.emit(PlaybackService::QualityChanged{.quality = quality, .ready = ready});
            });
        });
    }
  };

  PlaybackService::PlaybackService(IControlExecutor& executor, ViewService& views, library::MusicLibrary& library)
    : _impl{std::make_unique<Impl>(executor, views, library)}
  {
  }

  PlaybackService::~PlaybackService() = default;

  Subscription PlaybackService::onPreparing(std::move_only_function<void()> handler)
  {
    return _impl->preparingSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onStarted(std::move_only_function<void()> handler)
  {
    return _impl->startedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onPaused(std::move_only_function<void()> handler)
  {
    return _impl->pausedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onIdle(std::move_only_function<void()> handler)
  {
    return _impl->idleSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onNowPlayingChanged(std::move_only_function<void(NowPlayingChanged const&)> handler)
  {
    return _impl->nowPlayingChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onOutputChanged(std::move_only_function<void(OutputSelection const&)> handler)
  {
    return _impl->outputChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onStopped(std::move_only_function<void()> handler)
  {
    return _impl->stoppedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onDevicesChanged(std::move_only_function<void()> handler)
  {
    return _impl->devicesChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onQualityChanged(std::move_only_function<void(QualityChanged const&)> handler)
  {
    return _impl->qualityChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onRevealTrackRequested(
    std::move_only_function<void(RevealTrackRequested const&)> handler)
  {
    return _impl->revealTrackRequestedSignal.connect(std::move(handler));
  }

  PlaybackState const& PlaybackService::state() const
  {
    return _impl->state;
  }

  void PlaybackService::play(audio::TrackPlaybackDescriptor const& descriptor, ListId const sourceListId)
  {
    _impl->ensureReady();

    // Signal "about to play" so the UI resets the seekbar before the
    // blocking Engine::play call freezes the main thread.
    _impl->preparingSignal.emit();

    _impl->player->play(descriptor);
    _impl->currentTrackId = descriptor.trackId;
    _impl->currentSourceListId = sourceListId;
    _impl->currentTrackTitle = descriptor.title;
    _impl->currentTrackArtist = descriptor.artist;
    _impl->state = _impl->buildState(*_impl->player);
    _impl->startedSignal.emit();

    _impl->nowPlayingChangedSignal.emit(PlaybackService::NowPlayingChanged{
      .trackId = descriptor.trackId,
      .sourceListId = sourceListId,
    });
  }

  TrackId PlaybackService::playSelectionInView(ViewId const viewId)
  {
    try
    {
      auto const state = _impl->views.trackListState(viewId);
      auto const sel = state.selection;

      if (sel.empty())
      {
        return TrackId{};
      }

      auto const trackId = TrackId{sel.front()};
      auto const txn = _impl->library.readTransaction();
      auto reader = _impl->library.tracks().reader(txn);
      auto const optView = reader.get(trackId, library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        return TrackId{};
      }

      auto const uri = std::filesystem::path{optView->property().uri()};
      auto const filePath =
        uri.is_absolute() ? uri.lexically_normal() : (_impl->library.rootPath() / uri).lexically_normal();

      auto const desc = audio::TrackPlaybackDescriptor{
        .trackId = trackId,
        .filePath = filePath,
        .title = std::string{optView->metadata().title()},
        .durationMs = optView->coldHeader().durationMs,
      };

      play(desc, state.listId);

      return trackId;
    }
    catch (std::exception const&)
    {
      return TrackId{};
    }
  }

  void PlaybackService::pause()
  {
    _impl->player->pause();
    _impl->state = _impl->buildState(*_impl->player);
    _impl->pausedSignal.emit();
  }

  void PlaybackService::resume()
  {
    _impl->player->resume();
    _impl->state = _impl->buildState(*_impl->player);
    _impl->startedSignal.emit();
  }

  void PlaybackService::stop()
  {
    _impl->player->stop();
    _impl->currentTrackId = {};
    _impl->currentSourceListId = {};
    _impl->currentTrackTitle.clear();
    _impl->currentTrackArtist.clear();
    _impl->state = _impl->buildState(*_impl->player);
    _impl->stoppedSignal.emit();
    _impl->idleSignal.emit();
  }

  void PlaybackService::seek(std::uint32_t const positionMs)
  {
    _impl->player->seek(positionMs);
    _impl->state = _impl->buildState(*_impl->player);
  }

  void PlaybackService::setOutput(audio::BackendId const& backendId,
                                  audio::DeviceId const& deviceId,
                                  audio::ProfileId const& profileId)
  {
    _impl->player->setOutput(backendId, deviceId, profileId);
    _impl->state = _impl->buildState(*_impl->player);
    _impl->outputChangedSignal.emit(OutputSelection{
      .backendId = backendId,
      .deviceId = deviceId,
      .profileId = profileId,
    });
  }

  void PlaybackService::setVolume(float const volume)
  {
    _impl->player->setVolume(volume);
    _impl->state = _impl->buildState(*_impl->player);
  }

  void PlaybackService::setMuted(bool const muted)
  {
    _impl->player->setMuted(muted);
    _impl->state = _impl->buildState(*_impl->player);
  }

  void PlaybackService::revealPlayingTrack()
  {
    _impl->revealTrackRequestedSignal.emit(PlaybackService::RevealTrackRequested{
      .trackId = _impl->state.trackId,
      .preferredListId = _impl->state.sourceListId,
    });
  }

  void PlaybackService::addProvider(std::unique_ptr<audio::IBackendProvider> provider)
  {
    _impl->player->addProvider(std::move(provider));
  }
}
