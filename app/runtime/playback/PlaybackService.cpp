// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackSuccession.h"
#include "runtime/playback/PlaybackTransport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    PlaybackSourceState mapSourceState(PlaybackSuccessionSourceState const state) noexcept
    {
      switch (state)
      {
        case PlaybackSuccessionSourceState::Inactive: return PlaybackSourceState::Inactive;
        case PlaybackSuccessionSourceState::Live: return PlaybackSourceState::Live;
        case PlaybackSuccessionSourceState::Invalidated: return PlaybackSourceState::Invalidated;
      }

      return PlaybackSourceState::Inactive;
    }
  } // namespace

  struct PlaybackService::Impl final
    : PlaybackCommands
    , PlaybackEvents
  {
    struct DeferredPublicationControl final
    {
      Impl* owner = nullptr;
    };

    Impl(async::Executor& executorRef, PlaybackTransport& transportRef, PlaybackSuccession& successionRef)
      : executor{executorRef}
      , transport{transportRef}
      , succession{successionRef}
      , deferredPublicationControlPtr{std::make_shared<DeferredPublicationControl>(this)}
    {
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() override
    {
      subscriptions.clear();
      deferredPublicationControlPtr->owner = nullptr;
      deferredPublicationControlPtr.reset();
    }

    // One accepted logical command through PlaybackService brackets every lower signal
    // it triggers so at most one snapshot is published for it.
    class [[nodiscard]] CommandBracket final
    {
    public:
      explicit CommandBracket(Impl& owner) noexcept
        : _owner{owner}
      {
        _owner.beginCommand();
      }

      ~CommandBracket() { _owner.endCommand(); }

      CommandBracket(CommandBracket const&) = delete;
      CommandBracket& operator=(CommandBracket const&) = delete;
      CommandBracket(CommandBracket&&) = delete;
      CommandBracket& operator=(CommandBracket&&) = delete;

    private:
      Impl& _owner;
    };

    // Playback snapshot and events -------------------------------------------

    PlaybackSnapshot snapshot() const
    {
      auto composed = composeContent();
      composed.revision = lastSnapshot.revision;
      return composed;
    }

    async::Subscription onSnapshot(PlaybackSnapshotObserver observer) override
    {
      if (!snapshotSignal.hasConnectedHandlers())
      {
        lastSnapshot = composeContent();
        lastSnapshot.revision = PlaybackRevision{.value = revisionCounter};
      }

      return snapshotSignal.connect(std::move(observer));
    }

    async::Subscription onPlaybackFailure(std::move_only_function<void(PlaybackFailureEvent const&)> handler) override
    {
      return failureSignal.connect(std::move(handler));
    }

    async::Subscription onSeekPreview(std::move_only_function<void(PlaybackSeekPreview const&)> handler) override
    {
      return seekPreviewSignal.connect(std::move(handler));
    }

    async::Subscription onRevealTrackRequested(
      std::move_only_function<void(PlaybackRevealTrackRequest const&)> handler) override
    {
      return revealTrackSignal.connect(std::move(handler));
    }

    // PlaybackCommands --------------------------------------------------------

    Result<> startFromView(ViewId const viewId, TrackId const startTrackId) override
    {
      auto const bracket = CommandBracket{*this};
      return succession.playFromView(viewId, startTrackId);
    }

    void next() override
    {
      auto const bracket = CommandBracket{*this};
      succession.next();
    }

    void previous() override
    {
      auto const bracket = CommandBracket{*this};
      succession.previous();
    }

    void clearSequence() override
    {
      auto const bracket = CommandBracket{*this};
      succession.clear();
    }

    void setShuffleMode(ShuffleMode const mode) override
    {
      auto const bracket = CommandBracket{*this};
      succession.setShuffleMode(mode);
    }

    void setRepeatMode(RepeatMode const mode) override
    {
      auto const bracket = CommandBracket{*this};
      succession.setRepeatMode(mode);
    }

    void pause() override
    {
      auto const bracket = CommandBracket{*this};
      transport.pause();
    }

    void resume() override
    {
      auto const bracket = CommandBracket{*this};
      transport.resume();
    }

    void stop() override
    {
      auto const bracket = CommandBracket{*this};
      std::ignore = transport.stop();
    }

    void seek(std::chrono::milliseconds const elapsed, PlaybackSeekMode const mode) override
    {
      auto const bracket = CommandBracket{*this};
      transport.seek(
        elapsed,
        mode == PlaybackSeekMode::Preview ? PlaybackTransport::SeekMode::Preview : PlaybackTransport::SeekMode::Final);
    }

    void setOutputDevice(audio::BackendId const& backendId,
                         audio::DeviceId const& deviceId,
                         audio::ProfileId const& profileId) override
    {
      auto const bracket = CommandBracket{*this};
      transport.setOutputDevice(backendId, deviceId, profileId);
    }

    void setVolume(float const volume) override
    {
      auto const bracket = CommandBracket{*this};
      transport.setVolume(volume);
    }

    void setMuted(bool const muted) override
    {
      auto const bracket = CommandBracket{*this};
      transport.setMuted(muted);
    }

    void revealPlayingTrack() override { transport.revealPlayingTrack(); }

    void revealTrack(TrackId const trackId, ViewId const preferredViewId, ListId const preferredListId) override
    {
      transport.revealTrack(trackId, preferredViewId, preferredListId);
    }

    // Adapter wiring ----------------------------------------------------------

    void connectSources()
    {
      auto const markChanged = [this] { onSourceChanged(); };

      subscriptions.push_back(transport.onPreparing(markChanged));
      subscriptions.push_back(transport.onStarted(markChanged));
      subscriptions.push_back(transport.onPaused(markChanged));
      subscriptions.push_back(transport.onIdle(markChanged));
      subscriptions.push_back(transport.onStopped(markChanged));
      subscriptions.push_back(transport.onOutputDevicesChanged(markChanged));
      subscriptions.push_back(transport.onNowPlayingChanged([this](auto const&) { onSourceChanged(); }));
      subscriptions.push_back(transport.onOutputDeviceChanged([this](auto const&) { onSourceChanged(); }));
      subscriptions.push_back(transport.onQualityChanged([this](auto const&) { onSourceChanged(); }));
      subscriptions.push_back(transport.onVolumeChanged([this](float) { onSourceChanged(); }));
      subscriptions.push_back(transport.onMutedChanged([this](bool) { onSourceChanged(); }));
      subscriptions.push_back(
        transport.onSeekUpdate([this](PlaybackTransport::SeekUpdate const& update) { onSeekUpdate(update); }));
      subscriptions.push_back(
        transport.onPlaybackFailure([this](PlaybackFailure const& failure) { onFailure(failure); }));
      subscriptions.push_back(transport.onRevealTrackRequested(
        [this](PlaybackTransport::RevealTrackRequested const& request)
        {
          emitSafely(revealTrackSignal,
                     PlaybackRevealTrackRequest{.trackId = request.trackId,
                                                .preferredViewId = request.preferredViewId,
                                                .preferredListId = request.preferredListId});
        }));

      subscriptions.push_back(succession.onChanged([this](PlaybackSuccessionState const&) { onSourceChanged(); }));
      subscriptions.push_back(
        succession.onShuffleModeChanged([this](PlaybackSuccession::ShuffleModeChanged const&) { onSourceChanged(); }));
      subscriptions.push_back(
        succession.onRepeatModeChanged([this](PlaybackSuccession::RepeatModeChanged const&) { onSourceChanged(); }));
    }

    void onSourceChanged()
    {
      if (bracketDepth != 0)
      {
        bracketDirty = true;
        return;
      }

      scheduleDeferredPublish();
    }

    void onSeekUpdate(PlaybackTransport::SeekUpdate const& update)
    {
      if (update.mode == PlaybackTransport::SeekMode::Preview)
      {
        emitSafely(
          seekPreviewSignal, PlaybackSeekPreview{.revision = lastSnapshot.revision, .elapsed = update.elapsed});
        return;
      }

      // A final seek commits the transport position; publish it like any other
      // state change.
      onSourceChanged();
    }

    void onFailure(PlaybackFailure const& failure)
    {
      emitSafely(
        failureSignal,
        PlaybackFailureEvent{.revision = lastSnapshot.revision, .optCommandId = std::nullopt, .failure = failure});
    }

    void scheduleDeferredPublish()
    {
      // The adapter is inert until observed: with no snapshot subscriber it
      // enqueues no executor work and advances no revision, so it never
      // perturbs the scheduling that unmigrated callers still depend on.
      if (publishScheduled || !snapshotSignal.hasConnectedHandlers())
      {
        return;
      }

      publishScheduled = true;
      auto const weakControlPtr = std::weak_ptr<DeferredPublicationControl>{deferredPublicationControlPtr};
      executor.defer(
        [weakControlPtr]
        {
          auto const controlPtr = weakControlPtr.lock();

          if (controlPtr == nullptr || controlPtr->owner == nullptr)
          {
            return;
          }

          controlPtr->owner->publishScheduled = false;
          controlPtr->owner->publishNow();
        });
    }

    void beginCommand() noexcept { ++bracketDepth; }

    void endCommand() noexcept
    {
      if (--bracketDepth == 0 && bracketDirty)
      {
        bracketDirty = false;
        publishNow();
      }
    }

    void publishNow()
    {
      if (!snapshotSignal.hasConnectedHandlers())
      {
        return;
      }

      auto content = composeContent();

      if (content.sameContentAs(lastSnapshot))
      {
        return;
      }

      content.revision = PlaybackRevision{.value = ++revisionCounter};
      lastSnapshot = content;
      emitSafely(snapshotSignal, lastSnapshot);
    }

    template<typename Event>
    static void emitSafely(async::Signal<Event const&>& signal, Event const& event) noexcept
    {
      try
      {
        signal.emit(event);
      }
      catch (...)
      {
        // Runtime observation is exception-contained by the boundary. Every
        // still-connected observer has already run because Signal preserves
        // the first exception until delivery completes.
        return;
      }
    }

    PlaybackSnapshot composeContent() const
    {
      auto const& transportState = transport.state();
      auto const& successionState = succession.state();

      return PlaybackSnapshot{
        .revision = {},
        .transport =
          PlaybackTransportSnapshot{
            .transport = transportState.transport,
            .ready = transportState.ready,
            .elapsed = transportState.elapsed,
            .duration = transportState.duration,
            .nowPlaying = transportState.nowPlaying,
            .volume = transportState.volume,
            .output = transportState.output,
            .quality = transportState.quality,
          },
        .succession =
          PlaybackSuccessionSnapshot{
            .sourceState = mapSourceState(successionState.sourceState),
            .currentTrackId = successionState.currentTrackId,
            .sourceListId = successionState.sourceListId,
            .hasNext = successionState.hasNext,
            .hasPrevious = successionState.hasPrevious,
            .shuffle = successionState.shuffle,
            .repeat = successionState.repeat,
          },
        .preparation =
          PlaybackPreparationSnapshot{
            .hasPreparedNext = successionState.optResolvedSuccessor.has_value(),
          },
      };
    }

    async::Executor& executor;
    PlaybackTransport& transport;
    PlaybackSuccession& succession;

    async::Signal<PlaybackSnapshot const&> snapshotSignal;
    async::Signal<PlaybackFailureEvent const&> failureSignal;
    async::Signal<PlaybackSeekPreview const&> seekPreviewSignal;
    async::Signal<PlaybackRevealTrackRequest const&> revealTrackSignal;

    std::vector<async::Subscription> subscriptions;
    std::shared_ptr<DeferredPublicationControl> deferredPublicationControlPtr;
    PlaybackSnapshot lastSnapshot{};
    std::uint64_t revisionCounter = 0;
    std::size_t bracketDepth = 0;
    bool bracketDirty = false;
    bool publishScheduled = false;
  };

  PlaybackBootstrap::PlaybackBootstrap(PlaybackTransport& transport) noexcept
    : _transport{transport}
  {
  }

  std::unique_ptr<PlaybackService> PlaybackBootstrap::createPlaybackService(async::Executor& executor,
                                                                            PlaybackSuccession& succession)
  {
    return std::unique_ptr<PlaybackService>{
      new PlaybackService{std::make_unique<PlaybackService::Impl>(executor, _transport, succession)}};
  }

  void PlaybackBootstrap::addProvider(std::unique_ptr<audio::BackendProvider> providerPtr)
  {
    _transport.addProvider(std::move(providerPtr));
  }

  void PlaybackBootstrap::shutdown() noexcept
  {
    _transport.shutdown();
  }

  PlaybackService::PlaybackService(std::unique_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
    _implPtr->connectSources();
  }

  PlaybackService::~PlaybackService() = default;

  PlaybackSnapshot PlaybackService::snapshot() const
  {
    return _implPtr->snapshot();
  }

  PlaybackCommands& PlaybackService::commands() noexcept
  {
    return *_implPtr;
  }

  PlaybackEvents& PlaybackService::events() noexcept
  {
    return *_implPtr;
  }

  PlaybackService::CommandBracket::CommandBracket(PlaybackService& owner) noexcept
    : _owner{owner}
  {
    _owner._implPtr->beginCommand();
  }

  PlaybackService::CommandBracket::~CommandBracket()
  {
    _owner._implPtr->endCommand();
  }
} // namespace ao::rt
