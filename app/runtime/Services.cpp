// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "Services.h"
#include "CommandBus.h"
#include "CommandTypes.h"
#include "EventBus.h"
#include "EventTypes.h"
#include "ObservableStore.h"
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
#include <ao/utility/IMainThreadDispatcher.h>

#include <algorithm>
#include <memory>
#include <ranges>
#include <vector>

namespace ao::app
{
  namespace
  {
    class ExecutorDispatcher final : public ao::IMainThreadDispatcher
    {
    public:
      explicit ExecutorDispatcher(IControlExecutor& executor)
        : _executor{&executor}
      {
      }

      void dispatch(std::function<void()> task) override { _executor->dispatch(std::move(task)); }

    private:
      IControlExecutor* _executor;
    };

    auto buildPlaybackState(ao::audio::Player& player) -> PlaybackState
    {
      auto status = player.status();

      return PlaybackState{
        .transport = status.engine.transport,
        .positionMs = status.engine.positionMs,
        .durationMs = status.engine.durationMs,
        .volume = status.volume,
        .muted = status.muted,
        .volumeAvailable = status.volumeAvailable,
        .ready = status.isReady,
        .quality = PlaybackQuality::Unknown,
      };
    }
  }

  struct PlaybackService::Impl final
  {
    IControlExecutor& executor;
    EventBus& events;
    ObservableStore<PlaybackState> store;
    std::shared_ptr<ExecutorDispatcher> dispatcher;
    std::unique_ptr<ao::audio::Player> player;

    Impl(IControlExecutor& exec, EventBus& ev)
      : executor{exec}
      , events{ev}
      , dispatcher{std::make_shared<ExecutorDispatcher>(exec)}
      , player{std::make_unique<ao::audio::Player>(dispatcher)}
    {
    }
  };

  PlaybackService::PlaybackService(CommandBus& commands, EventBus& events, IControlExecutor& executor)
    : _impl{std::make_unique<Impl>(executor, events)}
  {
    commands.registerHandler<PlayTrack>(
      [this](PlayTrack const& cmd) -> ao::Result<void>
      {
        auto desc = ao::audio::TrackPlaybackDescriptor{
          .trackId = cmd.trackId,
        };
        _impl->player->play(desc);
        _impl->store.update(buildPlaybackState(*_impl->player));
        _impl->events.publish(PlaybackTransportChanged{
          .transport = ao::audio::Transport::Playing,
        });
        _impl->events.publish(NowPlayingTrackChanged{
          .trackId = cmd.trackId,
          .sourceListId = cmd.sourceListId,
        });
        return {};
      });

    commands.registerHandler<PlaySelectionInView>([](PlaySelectionInView const& /*cmd*/) -> ao::Result<ao::TrackId>
                                                  { return ao::TrackId{}; });

    commands.registerHandler<PlaySelectionInFocusedView>(
      [](PlaySelectionInFocusedView const&) -> ao::Result<ao::TrackId> { return ao::TrackId{}; });

    commands.registerHandler<PausePlayback>(
      [this](PausePlayback const&) -> ao::Result<void>
      {
        _impl->player->pause();
        _impl->store.update(buildPlaybackState(*_impl->player));
        _impl->events.publish(PlaybackTransportChanged{
          .transport = ao::audio::Transport::Paused,
        });
        return {};
      });

    commands.registerHandler<ResumePlayback>(
      [this](ResumePlayback const&) -> ao::Result<void>
      {
        _impl->player->resume();
        _impl->store.update(buildPlaybackState(*_impl->player));
        _impl->events.publish(PlaybackTransportChanged{
          .transport = ao::audio::Transport::Playing,
        });
        return {};
      });

    commands.registerHandler<StopPlayback>(
      [this](StopPlayback const&) -> ao::Result<void>
      {
        _impl->player->stop();
        _impl->store.update(buildPlaybackState(*_impl->player));
        _impl->events.publish(PlaybackTransportChanged{
          .transport = ao::audio::Transport::Idle,
        });
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
        _impl->store.update(buildPlaybackState(*_impl->player));
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
        return {};
      });

    commands.registerHandler<SetPlaybackMuted>(
      [this](SetPlaybackMuted const& cmd) -> ao::Result<void>
      {
        _impl->player->setMuted(cmd.muted);
        return {};
      });

    _impl->player->setTrackEndedCallback(
      [this]
      {
        _impl->store.update(buildPlaybackState(*_impl->player));
        _impl->events.publish(PlaybackTransportChanged{
          .transport = ao::audio::Transport::Idle,
        });
      });
  }

  PlaybackService::~PlaybackService() = default;

  IReadOnlyStore<PlaybackState>& PlaybackService::state()
  {
    return _impl->store;
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
    EventBus& events;
    ao::library::MusicLibrary& library;
  };

  LibraryMutationService::LibraryMutationService(CommandBus& commands,
                                                 EventBus& events,
                                                 ao::library::MusicLibrary& library)
    : _impl{std::make_unique<Impl>(events, library)}
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
        auto worker = ao::library::ImportWorker{
          _impl->library,
          cmd.paths,
          [](std::filesystem::path const&, std::int32_t) {},
          [] {},
        };
        worker.run();

        _impl->events.publish(TracksMutated{.trackIds = worker.result().insertedIds});
        _impl->events.publish(LibraryImportCompleted{
          .importedTrackCount = worker.result().insertedIds.size(),
        });

        return ImportFilesReply{
          .importedTrackCount = worker.result().insertedIds.size(),
        };
      });
  }

  LibraryMutationService::~LibraryMutationService() = default;

  struct LibraryQueryService::Impl final
  {
    ViewRegistry& views;
  };

  LibraryQueryService::LibraryQueryService(ViewRegistry& views)
    : _impl{std::make_unique<Impl>(views)}
  {
  }

  LibraryQueryService::~LibraryQueryService() = default;

  std::shared_ptr<ITrackListProjection> LibraryQueryService::trackListProjection(ViewId viewId)
  {
    return _impl->views.trackListProjection(viewId);
  }

  std::shared_ptr<ITrackDetailProjection> LibraryQueryService::detailProjection(DetailTarget const& /*target*/)
  {
    return nullptr;
  }

  struct NotificationService::Impl final
  {
    EventBus& events;
    ObservableStore<NotificationFeedState> feed;
    std::uint64_t nextId = 0;
  };

  NotificationService::NotificationService(EventBus& events)
    : _impl{std::make_unique<Impl>(events)}
  {
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
