// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "Services.h"
#include "CommandBus.h"
#include "CommandTypes.h"
#include "EventBus.h"
#include "EventTypes.h"
#include "ObservableStore.h"
#include "TrackDetailProjection.h"
#include "ViewRegistry.h"

#include <ao/Type.h>
#include <ao/audio/Player.h>
#include <ao/audio/Types.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/ImportWorker.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/utility/ThreadUtils.h>

#include <algorithm>
#include <memory>
#include <ranges>
#include <vector>

namespace ao::app
{
  namespace
  {
    auto buildPlaybackState(ao::audio::Player& player) -> PlaybackState
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
    ObservableStore<PlaybackState> store;
    std::unique_ptr<ao::audio::Player> player;
    ViewRegistry& views;
    ao::library::MusicLibrary& library;
    ao::TrackId currentTrackId{};
    ao::ListId currentSourceListId{};
    std::string currentTrackTitle{};
    std::string currentTrackArtist{};

    void ensureReady()
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

    PlaybackState buildState(ao::audio::Player& p) const
    {
      auto s = buildPlaybackState(p);
      s.trackId = currentTrackId;
      s.sourceListId = currentSourceListId;
      s.trackTitle = currentTrackTitle;
      s.trackArtist = currentTrackArtist;
      return s;
    }

    explicit Impl(IControlExecutor& exec, EventBus& ev, ViewRegistry& v, ao::library::MusicLibrary& lib)
      : executor{exec}, events{ev}, player{std::make_unique<ao::audio::Player>()}, views{v}, library{lib}
    {
      player->setTrackEndedCallback(
        [this]()
        {
          executor.dispatch(
            [this]()
            {
              auto state = buildState(*player);
              events.publish(PlaybackTransportChanged{.transport = ao::audio::Transport::Idle});
              store.update(std::move(state));
            });
        });

      player->setOnDevicesChanged(
        [this](std::vector<ao::audio::IBackendProvider::Status> const&)
        {
          executor.dispatch(
            [this]()
            {
              auto state = buildState(*player);

              // Auto-select first available default output if none is selected yet
              if (state.selectedOutput.backendId.empty() && !state.availableOutputs.empty())
              {
                auto const& backend = state.availableOutputs.front();
                if (!backend.devices.empty())
                {
                  auto const& device = backend.devices.front();
                  auto profileId = ao::audio::kProfileShared;
                  if (!backend.supportedProfiles.empty())
                  {
                    profileId = backend.supportedProfiles.front().id;
                  }
                  player->setOutput(backend.id, device.id, profileId);
                  state = buildState(*player);
                }
              }

              store.update(std::move(state));
              events.publish(PlaybackDevicesChanged{});
            });
        });

      player->setOnQualityChanged(
        [this](ao::audio::Quality quality, bool ready)
        {
          executor.dispatch(
            [this, quality, ready]()
            {
              auto state = store.snapshot();
              state.quality = quality;
              state.ready = ready;
              store.update(std::move(state));
              events.publish(PlaybackQualityChanged{.quality = quality, .ready = ready});
            });
        });
    }
  };

