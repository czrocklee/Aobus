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
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
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
    struct DeferredControl final
    {
      Impl* owner = nullptr;
    };

    struct QueuedCommand final
    {
      std::uint64_t generation = 0;
      bool staleWhenSuperseded = false;
      std::move_only_function<Result<bool>()> operation;
    };

    Impl(async::Executor& executorRef, PlaybackTransport& transportRef, PlaybackSuccession& successionRef)
      : executor{executorRef}
      , transport{transportRef}
      , succession{successionRef}
      , deferredControlPtr{std::make_shared<DeferredControl>(this)}
      , lastSnapshot{composeContent()}
      , observedAnchorTrackId{lastSnapshot.transport.nowPlaying.trackId}
      , observedAnchorSourceListId{lastSnapshot.transport.nowPlaying.sourceListId}
    {
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() override
    {
      subscriptions.clear();
      deferredControlPtr->owner = nullptr;
      deferredControlPtr.reset();
    }

    // Playback snapshot and events -------------------------------------------

    PlaybackSnapshot const& snapshot() const noexcept { return lastSnapshot; }

    async::Subscription onSnapshot(PlaybackSnapshotObserver observer) override
    {
      return snapshotSignal.connect(std::move(observer));
    }

    async::Subscription onSeekPreview(std::move_only_function<void(std::chrono::milliseconds)> handler) override
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
      return submitResult(
        [this, viewId, startTrackId] -> Result<bool>
        {
          if (auto const started = succession.playFromView(viewId, startTrackId); !started)
          {
            return std::unexpected{started.error()};
          }

          // Admission starts worker preparation but does not establish a new
          // playback clock anchor. The accepted completion publishes that
          // anchor after transport and succession commit together.
          return false;
        },
        true,
        true);
    }

    void next() override
    {
      submitPositioning([this] { return succession.next(); }, true, true);
    }

    void previous() override
    {
      submitPositioning([this] { return succession.previous(); }, true, true);
    }

    void clearSequence() override
    {
      submitVoid([this] { succession.clear(); }, true, false);
    }

    void setShuffleMode(ShuffleMode const mode) override
    {
      submitVoid([this, mode] { succession.setShuffleMode(mode); }, false, false);
    }

    void setRepeatMode(RepeatMode const mode) override
    {
      submitVoid([this, mode] { succession.setRepeatMode(mode); }, false, false);
    }

    void pause() override
    {
      submitVoid([this] { transport.pause(); }, false, false);
    }

    void resume() override
    {
      submitVoid([this] { transport.resume(); }, false, false);
    }

    void stop() override
    {
      submitVoid([this] { std::ignore = transport.stop(); }, true, false);
    }

    void seek(std::chrono::milliseconds const elapsed, PlaybackSeekMode const mode) override
    {
      submitVoid(
        [this, elapsed, mode]
        {
          transport.seek(elapsed,
                         mode == PlaybackSeekMode::Preview ? PlaybackTransport::SeekMode::Preview
                                                           : PlaybackTransport::SeekMode::Final);
        },
        false,
        false);
    }

    void setOutputDevice(audio::BackendId const& backendId,
                         audio::DeviceId const& deviceId,
                         audio::ProfileId const& profileId) override
    {
      submitVoid([this, backendId, deviceId, profileId] { transport.setOutputDevice(backendId, deviceId, profileId); },
                 true,
                 false);
    }

    void setVolume(float const volume) override
    {
      submitVoid([this, volume] { transport.setVolume(volume); }, false, false);
    }

    void setMuted(bool const muted) override
    {
      submitVoid([this, muted] { transport.setMuted(muted); }, false, false);
    }

    void revealPlayingTrack() override
    {
      submitVoid([this] { transport.revealPlayingTrack(); }, false, false);
    }

    void revealTrack(TrackId const trackId, ViewId const preferredViewId, ListId const preferredListId) override
    {
      submitVoid([this, trackId, preferredViewId, preferredListId]
                 { transport.revealTrack(trackId, preferredViewId, preferredListId); },
                 false,
                 false);
    }

    Result<> submitResult(std::move_only_function<Result<bool>()> operation,
                          bool const invalidatesOlderCommand,
                          bool const staleWhenSuperseded)
    {
      if (closed)
      {
        return makeError(Error::Code::InvalidState, "Playback is shutting down");
      }

      auto command = QueuedCommand{
        .generation = ++commandGenerationCounter,
        .staleWhenSuperseded = staleWhenSuperseded,
        .operation = std::move(operation),
      };

      if (invalidatesOlderCommand)
      {
        latestInvalidatingGeneration = command.generation;
      }

      if (insideBoundary() || hasQueuedCommandBacklog())
      {
        queuedCommands.push_back(std::move(command));

        if (!insideBoundary())
        {
          scheduleCommandDrain();
        }

        return {};
      }

      return executeCommandAndContinue(command);
    }

    void submitVoid(std::move_only_function<void()> operation,
                    bool const invalidatesOlderCommand,
                    bool const staleWhenSuperseded)
    {
      std::ignore = submitResult(
        [operation = std::move(operation)] mutable -> Result<bool>
        {
          operation();
          return false;
        },
        invalidatesOlderCommand,
        staleWhenSuperseded);
    }

    void submitPositioning(std::move_only_function<bool()> operation,
                           bool const invalidatesOlderCommand,
                           bool const staleWhenSuperseded)
    {
      std::ignore = submitResult([operation = std::move(operation)] mutable -> Result<bool> { return operation(); },
                                 invalidatesOlderCommand,
                                 staleWhenSuperseded);
    }

    Result<> executeCommand(QueuedCommand& command)
    {
      if (closed || (command.staleWhenSuperseded && command.generation < latestInvalidatingGeneration))
      {
        return {};
      }

      beginCommit();
      auto result = Result<bool>{};
      auto operationException = std::exception_ptr{};
      bool forcesPositionAnchor = false;

      try
      {
        result = command.operation();

        if (result)
        {
          forcesPositionAnchor = *result;
        }
      }
      catch (...)
      {
        operationException = std::current_exception();
      }

      auto settlementException = std::exception_ptr{};

      try
      {
        settleCommit(forcesPositionAnchor);
      }
      catch (...)
      {
        settlementException = std::current_exception();
      }

      closeCommit();

      if (operationException)
      {
        std::rethrow_exception(operationException);
      }

      if (settlementException)
      {
        std::rethrow_exception(settlementException);
      }

      if (!result)
      {
        return std::unexpected{result.error()};
      }

      return {};
    }

    Result<> executeCommandAndContinue(QueuedCommand& command)
    {
      auto result = Result<>{};
      auto executionException = std::exception_ptr{};

      try
      {
        result = executeCommand(command);
      }
      catch (...)
      {
        executionException = std::current_exception();
      }

      if (executionException)
      {
        try
        {
          scheduleCommandDrain();
        }
        catch (...) // NOLINT(bugprone-empty-catch) -- preserve the primary invariant fault
        {
          // Preserve the invariant fault from the command or its settlement.
          // A later admission retries the still-visible backlog.
        }

        std::rethrow_exception(executionException);
      }

      scheduleCommandDrain();
      return result;
    }

    bool runSynchronousCommand(std::move_only_function<bool()> operation)
    {
      if (closed || insideBoundary() || hasQueuedCommandBacklog())
      {
        return false;
      }

      auto command = QueuedCommand{
        .generation = ++commandGenerationCounter,
        .staleWhenSuperseded = false,
        .operation = [operation = std::move(operation)] mutable -> Result<bool> { return operation(); },
      };
      latestInvalidatingGeneration = command.generation;
      std::ignore = executeCommandAndContinue(command);
      return true;
    }

    void scheduleCommandDrain()
    {
      if (closed || commandDrainScheduled || queuedCommands.empty() || insideBoundary())
      {
        return;
      }

      commandDrainScheduled = true;
      auto const weakControlPtr = std::weak_ptr<DeferredControl>{deferredControlPtr};

      try
      {
        executor.defer(
          [weakControlPtr]
          {
            auto const controlPtr = weakControlPtr.lock();

            if (controlPtr == nullptr || controlPtr->owner == nullptr)
            {
              return;
            }

            controlPtr->owner->drainOneCommand();
          });
      }
      catch (...)
      {
        commandDrainScheduled = false;
        throw;
      }
    }

    void drainOneCommand()
    {
      commandDrainScheduled = false;

      if (closed || queuedCommands.empty())
      {
        return;
      }

      auto command = std::move(queuedCommands.front());
      queuedCommands.pop_front();
      std::ignore = executeCommandAndContinue(command);
    }

    bool insideBoundary() const noexcept { return commitDepth != 0 || publicationDepth != 0; }
    bool hasQueuedCommandBacklog() const noexcept { return commandDrainScheduled || !queuedCommands.empty(); }

    // Adapter wiring ----------------------------------------------------------

    void connectSources()
    {
      auto const markChanged = [this] { onSourceChanged(); };

      subscriptions.push_back(transport.onPreparing(markChanged));
      subscriptions.push_back(transport.onStarted(markChanged));
      subscriptions.push_back(transport.onPaused(markChanged));
      subscriptions.push_back(transport.onIdle([this] { onTransportIdle(); }));
      subscriptions.push_back(transport.onStopped(markChanged));
      subscriptions.push_back(transport.onOutputDevicesChanged(markChanged));
      subscriptions.push_back(transport.onNowPlayingChanged([this](PlaybackTransport::NowPlayingChanged const& event)
                                                            { onPositionAnchorChanged(event); }));
      subscriptions.push_back(transport.onOutputDeviceChanged([this](auto const&) { onSourceChanged(); }));
      subscriptions.push_back(transport.onQualityChanged([this](auto const&) { onSourceChanged(); }));
      subscriptions.push_back(transport.onVolumeChanged([this](float) { onSourceChanged(); }));
      subscriptions.push_back(transport.onMutedChanged([this](bool) { onSourceChanged(); }));
      subscriptions.push_back(
        transport.onSeekUpdate([this](PlaybackTransport::SeekUpdate const& update) { onSeekUpdate(update); }));
      subscriptions.push_back(transport.onRevealTrackRequested(
        [this](PlaybackTransport::RevealTrackRequested const& request)
        {
          onRevealTrackRequested(PlaybackRevealTrackRequest{.trackId = request.trackId,
                                                            .preferredViewId = request.preferredViewId,
                                                            .preferredListId = request.preferredListId});
        }));

      subscriptions.push_back(succession.onChanged([this](PlaybackSuccessionState const&) { onSourceChanged(); }));
      subscriptions.push_back(succession.onExplicitStartSettled(
        [this]
        {
          if (commitDepth == 0)
          {
            pendingExternalPositionAnchor = true;
          }

          onSourceChanged();
        }));
      subscriptions.push_back(
        succession.onShuffleModeChanged([this](PlaybackSuccession::ShuffleModeChanged const&) { onSourceChanged(); }));
      subscriptions.push_back(
        succession.onRepeatModeChanged([this](PlaybackSuccession::RepeatModeChanged const&) { onSourceChanged(); }));
    }

    void onSourceChanged()
    {
      if (commitDepth != 0)
      {
        return;
      }

      scheduleDeferredPublish();
    }

    void onTransportIdle()
    {
      if (commitDepth == 0)
      {
        pendingExternalPositionAnchor = true;
      }

      onSourceChanged();
    }

    void onSeekUpdate(PlaybackTransport::SeekUpdate const& update)
    {
      if (update.mode == PlaybackTransport::SeekMode::Preview)
      {
        if (commitDepth != 0)
        {
          pendingSeekPreviews.push_back(update.elapsed);
        }
        else
        {
          emitSafely(seekPreviewSignal, update.elapsed);
        }

        return;
      }

      ++positionRevisionCounter;
      ++finalSeekRevisionCounter;
      onSourceChanged();
    }

    void onPositionAnchorChanged(PlaybackTransport::NowPlayingChanged const& event)
    {
      pendingExternalPositionAnchor = false;
      auto const repeatsEmptyAnchor = event.trackId == kInvalidTrackId && event.trackId == observedAnchorTrackId &&
                                      event.sourceListId == observedAnchorSourceListId;
      observedAnchorTrackId = event.trackId;
      observedAnchorSourceListId = event.sourceListId;

      if (repeatsEmptyAnchor)
      {
        onSourceChanged();
        return;
      }

      ++positionRevisionCounter;
      commitObservedPositionAnchor = commitDepth != 0;
      onSourceChanged();
    }

    void onRevealTrackRequested(PlaybackRevealTrackRequest request)
    {
      if (commitDepth != 0)
      {
        pendingRevealRequests.push_back(std::move(request));
        return;
      }

      emitSafely(revealTrackSignal, request);
    }

    void scheduleDeferredPublish()
    {
      if (publishScheduled)
      {
        return;
      }

      publishScheduled = true;
      auto const weakControlPtr = std::weak_ptr<DeferredControl>{deferredControlPtr};

      try
      {
        executor.defer(
          [weakControlPtr]
          {
            auto const controlPtr = weakControlPtr.lock();

            if (controlPtr == nullptr || controlPtr->owner == nullptr)
            {
              return;
            }

            controlPtr->owner->publishScheduled = false;

            if (controlPtr->owner->closed)
            {
              return;
            }

            controlPtr->owner->publishNow();
          });
      }
      catch (...)
      {
        publishScheduled = false;
        throw;
      }
    }

    void beginCommit()
    {
      if (commitDepth == 0)
      {
        auto const& nowPlaying = transport.state().nowPlaying;
        commitStartTrackId = nowPlaying.trackId;
        commitStartSourceListId = nowPlaying.sourceListId;
        commitObservedPositionAnchor = false;
      }

      ++commitDepth;
    }

    void settleCommit(bool const forcesPositionAnchor)
    {
      if (commitDepth > 1)
      {
        return;
      }

      auto const& nowPlaying = transport.state().nowPlaying;
      auto const subjectChanged =
        nowPlaying.trackId != commitStartTrackId || nowPlaying.sourceListId != commitStartSourceListId;

      if (!commitObservedPositionAnchor && (forcesPositionAnchor || subjectChanged))
      {
        ++positionRevisionCounter;
        observedAnchorTrackId = nowPlaying.trackId;
        observedAnchorSourceListId = nowPlaying.sourceListId;
      }

      publishNow();
      publishPendingEvents();
    }

    void closeCommit() noexcept { --commitDepth; }

    void publishNow()
    {
      auto const& nowPlaying = transport.state().nowPlaying;
      auto const unobservedSubjectChange =
        nowPlaying.trackId != observedAnchorTrackId || nowPlaying.sourceListId != observedAnchorSourceListId;

      if (pendingExternalPositionAnchor || unobservedSubjectChange)
      {
        ++positionRevisionCounter;
        observedAnchorTrackId = nowPlaying.trackId;
        observedAnchorSourceListId = nowPlaying.sourceListId;
        pendingExternalPositionAnchor = false;
      }

      auto content = composeContent();

      if (content == lastSnapshot)
      {
        return;
      }

      lastSnapshot = std::move(content);
      emitSafely(snapshotSignal, lastSnapshot);
    }

    void publishPendingEvents()
    {
      auto seekPreviews = std::exchange(pendingSeekPreviews, {});
      auto revealRequests = std::exchange(pendingRevealRequests, {});

      for (auto const elapsed : seekPreviews)
      {
        emitSafely(seekPreviewSignal, elapsed);
      }

      for (auto const& request : revealRequests)
      {
        emitSafely(revealTrackSignal, request);
      }
    }

    template<typename... Args>
    void emitSafely(async::Signal<Args...>& signal, std::type_identity_t<Args>... args)
    {
      ++publicationDepth;

      try
      {
        signal.emit(args...);
      }
      catch (...) // NOLINT(bugprone-empty-catch) -- public observer faults are isolated
      {
        // Runtime observation is exception-contained by the boundary. Every
        // still-connected observer has already run because Signal preserves
        // the first exception until delivery completes.
      }

      --publicationDepth;

      if (!insideBoundary())
      {
        scheduleCommandDrain();
      }
    }

    void shutdown() noexcept
    {
      if (closed)
      {
        return;
      }

      closed = true;
      latestInvalidatingGeneration = ++commandGenerationCounter;
      queuedCommands.clear();
      subscriptions.clear();
      deferredControlPtr->owner = nullptr;
    }

    PlaybackSnapshot composeContent() const
    {
      auto const& transportState = transport.state();
      auto const& successionState = succession.state();

      return PlaybackSnapshot{
        .transport =
          PlaybackTransportSnapshot{
            .transport = transportState.transport,
            .ready = transportState.ready,
            .positionRevision = PlaybackPositionRevision{.value = positionRevisionCounter},
            .finalSeekRevision = PlaybackFinalSeekRevision{.value = finalSeekRevisionCounter},
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
      };
    }

    async::Executor& executor;
    PlaybackTransport& transport;
    PlaybackSuccession& succession;

    async::Signal<PlaybackSnapshot const&> snapshotSignal;
    async::Signal<std::chrono::milliseconds> seekPreviewSignal;
    async::Signal<PlaybackRevealTrackRequest const&> revealTrackSignal;

    std::vector<async::Subscription> subscriptions;
    std::shared_ptr<DeferredControl> deferredControlPtr;

    // The commit pump is callback-executor-confined. Either boundary depth
    // blocks synchronous command execution; the queue plus its scheduled marker
    // forms one FIFO backlog. An outer commit owns its anchor evidence and
    // transient events until settlement. Each deferred
    // marker represents at most one task; shutdown revokes the task's owner link.
    std::deque<QueuedCommand> queuedCommands;
    std::vector<std::chrono::milliseconds> pendingSeekPreviews;
    std::vector<PlaybackRevealTrackRequest> pendingRevealRequests;
    std::uint64_t positionRevisionCounter = 0;
    std::uint64_t finalSeekRevisionCounter = 0;
    std::uint64_t commandGenerationCounter = 0;
    std::uint64_t latestInvalidatingGeneration = 0;
    TrackId commitStartTrackId = kInvalidTrackId;
    ListId commitStartSourceListId = kInvalidListId;
    PlaybackSnapshot lastSnapshot{};
    TrackId observedAnchorTrackId = kInvalidTrackId;
    ListId observedAnchorSourceListId = kInvalidListId;
    std::size_t commitDepth = 0;
    std::size_t publicationDepth = 0;
    bool commitObservedPositionAnchor = false;
    bool pendingExternalPositionAnchor = false;
    bool publishScheduled = false;
    bool commandDrainScheduled = false;
    bool closed = false;
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

  PlaybackSnapshot const& PlaybackService::snapshot() const
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

  bool PlaybackService::runSynchronousCommand(std::move_only_function<bool()> operation)
  {
    return _implPtr->runSynchronousCommand(std::move(operation));
  }

  void PlaybackService::shutdown() noexcept
  {
    _implPtr->shutdown();
  }
} // namespace ao::rt
