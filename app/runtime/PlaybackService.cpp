// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaybackService.h"
#include "EventBus.h"
#include "EventTypes.h"
#include "ViewService.h"

#include <ao/audio/Player.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/utility/ThreadUtils.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace ao::app
{
  namespace
  {
    PlaybackState buildPlaybackState(ao::audio::Player const& player)
    {
      auto status = player.status();

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
    EventBus& events;
    PlaybackState state;
    std::unique_ptr<ao::audio::Player> player;
    ViewService& views;
    ao::library::MusicLibrary& library;
    ao::TrackId currentTrackId{};
    ao::ListId currentSourceListId{};
    std::string currentTrackTitle{};
    std::string currentTrackArtist{};

    void ensureReady() const
    {
      if (player->isReady())
      {
        return;
      }

      auto status = player->status();

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
      auto profileId = ao::audio::kProfileShared;

      if (!backend.metadata.supportedProfiles.empty())
      {
        profileId = backend.metadata.supportedProfiles.front().id;
      }

      player->setOutput(backend.metadata.id, device.id, profileId);
    }

    PlaybackState buildState(ao::audio::Player const& targetPlayer) const
    {
      auto snapshot = buildPlaybackState(targetPlayer);
      snapshot.trackId = currentTrackId;
      snapshot.sourceListId = currentSourceListId;
      snapshot.trackTitle = currentTrackTitle;
      snapshot.trackArtist = currentTrackArtist;
      return snapshot;
    }

    explicit Impl(IControlExecutor& controlExecutor,
                  EventBus& eventBus,
                  ViewService& viewService,
                  ao::library::MusicLibrary& musicLibrary)
      : executor{controlExecutor}
      , events{eventBus}
      , player{std::make_unique<ao::audio::Player>()}
      , views{viewService}
      , library{musicLibrary}
    {
      player->setOnTrackEnded(
        [this]
        {
          executor.dispatch(
            [this]
            {
              state = buildState(*player);
              events.publish(PlaybackTransportChanged{.transport = ao::audio::Transport::Idle});
            });
        });

      player->setOnDevicesChanged(
        [this](std::vector<ao::audio::IBackendProvider::Status> const&)
        {
          executor.dispatch(
            [this]
            {
              state = buildState(*player);

              // Auto-select first available default output if none is selected yet
              if (!state.selectedOutput.backendId.empty() || state.availableOutputs.empty())
              {
                events.publish(PlaybackDevicesChanged{});
                return;
              }

              auto const& backend = state.availableOutputs.front();
              if (backend.devices.empty())
              {
                events.publish(PlaybackDevicesChanged{});
                return;
              }

              auto const& device = backend.devices.front();
              auto profileId = ao::audio::kProfileShared;

              if (!backend.supportedProfiles.empty())
              {
                profileId = backend.supportedProfiles.front().id;
              }

              player->setOutput(backend.id, device.id, profileId);
              state = buildState(*player);

              events.publish(PlaybackDevicesChanged{});
            });
        });

      player->setOnQualityChanged(
        [this](ao::audio::Quality quality, bool ready)
        {
          executor.dispatch(
            [this, quality, ready]
            {
              state.quality = quality;
              state.ready = ready;
              events.publish(PlaybackQualityChanged{.quality = quality, .ready = ready});
            });
        });
    }
  };

  PlaybackService::PlaybackService(EventBus& events,
                                   IControlExecutor& executor,
                                   ViewService& views,
                                   ao::library::MusicLibrary& library)
    : _impl{std::make_unique<Impl>(executor, events, views, library)}
  {
  }

  PlaybackService::~PlaybackService() = default;

  PlaybackState PlaybackService::state() const
  {
    return _impl->state;
  }

  void PlaybackService::play(ao::audio::TrackPlaybackDescriptor const& descriptor, ao::ListId sourceListId)
  {
    _impl->ensureReady();
    _impl->player->play(descriptor);
    _impl->currentTrackId = descriptor.trackId;
    _impl->currentSourceListId = sourceListId;
    _impl->currentTrackTitle = descriptor.title;
    _impl->currentTrackArtist = descriptor.artist;
    _impl->state = _impl->buildState(*_impl->player);
    _impl->events.publish(PlaybackTransportChanged{.transport = ao::audio::Transport::Playing});
    _impl->events.publish(NowPlayingTrackChanged{
      .trackId = descriptor.trackId,
      .sourceListId = sourceListId,
    });
  }

  ao::TrackId PlaybackService::playSelectionInView(ViewId viewId)
  {
    try
    {
      auto const state = _impl->views.trackListState(viewId);
      auto const sel = state.selection;

      if (sel.empty())
      {
        return ao::TrackId{};
      }

      auto const trackId = sel.front();
      auto const txn = _impl->library.readTransaction();
      auto reader = _impl->library.tracks().reader(txn);
      auto const optView = reader.get(trackId, ao::library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        return ao::TrackId{};
      }

      auto uri = std::filesystem::path{optView->property().uri()};
      auto filePath = uri.is_absolute() ? uri.lexically_normal() : (_impl->library.rootPath() / uri).lexically_normal();

      auto desc = ao::audio::TrackPlaybackDescriptor{
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
      return ao::TrackId{};
    }
  }

  void PlaybackService::pause()
  {
    _impl->player->pause();
    _impl->state = _impl->buildState(*_impl->player);
    _impl->events.publish(PlaybackTransportChanged{
      .transport = ao::audio::Transport::Paused,
    });
  }

  void PlaybackService::resume()
  {
    _impl->player->resume();
    _impl->state = _impl->buildState(*_impl->player);
    _impl->events.publish(PlaybackTransportChanged{
      .transport = ao::audio::Transport::Playing,
    });
  }

  void PlaybackService::stop()
  {
    _impl->player->stop();
    _impl->currentTrackId = {};
    _impl->currentSourceListId = {};
    _impl->currentTrackTitle.clear();
    _impl->currentTrackArtist.clear();
    _impl->state = _impl->buildState(*_impl->player);
    _impl->events.publish(PlaybackStopped{});
    _impl->events.publish(PlaybackTransportChanged{.transport = ao::audio::Transport::Idle});
  }

  void PlaybackService::seek(std::uint32_t positionMs)
  {
    _impl->player->seek(positionMs);
    _impl->state = _impl->buildState(*_impl->player);
    auto const transport = _impl->state.transport;
    _impl->events.publish(PlaybackTransportChanged{.transport = transport});
  }

  void PlaybackService::setOutput(ao::audio::BackendId const& backendId,
                                  ao::audio::DeviceId const& deviceId,
                                  ao::audio::ProfileId const& profileId)
  {
    _impl->player->setOutput(backendId, deviceId, profileId);
    _impl->state = _impl->buildState(*_impl->player);
    _impl->events.publish(PlaybackOutputChanged{
      .selection =
        OutputSelection{
          .backendId = backendId,
          .deviceId = deviceId,
          .profileId = profileId,
        },
    });
  }

  void PlaybackService::setVolume(float volume)
  {
    _impl->player->setVolume(volume);
    _impl->state = _impl->buildState(*_impl->player);
  }

  void PlaybackService::setMuted(bool muted)
  {
    _impl->player->setMuted(muted);
    _impl->state = _impl->buildState(*_impl->player);
  }

  void PlaybackService::revealPlayingTrack()
  {
    _impl->events.publish(RevealTrackRequested{
      .trackId = _impl->state.trackId,
      .preferredListId = _impl->state.sourceListId,
    });
  }

  void PlaybackService::addProvider(std::unique_ptr<ao::audio::IBackendProvider> provider)
  {
    _impl->player->addProvider(std::move(provider));
  }
}