  PlaybackService::PlaybackService(CommandBus& commands,
                                   EventBus& events,
                                   IControlExecutor& executor,
                                   ViewRegistry& views,
                                   ao::library::MusicLibrary& library)
    : _impl{std::make_unique<Impl>(executor, events, views, library)}
  {
    commands.registerHandler<PlayTrack>(
      [this](PlayTrack const& cmd) -> ao::Result<void>
      {
        _impl->ensureReady();
        _impl->player->play(cmd.descriptor);
        _impl->currentTrackId = cmd.descriptor.trackId;
        _impl->currentSourceListId = cmd.sourceListId;
        _impl->currentTrackTitle = cmd.descriptor.title;
        _impl->currentTrackArtist = cmd.descriptor.artist;
        _impl->store.update(_impl->buildState(*_impl->player));
        _impl->events.publish(PlaybackTransportChanged{.transport = ao::audio::Transport::Playing});
        _impl->events.publish(NowPlayingTrackChanged{
          .trackId = cmd.descriptor.trackId,
          .sourceListId = cmd.sourceListId,
        });
        return {};
      });

    commands.registerHandler<PlaySelectionInView>(
      [this](PlaySelectionInView const& cmd) -> ao::Result<ao::TrackId>
      {
        try
        {
          auto const& state = _impl->views.trackListState(cmd.viewId);
          auto const sel = state.snapshot().selection;
          if (sel.empty())
          {
            return ao::TrackId{};
          }

          auto const trackId = sel.front();
          auto txn = _impl->library.readTransaction();
          auto reader = _impl->library.tracks().reader(txn);
          auto const optView = reader.get(trackId, ao::library::TrackStore::Reader::LoadMode::Both);
          if (!optView)
          {
            return ao::TrackId{};
          }

          auto uri = std::filesystem::path{optView->property().uri()};
          auto filePath =
            uri.is_absolute() ? uri.lexically_normal() : (_impl->library.rootPath() / uri).lexically_normal();

          auto desc = ao::audio::TrackPlaybackDescriptor{
            .trackId = trackId,
            .filePath = filePath,
            .title = std::string{optView->metadata().title()},
            .durationMs = optView->coldHeader().durationMs,
          };

          _impl->ensureReady();
          _impl->player->play(desc);
          _impl->currentTrackId = trackId;
          _impl->currentSourceListId = state.snapshot().listId;
          _impl->currentTrackTitle = desc.title;
          _impl->currentTrackArtist = desc.artist;
          _impl->store.update(_impl->buildState(*_impl->player));
          _impl->events.publish(PlaybackTransportChanged{.transport = ao::audio::Transport::Playing});
          _impl->events.publish(NowPlayingTrackChanged{.trackId = trackId, .sourceListId = state.snapshot().listId});
          return trackId;
        }
        catch (std::exception const&)
        {
          return ao::TrackId{};
        }
      });

    commands.registerHandler<PausePlayback>(
      [this](PausePlayback const&) -> ao::Result<void>
      {
        _impl->player->pause();
        _impl->store.update(_impl->buildState(*_impl->player));
        _impl->events.publish(PlaybackTransportChanged{
          .transport = ao::audio::Transport::Paused,
        });
        return {};
      });

    commands.registerHandler<ResumePlayback>(
      [this](ResumePlayback const&) -> ao::Result<void>
      {
        _impl->player->resume();
        _impl->store.update(_impl->buildState(*_impl->player));
        _impl->events.publish(PlaybackTransportChanged{
          .transport = ao::audio::Transport::Playing,
        });
        return {};
      });

    commands.registerHandler<StopPlayback>(
      [this](StopPlayback const&) -> ao::Result<void>
      {
        _impl->player->stop();
        _impl->currentTrackId = {};
        _impl->currentSourceListId = {};
        _impl->currentTrackTitle.clear();
        _impl->currentTrackArtist.clear();
        _impl->store.update(_impl->buildState(*_impl->player));
        _impl->events.publish(PlaybackStopped{});
        _impl->events.publish(PlaybackTransportChanged{.transport = ao::audio::Transport::Idle});
        return {};
      });

    commands.registerHandler<SeekPlayback>(
      [this](SeekPlayback const& cmd) -> ao::Result<void>
      {
        _impl->player->seek(cmd.positionMs);
        return {};
      });

    commands.registerHandler<SetPlaybackOutput>(
      [this](SetPlaybackOutput const& cmd) -> ao::Result<void>
      {
        _impl->player->setOutput(cmd.backendId, cmd.deviceId, cmd.profileId);
        _impl->store.update(_impl->buildState(*_impl->player));
        _impl->events.publish(PlaybackOutputChanged{
          .selection =
            OutputSelection{
              .backendId = cmd.backendId,
              .deviceId = cmd.deviceId,
              .profileId = cmd.profileId,
            },
        });
        return {};
      });

    commands.registerHandler<SetPlaybackVolume>(
      [this](SetPlaybackVolume const& cmd) -> ao::Result<void>
      {
        _impl->player->setVolume(cmd.volume);
        _impl->store.update(_impl->buildState(*_impl->player));
        return {};
      });

    commands.registerHandler<SetPlaybackMuted>(
      [this](SetPlaybackMuted const& cmd) -> ao::Result<void>
      {
        _impl->player->setMuted(cmd.muted);
        _impl->store.update(_impl->buildState(*_impl->player));
        return {};
      });

    commands.registerHandler<RefreshPlaybackState>(
      [this](RefreshPlaybackState const&) -> ao::Result<void>
      {
        _impl->store.update(_impl->buildState(*_impl->player));
        return {};
      });

    commands.registerHandler<RevealPlayingTrack>(
      [this](RevealPlayingTrack const&) -> ao::Result<void>
      {
        auto const state = _impl->store.snapshot();
        _impl->events.publish(RevealTrackRequested{
          .trackId = state.trackId,
          .preferredListId = state.sourceListId,
        });
        return {};
      });
  }

  PlaybackService::~PlaybackService() = default;

  IReadOnlyStore<PlaybackState>& PlaybackService::state()
  {
    return _impl->store;
  }

  void PlaybackService::addProvider(std::unique_ptr<ao::audio::IBackendProvider> provider)
  {
    _impl->player->addProvider(std::move(provider));
  }

  namespace
  {
    struct PatchResult final
    {
      bool changedHot = false;
      bool changedCold = false;
    };

    auto applyMetadataPatch(ao::library::TrackBuilder& builder, MetadataPatch const& patch) -> PatchResult
    {
      auto& meta = builder.metadata();
      auto result = PatchResult{};

      if (patch.optTitle)
      {
        meta.title(*patch.optTitle);
        result.changedHot = true;
      }

      if (patch.optArtist)
      {
        meta.artist(*patch.optArtist);
        result.changedHot = true;
      }

      if (patch.optAlbum)
      {
        meta.album(*patch.optAlbum);
        result.changedHot = true;
      }

      if (patch.optGenre)
      {
        meta.genre(*patch.optGenre);
        result.changedHot = true;
      }

      if (patch.optComposer)
      {
        meta.composer(*patch.optComposer);
        result.changedHot = true;
      }

      if (patch.optWork)
      {
        meta.work(*patch.optWork);
        result.changedCold = true;
      }

      return result;
    }
  }

  struct LibraryMutationService::Impl final
  {
    IControlExecutor& executor;
    EventBus& events;
    ao::library::MusicLibrary& library;
    std::jthread importThread;
  };

  LibraryMutationService::LibraryMutationService(CommandBus& commands,
                                                 EventBus& events,
                                                 IControlExecutor& executor,
                                                 ao::library::MusicLibrary& library)
    : _impl{std::make_unique<Impl>(executor, events, library)}
  {
    commands.registerHandler<UpdateTrackMetadata>(
      [this](UpdateTrackMetadata const& cmd) -> ao::Result<UpdateTrackMetadataReply>
      {
        auto txn = _impl->library.writeTransaction();
        auto writer = _impl->library.tracks().writer(txn);
        auto mutated = std::vector<ao::TrackId>{};

        for (auto const trackId : cmd.trackIds)
        {
          auto const optView = writer.get(trackId, ao::library::TrackStore::Reader::LoadMode::Both);
          if (!optView)
          {
            continue;
          }

          auto builder = ao::library::TrackBuilder::fromView(*optView, _impl->library.dictionary());

          auto patchResult = applyMetadataPatch(builder, cmd.patch);

          if (patchResult.changedHot)
          {
            auto const hotData = builder.serializeHot(txn, _impl->library.dictionary());
            writer.updateHot(trackId, hotData);
          }

          if (patchResult.changedCold)
          {
            auto const coldData = builder.serializeCold(txn, _impl->library.dictionary(), _impl->library.resources());
            writer.updateCold(trackId, coldData);
          }

          mutated.push_back(trackId);
        }

        txn.commit();

        _impl->events.publish(TracksMutated{.trackIds = mutated});

        return UpdateTrackMetadataReply{.mutatedIds = std::move(mutated)};
      });

    commands.registerHandler<EditTrackTags>(
      [this](EditTrackTags const& cmd) -> ao::Result<EditTrackTagsReply>
      {
        auto txn = _impl->library.writeTransaction();
        auto writer = _impl->library.tracks().writer(txn);
        auto mutated = std::vector<ao::TrackId>{};

        for (auto const trackId : cmd.trackIds)
        {
          auto const optView = writer.get(trackId, ao::library::TrackStore::Reader::LoadMode::Hot);
          if (!optView)
          {
            continue;
          }

          auto builder = ao::library::TrackBuilder::fromView(*optView, _impl->library.dictionary());

          auto& tags = builder.tags();
          for (auto const& tag : cmd.tagsToAdd)
          {
            tags.add(tag);
          }
          for (auto const& tag : cmd.tagsToRemove)
          {
            tags.remove(tag);
          }

          auto const hotData = builder.serializeHot(txn, _impl->library.dictionary());
          writer.updateHot(trackId, hotData);
          mutated.push_back(trackId);
        }

        txn.commit();

        _impl->events.publish(TracksMutated{.trackIds = mutated});

        return EditTrackTagsReply{.mutatedIds = std::move(mutated)};
      });

    commands.registerHandler<ImportFiles>(
      [this](ImportFiles const& cmd) -> ao::Result<ImportFilesReply>
      {
        auto totalFiles = cmd.paths.size();
        auto worker = std::make_shared<ao::library::ImportWorker>(
          _impl->library,
          cmd.paths,
          [this, totalFiles](std::filesystem::path const& filePath, std::int32_t index)
          {
            _impl->executor.dispatch(
              [this, filePath, index, totalFiles]()
              {
                _impl->events.publish(ImportProgressUpdated{
                  .fraction = totalFiles > 0 ? static_cast<double>(index) / static_cast<double>(totalFiles) : 0.0,
                  .message = "Importing: " + filePath.filename().string(),
                });
              });
          },
          []() {});

        _impl->importThread = std::jthread(
          [this, worker]()
          {
            ao::setCurrentThreadName("FileImport");
            worker->run();

            auto const& result = worker->result();
            _impl->executor.dispatch(
              [this, ids = result.insertedIds, count = result.insertedIds.size()]()
              {
                _impl->events.publish(TracksMutated{.trackIds = ids});
                _impl->events.publish(LibraryImportCompleted{.importedTrackCount = count});
              });
          });

        return ImportFilesReply{.importedTrackCount = 0};
      });
  }

  LibraryMutationService::~LibraryMutationService() = default;

  struct LibraryQueryService::Impl final
  {
    ViewRegistry& views;
    EventBus& events;
    ao::library::MusicLibrary& library;
  };

  LibraryQueryService::LibraryQueryService(ViewRegistry& views, EventBus& events, ao::library::MusicLibrary& library)
    : _impl{std::make_unique<Impl>(views, events, library)}
  {
  }

  LibraryQueryService::~LibraryQueryService() = default;

  std::shared_ptr<ITrackListProjection> LibraryQueryService::trackListProjection(ViewId viewId)
  {
    return _impl->views.trackListProjection(viewId);
  }

  std::shared_ptr<ITrackDetailProjection> LibraryQueryService::detailProjection(DetailTarget const& target)
  {
    return std::make_shared<TrackDetailProjection>(target, _impl->views, _impl->events, _impl->library);
  }

  struct NotificationService::Impl final
  {
    EventBus& events;
    ObservableStore<NotificationFeedState> feed;
    std::uint64_t nextId = 0;
  };

  NotificationService::NotificationService(CommandBus& commands, EventBus& events)
    : _impl{std::make_unique<Impl>(events)}
  {
    commands.registerHandler<PostNotification>([this](PostNotification const& cmd) -> ao::Result<NotificationId>
                                               { return post(cmd.severity, cmd.message, cmd.sticky, cmd.optTimeout); });

    commands.registerHandler<DismissNotification>(
      [this](DismissNotification const& cmd) -> ao::Result<void>
      {
        dismiss(cmd.id);
        return {};
      });
  }

  NotificationService::~NotificationService() = default;

  IReadOnlyStore<NotificationFeedState>& NotificationService::feed()
  {
    return _impl->feed;
  }

  NotificationId NotificationService::post(NotificationSeverity severity,
                                           std::string message,
                                           bool sticky,
                                           std::optional<std::chrono::milliseconds> optTimeout)
  {
    auto const id = NotificationId{++_impl->nextId};

    auto entry = NotificationEntry{
      .id = id,
      .severity = severity,
      .message = std::move(message),
      .sticky = sticky,
      .optTimeout = optTimeout,
    };

    auto state = _impl->feed.snapshot();
    state.entries.push_back(std::move(entry));
    _impl->feed.update(std::move(state));

    _impl->events.publish(NotificationPosted{.id = id});

    return id;
  }

  void NotificationService::dismiss(NotificationId id)
  {
    auto state = _impl->feed.snapshot();

    auto const it = std::ranges::find(state.entries, id, &NotificationEntry::id);

    if (it != state.entries.end())
    {
      state.entries.erase(it);
      _impl->feed.update(std::move(state));
      _impl->events.publish(NotificationDismissed{.id = id});
    }
  }

  void NotificationService::dismissAll()
  {
    auto state = _impl->feed.snapshot();
    state.entries.clear();
    _impl->feed.update(std::move(state));
  }
}
