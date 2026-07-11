// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackCursorSession.h"
#include "runtime/playback/PlaybackRestartDeadline.h"
#include "runtime/playback/ProjectionAnchor.h"
#include "runtime/playback/ShuffleHistory.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Transport.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/Signal.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/utility/ThreadName.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <source_location>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    constexpr std::size_t kMaxConsecutivePlaybackFailures = 3;

    PlaybackRestartDeadline::TimePoint playbackRestartNow()
    {
      return PlaybackRestartDeadline::Clock::now();
    }

    class RestartDeadlineScheduler final : public PlaybackRestartDeadline::Scheduler
    {
    public:
      ~RestartDeadlineScheduler() override { cancel(); }

      RestartDeadlineScheduler() = default;
      RestartDeadlineScheduler(RestartDeadlineScheduler const&) = delete;
      RestartDeadlineScheduler& operator=(RestartDeadlineScheduler const&) = delete;
      RestartDeadlineScheduler(RestartDeadlineScheduler&&) = delete;
      RestartDeadlineScheduler& operator=(RestartDeadlineScheduler&&) = delete;

      void schedule(PlaybackRestartDeadline::TimePoint const deadline,
                    PlaybackRestartDeadline::DeadlineCallback callback) override
      {
        cancel();
        _thread = std::jthread{[deadline, callback = std::move(callback)](std::stop_token stopToken) mutable
                               {
                                 setCurrentThreadName("PlaybackRestart");
                                 auto mutex = std::mutex{};
                                 auto wake = std::condition_variable_any{};
                                 auto lock = std::unique_lock{mutex};
                                 auto const stopped = wake.wait_until(lock, stopToken, deadline, [] { return false; });

                                 if (!stopped && !stopToken.stop_requested())
                                 {
                                   callback();
                                 }
                               }};
      }

      void cancel() noexcept override
      {
        if (_thread.joinable())
        {
          _thread.request_stop();
          _thread.join();
        }
      }

    private:
      std::jthread _thread;
    };

    Result<PlaybackService::PlaybackRequest> playbackRequestForTrack(library::MusicLibrary& library,
                                                                     TrackId const trackId)
    {
      auto const transaction = library.readTransaction();
      auto reader = library.tracks().reader(transaction);
      auto const optView = reader.get(trackId, library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        return makeError(Error::Code::NotFound, "track not found");
      }

      auto const& view = *optView;
      auto const metadata = view.metadata();
      auto const property = view.property();
      auto const uri = std::filesystem::path{property.uri()};
      auto const optFilePath =
        uri.empty() ? std::optional<std::filesystem::path>{}
                    : std::optional<std::filesystem::path>{
                        uri.is_absolute() ? uri.lexically_normal() : (library.rootPath() / uri).lexically_normal()};
      auto request = PlaybackService::PlaybackRequest{
        .item =
          NowPlayingInfo{
            .trackId = trackId,
            .coverArtId = view.coverArt()
                            .primary()
                            .transform([](library::CoverArt const cover) { return cover.resourceId; })
                            .value_or(kInvalidResourceId),
            .title = std::string{metadata.title()},
            .artist = std::string{library.dictionary().getOrDefault(metadata.artistId())},
            .album = std::string{library.dictionary().getOrDefault(metadata.albumId())},
          },
        .input =
          audio::PlaybackInput{
            .duration = property.duration(),
            .sampleRateHint = property.sampleRate().raw(),
            .channelsHint = property.channels().raw(),
            .bitDepthHint = property.bitDepth().raw(),
          },
      };

      if (optFilePath)
      {
        request.input.filePath = *optFilePath;
      }

      return request;
    }

    bool isRecoverableTrackFailure(PlaybackFailure const& failure) noexcept
    {
      return failure.recoverable &&
             (failure.kind == PlaybackFailureKind::TrackOpen || failure.kind == PlaybackFailureKind::Decode);
    }

    std::string skippedTracksMessage(std::size_t const count)
    {
      return count == 1 ? std::string{"Skipped 1 unplayable track"}
                        : "Skipped " + std::to_string(count) + " unplayable tracks";
    }

    std::string playbackStoppedMessage()
    {
      return "Playback stopped after " + std::to_string(kMaxConsecutivePlaybackFailures) + " unplayable tracks";
    }

    std::string terminalTrackFailureMessage(PlaybackFailure const& failure)
    {
      auto message = failure.title.empty() ? std::string{"Playback stopped"} : "Playback stopped for " + failure.title;

      if (!failure.error.message.empty())
      {
        message += ": " + failure.error.message;
      }

      return message;
    }

    [[noreturn]] void failExecutorAffinity(std::source_location const& location)
    {
      APP_LOG_CRITICAL("PlaybackSequenceService thread-affinity violation: '{}' invoked off the executor thread "
                       "({}:{})",
                       location.function_name(),
                       location.file_name(),
                       location.line());

      if (auto const& loggerPtr = Log::appLogger(); loggerPtr)
      {
        loggerPtr->flush();
      }

      std::abort();
    }

    template<typename Publish>
    void publishSequenceObserverSafely(std::string_view const eventName, Publish&& publish) noexcept
    {
      try
      {
        std::forward<Publish>(publish)();
      }
      catch (std::exception const& error)
      {
        try
        {
          APP_LOG_ERROR("Playback sequence {} observer threw: {}", eventName, error.what());
        }
        catch (...) // NOLINT(bugprone-empty-catch) -- observer containment must remain noexcept
        {
        }
      }
      catch (...)
      {
        try
        {
          APP_LOG_ERROR("Playback sequence {} observer threw an unknown exception", eventName);
        }
        catch (...) // NOLINT(bugprone-empty-catch) -- observer containment must remain noexcept
        {
        }
      }
    }

    template<typename Step>
    void runAcceptedRestoreStepSafely(std::string_view const stepName, Step&& step) noexcept
    {
      try
      {
        std::forward<Step>(step)();
      }
      catch (std::exception const& error)
      {
        try
        {
          APP_LOG_ERROR("Playback restore {} failed after acceptance: {}", stepName, error.what());
        }
        catch (...) // NOLINT(bugprone-empty-catch) -- accepted restore must remain noexcept
        {
        }
      }
      catch (...)
      {
        try
        {
          APP_LOG_ERROR("Playback restore {} failed after acceptance with an unknown exception", stepName);
        }
        catch (...) // NOLINT(bugprone-empty-catch) -- accepted restore must remain noexcept
        {
        }
      }
    }
  } // namespace

  struct PlaybackSequenceService::Impl final : std::enable_shared_from_this<Impl>
  {
    enum class NavigationDirection : std::uint8_t
    {
      Forward,
      Backward,
    };

    struct RestorableCursorSnapshot final
    {
      PlaybackLaunchContext launchContext;
      TrackId currentTrackId = kInvalidTrackId;
      ProjectionAnchor anchor;
    };

    class [[nodiscard]] AcceptanceTransaction final
    {
    public:
      explicit AcceptanceTransaction(Impl& owner) noexcept
        : _owner{owner}
      {
        ++_owner.acceptanceDepth;
      }

      ~AcceptanceTransaction() { --_owner.acceptanceDepth; }

      AcceptanceTransaction(AcceptanceTransaction const&) = delete;
      AcceptanceTransaction& operator=(AcceptanceTransaction const&) = delete;
      AcceptanceTransaction(AcceptanceTransaction&&) = delete;
      AcceptanceTransaction& operator=(AcceptanceTransaction&&) = delete;

    private:
      Impl& _owner;
    };

    class [[nodiscard]] SessionInstallationTransaction final
    {
    public:
      explicit SessionInstallationTransaction(Impl& owner) noexcept
        : _owner{owner}
      {
        ++_owner.sessionInstallationDepth;
      }

      ~SessionInstallationTransaction() { --_owner.sessionInstallationDepth; }

      SessionInstallationTransaction(SessionInstallationTransaction const&) = delete;
      SessionInstallationTransaction& operator=(SessionInstallationTransaction const&) = delete;
      SessionInstallationTransaction(SessionInstallationTransaction&&) = delete;
      SessionInstallationTransaction& operator=(SessionInstallationTransaction&&) = delete;

    private:
      Impl& _owner;
    };

    class [[nodiscard]] RestoreTransaction final
    {
    public:
      explicit RestoreTransaction(Impl& owner) noexcept
        : _owner{owner}
      {
        ++_owner.restoreDepth;
      }

      ~RestoreTransaction() { --_owner.restoreDepth; }

      RestoreTransaction(RestoreTransaction const&) = delete;
      RestoreTransaction& operator=(RestoreTransaction const&) = delete;
      RestoreTransaction(RestoreTransaction&&) = delete;
      RestoreTransaction& operator=(RestoreTransaction&&) = delete;

    private:
      Impl& _owner;
    };

    Impl(async::Executor& executor,
         ViewService& views,
         TrackSourceCache& sources,
         library::MusicLibrary& library,
         PlaybackService& playback,
         NotificationService& notifications)
      : executor{executor}
      , views{views}
      , sources{sources}
      , library{library}
      , playback{playback}
      , notifications{notifications}
      , restartDeadline{executor,
                        restartScheduler,
                        playbackRestartNow,
                        [this] { return this->playback.elapsed(); },
                        [this](bool const available) { handleRestartAvailabilityChanged(available); }}
    {
    }

    ~Impl() { shutdown(); }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void ensureOnExecutor(std::source_location location = std::source_location::current()) const
    {
      if (!executor.isCurrent()) [[unlikely]]
      {
        failExecutorAffinity(location);
      }
    }

    ShuffleHistory::CandidateChooser makeCandidateChooser()
    {
      return [generator = std::mt19937{std::random_device{}()}](std::span<TrackId const> candidates) mutable
      {
        if (candidates.empty())
        {
          return kInvalidTrackId;
        }

        auto distribution = std::uniform_int_distribution<std::size_t>{0, candidates.size() - 1};
        return candidates[distribution(generator)];
      };
    }

    bool isClosing() const noexcept { return closing.load(std::memory_order_acquire); }
    bool isAcceptingStart() const noexcept { return acceptanceDepth != 0; }
    bool isInstallingSession() const noexcept { return sessionInstallationDepth != 0; }
    bool isRestoringSession() const noexcept { return restoreDepth != 0; }
    bool blocksSequenceCommand() const { return isAcceptingStart() || playback.isPublishingAcceptedStart(); }

    static PlaybackSequenceSourceState sourceStateFor(PlaybackCursorSession const* session) noexcept
    {
      if (session == nullptr)
      {
        return PlaybackSequenceSourceState::Inactive;
      }

      return session->cursor().sourceState() == PlaybackCursor::SourceState::Live
               ? PlaybackSequenceSourceState::Live
               : PlaybackSequenceSourceState::Invalidated;
    }

    static bool sameSemanticTuple(PlaybackSequenceState const& lhs, PlaybackSequenceState const& rhs) noexcept
    {
      return lhs.sourceState == rhs.sourceState && lhs.currentTrackId == rhs.currentTrackId &&
             lhs.hasNext == rhs.hasNext && lhs.hasPrevious == rhs.hasPrevious &&
             lhs.optResolvedSuccessor == rhs.optResolvedSuccessor;
    }

    void synchronizeState(bool const containObserverExceptions = false)
    {
      auto next = PlaybackSequenceState{
        .sourceState = sourceStateFor(sessionPtr.get()),
        .shuffle = shuffleMode,
        .repeat = repeatMode,
        .semanticRevision = state.semanticRevision,
      };

      if (sessionPtr)
      {
        auto const& cursor = sessionPtr->cursor();
        auto const& semantic = cursor.semanticTuple();
        next.currentTrackId = semantic.currentTrackId;
        next.sourceListId = cursor.launchContext().sourceListId;
        next.hasNext = semantic.hasNext;
        next.hasPrevious = semantic.hasPrevious;
        next.optResolvedSuccessor = semantic.optResolvedSuccessor;
      }

      auto const semanticChanged = !sameSemanticTuple(state, next);

      if (semanticChanged)
      {
        next.semanticRevision = state.semanticRevision + 1;
      }

      state = std::move(next);

      if (semanticChanged && !isClosing())
      {
        if (containObserverExceptions)
        {
          publishSequenceObserverSafely("state-changed", [this] { changedSignal.emit(state); });
        }
        else
        {
          changedSignal.emit(state);
        }
      }
    }

    void captureRestorableSnapshot()
    {
      if (!sessionPtr)
      {
        return;
      }

      auto const& cursor = sessionPtr->cursor();
      optLastRestorableSnapshot = RestorableCursorSnapshot{
        .launchContext = cursor.launchContext(),
        .currentTrackId = cursor.currentTrackId(),
        .anchor = cursor.anchor(),
      };
    }

    void notifyPersistenceIntentChanged(bool const containObserverExceptions = false)
    {
      if (!isRestoringSession() && !isClosing())
      {
        if (containObserverExceptions)
        {
          publishSequenceObserverSafely(
            "persistence-intent-changed", [this] { persistenceIntentChangedSignal.emit(); });
        }
        else
        {
          persistenceIntentChangedSignal.emit();
        }
      }
    }

    void deactivateSession()
    {
      if (!sessionPtr)
      {
        return;
      }

      captureRestorableSnapshot();
      auto const optDisarmedToken = playback.clearSequencePreparedNext();
      sessionPtr->invalidatePreparedNext(optDisarmedToken);
      sessionPtr.reset();
      restartDeadline.clearSession();
      resetFailureState();
      synchronizeState();
    }

    void stopTerminal(bool postNotification)
    {
      if (!sessionPtr)
      {
        return;
      }

      captureRestorableSnapshot();
      stoppingTransport = true;

      if (auto const barrier = playback.stop(); sessionPtr)
      {
        sessionPtr->clearPreparedCoveredBy(barrier);
      }

      stoppingTransport = false;
      sessionPtr.reset();
      restartDeadline.clearSession();
      resetFailureState();
      synchronizeState();

      if (postNotification && !isClosing())
      {
        notifications.post(NotificationRequest{
          .severity = NotificationSeverity::Info,
          .message = "Playback sequence finished",
          .content = NotificationContentState{.topic = NotificationTopic::PlaybackSequence},
        });
      }
    }

    void resetFailureState() noexcept
    {
      skipNotificationId = kInvalidNotificationId;
      skippedFailureCount = 0;

      if (sessionPtr)
      {
        sessionPtr->resetConsecutiveFailureCount();
      }
    }

    void reportSkippedTrack()
    {
      ++skippedFailureCount;
      auto const message = skippedTracksMessage(skippedFailureCount);

      if (skipNotificationId == kInvalidNotificationId || !notifications.updateMessage(skipNotificationId, message))
      {
        skipNotificationId = notifications.post(NotificationRequest{
          .severity = NotificationSeverity::Warning,
          .message = message,
          .content = NotificationContentState{.topic = NotificationTopic::PlaybackSequence},
        });
      }
    }

    void reportFailureLimit()
    {
      notifications.post(NotificationRequest{
        .severity = NotificationSeverity::Error,
        .message = playbackStoppedMessage(),
        .sticky = true,
        .content = NotificationContentState{.topic = NotificationTopic::PlaybackSequence},
      });
    }

    void reportTerminalTrackFailure(PlaybackFailure const& failure)
    {
      notifications.post(NotificationRequest{
        .severity = NotificationSeverity::Error,
        .message = terminalTrackFailureMessage(failure),
        .sticky = true,
        .content = NotificationContentState{.topic = NotificationTopic::PlaybackSequence},
      });
    }

    void startObservingCurrentSession()
    {
      auto const weakSelfPtr = weak_from_this();
      sessionPtr->startObserving(
        [weakSelfPtr](PlaybackCursor::MutationEffect const effect, bool const sourceInvalidated)
        {
          if (auto const selfPtr = weakSelfPtr.lock(); selfPtr && !selfPtr->isClosing())
          {
            selfPtr->handleProjectionBatch(effect, sourceInvalidated);
          }
        });
    }

    void handleProjectionBatch(PlaybackCursor::MutationEffect const effect, bool const sourceInvalidated)
    {
      if (!sessionPtr || isInstallingSession())
      {
        return;
      }

      if (sourceInvalidated)
      {
        auto const optDisarmedToken = playback.clearSequencePreparedNext();
        sessionPtr->invalidatePreparedNext(optDisarmedToken);
        captureRestorableSnapshot();
      }
      else
      {
        reprepareNext(false);
      }

      if (effect.semanticChanged || sourceInvalidated)
      {
        synchronizeState();
      }

      if (effect.persistenceIntentChanged)
      {
        notifyPersistenceIntentChanged();
      }
    }

    bool reprepareNext(bool const force)
    {
      if (!sessionPtr)
      {
        std::ignore = playback.clearSequencePreparedNext();
        return false;
      }

      auto& session = *sessionPtr;
      auto& registry = session.preparedNextRegistry();
      auto optSuccessor = session.cursor().semanticTuple().optResolvedSuccessor;

      if (!force && optSuccessor)
      {
        if (auto const optActiveToken = registry.activeToken(); optActiveToken)
        {
          if (auto const optAnchor = registry.anchorFor(*optActiveToken);
              optAnchor && optAnchor->trackId() == *optSuccessor)
          {
            return true;
          }
        }
      }

      auto const optDisarmedToken = playback.clearSequencePreparedNext();
      registry.invalidate(optDisarmedToken);

      for (std::size_t attempt = 0; optSuccessor && attempt < kMaxConsecutivePlaybackFailures; ++attempt)
      {
        auto const successor = *optSuccessor;
        auto prepared = playback.prepareSequenceNext(successor, session.cursor().launchContext().sourceListId);

        if (prepared)
        {
          auto const optIssuedGeneration = playback.preparedNextIssuedGeneration(*prepared);

          if (!optIssuedGeneration)
          {
            auto const optAcknowledged = playback.clearSequencePreparedNext();
            registry.invalidate(optAcknowledged);
            return false;
          }

          registry.activate(
            *prepared, *optIssuedGeneration, session.anchorFor(successor, session.cursor().anchor().anchorIndex()));
          return true;
        }

        if (session.cursor().shuffleMode() != ShuffleMode::On ||
            !session.shuffleHistory().discardForwardCandidate(successor))
        {
          return false;
        }

        if (auto const effect = session.refreshSemanticState(); effect.semanticChanged)
        {
          synchronizeState(blocksSequenceCommand());
        }

        optSuccessor = session.cursor().semanticTuple().optResolvedSuccessor;

        if (optSuccessor == successor)
        {
          return false;
        }
      }

      return false;
    }

    Result<> startTrack(TrackId const trackId, ShuffleHistory::TransitionOrigin const origin)
    {
      if (!sessionPtr)
      {
        return makeError(Error::Code::InvalidState, "No active playback sequence");
      }

      auto const acceptance = AcceptanceTransaction{*this};
      auto receipt = playback.playTrack(trackId, sessionPtr->cursor().launchContext().sourceListId);

      if (!receipt)
      {
        return std::unexpected{receipt.error()};
      }

      if (isClosing())
      {
        return {};
      }

      sessionPtr->clearPreparedCoveredBy(receipt->cancellationBarrier);
      auto adopted = sessionPtr->adoptCurrent(trackId, std::nullopt, origin);

      if (!adopted)
      {
        return std::unexpected{adopted.error()};
      }

      resetFailureState();
      restartDeadline.currentTrackChanged(std::chrono::milliseconds{0}, true);
      reprepareNext(false);
      synchronizeState(true);
      notifyPersistenceIntentChanged(true);
      return {};
    }

    std::optional<TrackId> nextFailureCandidate(TrackId const failedTrackId,
                                                NavigationDirection const direction,
                                                std::span<TrackId const> attemptedTrackIds) const
    {
      if (!sessionPtr || sessionPtr->cursor().sourceState() == PlaybackCursor::SourceState::Invalidated ||
          sessionPtr->cursor().repeatMode() == RepeatMode::One)
      {
        return std::nullopt;
      }

      auto const projectionSize = sessionPtr->projectionSize();

      if (projectionSize == 0)
      {
        return std::nullopt;
      }

      auto startIndex = sessionPtr->indexOf(failedTrackId).value_or(sessionPtr->cursor().anchor().anchorIndex());

      for (std::size_t offset = 1; offset <= projectionSize; ++offset)
      {
        std::size_t index = 0;

        if (direction == NavigationDirection::Forward)
        {
          if (startIndex + offset >= projectionSize && sessionPtr->cursor().repeatMode() != RepeatMode::All)
          {
            break;
          }

          index = (startIndex + offset) % projectionSize;
        }
        else
        {
          if (offset > startIndex && sessionPtr->cursor().repeatMode() != RepeatMode::All)
          {
            break;
          }

          index = (startIndex + projectionSize - (offset % projectionSize)) % projectionSize;
        }

        if (auto const candidate = sessionPtr->trackIdAt(index); !std::ranges::contains(attemptedTrackIds, candidate))
        {
          return candidate;
        }
      }

      return std::nullopt;
    }

    // The bounded recovery walk is one state machine across sequential and shuffle origins.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    bool attemptNavigation(PlaybackCursor::CommandResolution resolution,
                           ShuffleHistory::TransitionOrigin const origin,
                           NavigationDirection const direction,
                           bool const stopWhenExhausted)
    {
      if (!sessionPtr)
      {
        return false;
      }

      if (resolution.action == PlaybackCursor::CommandAction::NoOp)
      {
        return false;
      }

      if (resolution.action == PlaybackCursor::CommandAction::Stop)
      {
        stopTerminal(stopWhenExhausted);
        return true;
      }

      auto attemptedTrackIds = std::vector<TrackId>{};
      auto candidate = resolution.trackId;
      auto candidateOrigin = origin;

      while (attemptedTrackIds.size() < kMaxConsecutivePlaybackFailures)
      {
        attemptedTrackIds.push_back(candidate);
        auto const played = startTrack(candidate,
                                       resolution.action == PlaybackCursor::CommandAction::RestartCurrent
                                         ? ShuffleHistory::TransitionOrigin::Restart
                                         : candidateOrigin);

        if (played)
        {
          return true;
        }

        reportSkippedTrack();
        auto const failureCount =
          sessionPtr ? sessionPtr->incrementConsecutiveFailureCount() : kMaxConsecutivePlaybackFailures;

        if (failureCount >= kMaxConsecutivePlaybackFailures)
        {
          reportFailureLimit();
          stopTerminal(false);
          return true;
        }

        auto optCandidate = std::optional<TrackId>{};

        if (sessionPtr && sessionPtr->cursor().shuffleMode() == ShuffleMode::On)
        {
          if (candidateOrigin == ShuffleHistory::TransitionOrigin::HistoryPrevious)
          {
            for (std::size_t retryCount = 0; retryCount < ShuffleHistory::kHistoryCapacity; ++retryCount)
            {
              auto const retry = sessionPtr->resolvePrevious();

              if (retry.action != PlaybackCursor::CommandAction::StartTrack)
              {
                break;
              }

              if (!std::ranges::contains(attemptedTrackIds, retry.trackId))
              {
                optCandidate = retry.trackId;
                break;
              }
            }
          }
          else if (direction == NavigationDirection::Forward)
          {
            std::ignore = sessionPtr->rerollShuffleForward(attemptedTrackIds);
            std::ignore = sessionPtr->refreshSemanticState();

            if (auto const retry = sessionPtr->cursor().resolveNext();
                retry.action == PlaybackCursor::CommandAction::StartTrack &&
                !std::ranges::contains(attemptedTrackIds, retry.trackId))
            {
              optCandidate = retry.trackId;
            }
          }
        }
        else
        {
          optCandidate = nextFailureCandidate(candidate, direction, attemptedTrackIds);
        }

        if (!optCandidate)
        {
          if (sessionPtr)
          {
            if (auto const effect = sessionPtr->refreshSemanticState(); effect.semanticChanged)
            {
              synchronizeState();
            }
          }

          if (stopWhenExhausted)
          {
            stopTerminal(false);
          }

          return false;
        }

        candidate = *optCandidate;
        resolution.action = PlaybackCursor::CommandAction::StartTrack;

        if (candidateOrigin != ShuffleHistory::TransitionOrigin::HistoryPrevious)
        {
          candidateOrigin = direction == NavigationDirection::Forward
                              ? ShuffleHistory::TransitionOrigin::Forward
                              : ShuffleHistory::TransitionOrigin::SequentialPrevious;
        }
      }

      return false;
    }

    void advanceNatural()
    {
      if (!sessionPtr)
      {
        return;
      }

      std::ignore = attemptNavigation(sessionPtr->cursor().resolveNaturalAdvance(),
                                      ShuffleHistory::TransitionOrigin::Forward,
                                      NavigationDirection::Forward,
                                      true);
    }

    void handleNowPlayingChanged(PlaybackService::NowPlayingChanged const& event)
    {
      if (isAcceptingStart() || !sessionPtr || event.trackId == kInvalidTrackId)
      {
        return;
      }

      if (event.sourceListId != sessionPtr->cursor().launchContext().sourceListId)
      {
        if (!event.optPreparedNextToken)
        {
          deactivateSession();
        }

        return;
      }

      auto adopted =
        sessionPtr->adoptCurrent(event.trackId, event.optPreparedNextToken, ShuffleHistory::TransitionOrigin::Forward);

      if (!adopted)
      {
        return;
      }

      resetFailureState();
      restartDeadline.currentTrackChanged(std::chrono::milliseconds{0}, true);
      reprepareNext(false);
      synchronizeState(blocksSequenceCommand());
      notifyPersistenceIntentChanged(blocksSequenceCommand());
    }

    PlaybackFailureDisposition handlePlaybackFailure(PlaybackFailure const& failure)
    {
      if (!sessionPtr || failure.sourceListId != sessionPtr->cursor().launchContext().sourceListId)
      {
        return PlaybackFailureDisposition::Unhandled;
      }

      if (failure.optPreparedNextToken)
      {
        auto& registry = sessionPtr->preparedNextRegistry();

        if (!registry.contains(*failure.optPreparedNextToken))
        {
          return PlaybackFailureDisposition::Unhandled;
        }

        std::ignore = registry.acknowledgeDisarm(*failure.optPreparedNextToken);
        sessionPtr->shuffleHistory().discardForwardCandidate(failure.trackId);
        std::ignore = sessionPtr->refreshSemanticState();
        reprepareNext(false);
        synchronizeState();
        return PlaybackFailureDisposition::Recovered;
      }

      if (failure.trackId != sessionPtr->cursor().currentTrackId())
      {
        return PlaybackFailureDisposition::Unhandled;
      }

      if (!isRecoverableTrackFailure(failure) ||
          sessionPtr->cursor().sourceState() == PlaybackCursor::SourceState::Invalidated)
      {
        if (failure.kind == PlaybackFailureKind::TrackOpen || failure.kind == PlaybackFailureKind::Decode)
        {
          reportTerminalTrackFailure(failure);
        }

        stopTerminal(false);
        return PlaybackFailureDisposition::Stopped;
      }

      auto const failureCount = sessionPtr->incrementConsecutiveFailureCount();

      if (failureCount >= kMaxConsecutivePlaybackFailures)
      {
        reportFailureLimit();
        stopTerminal(false);
        return PlaybackFailureDisposition::Stopped;
      }

      reportSkippedTrack();
      auto const recovered = attemptNavigation(sessionPtr->cursor().resolveNext(),
                                               ShuffleHistory::TransitionOrigin::Forward,
                                               NavigationDirection::Forward,
                                               true);
      return recovered && sessionPtr ? PlaybackFailureDisposition::Recovered : PlaybackFailureDisposition::Stopped;
    }

    void handleRestartAvailabilityChanged(bool const available)
    {
      if (!sessionPtr || isClosing())
      {
        return;
      }

      auto const effect = sessionPtr->setPreviousRestartAvailable(available);

      if (effect.semanticChanged)
      {
        synchronizeState(blocksSequenceCommand());
      }
    }

    void start(std::shared_ptr<Impl> const& selfPtr)
    {
      auto const weakSelfPtr = std::weak_ptr<Impl>{selfPtr};
      playback.bindPlaybackFailureRecovery(
        [weakSelfPtr](PlaybackFailure const& failure)
        {
          if (auto const selfPtr = weakSelfPtr.lock(); selfPtr && !selfPtr->isClosing())
          {
            return selfPtr->handlePlaybackFailure(failure);
          }

          return PlaybackFailureDisposition::Unhandled;
        });
      idleSubscription = playback.onIdle(
        [weakSelfPtr]
        {
          if (auto const selfPtr = weakSelfPtr.lock(); selfPtr && !selfPtr->isClosing() && selfPtr->sessionPtr &&
                                                       !selfPtr->stoppingTransport &&
                                                       selfPtr->playback.state().transport == audio::Transport::Idle)
          {
            selfPtr->advanceNatural();
          }
        });
      nowPlayingSubscription = playback.onNowPlayingChanged(
        [weakSelfPtr](PlaybackService::NowPlayingChanged const& event)
        {
          if (auto const selfPtr = weakSelfPtr.lock(); selfPtr && !selfPtr->isClosing())
          {
            selfPtr->handleNowPlayingChanged(event);
          }
        });
      outputSubscription = playback.onOutputDeviceChanged(
        [weakSelfPtr](OutputDeviceSelection const&)
        {
          if (auto const selfPtr = weakSelfPtr.lock(); selfPtr && !selfPtr->isClosing())
          {
            selfPtr->reprepareNext(true);
          }
        });
      seekSubscription = playback.onSeekUpdate(
        [weakSelfPtr](PlaybackService::SeekUpdate const& event)
        {
          if (auto const selfPtr = weakSelfPtr.lock();
              selfPtr && !selfPtr->isClosing() && selfPtr->sessionPtr && event.mode == PlaybackService::SeekMode::Final)
          {
            selfPtr->restartDeadline.seek(selfPtr->playback.elapsed());
            selfPtr->reprepareNext(true);
          }
        });
      startedSubscription = playback.onStarted(
        [weakSelfPtr]
        {
          if (auto const selfPtr = weakSelfPtr.lock();
              selfPtr && !selfPtr->isClosing() && selfPtr->sessionPtr && !selfPtr->isAcceptingStart())
          {
            selfPtr->restartDeadline.resume(selfPtr->playback.elapsed());
          }
        });
      pausedSubscription = playback.onPaused(
        [weakSelfPtr]
        {
          if (auto const selfPtr = weakSelfPtr.lock(); selfPtr && !selfPtr->isClosing() && selfPtr->sessionPtr)
          {
            selfPtr->restartDeadline.pause(selfPtr->playback.elapsed());
          }
        });
      stoppedSubscription = playback.onStopped(
        [weakSelfPtr]
        {
          if (auto const selfPtr = weakSelfPtr.lock(); selfPtr && !selfPtr->isClosing() && !selfPtr->stoppingTransport)
          {
            selfPtr->deactivateSession();
          }
        });
    }

    void shutdown() noexcept
    {
      if (closing.exchange(true, std::memory_order_acq_rel))
      {
        return;
      }

      playback.unbindPlaybackFailureRecovery();
      idleSubscription.reset();
      nowPlayingSubscription.reset();
      outputSubscription.reset();
      seekSubscription.reset();
      startedSubscription.reset();
      pausedSubscription.reset();
      stoppedSubscription.reset();
      restartDeadline.shutdown();
      sessionPtr.reset();
      changedSignal.disconnectAll();
      shuffleModeChangedSignal.disconnectAll();
      repeatModeChangedSignal.disconnectAll();
      persistenceIntentChangedSignal.disconnectAll();
    }

    async::Executor& executor;
    ViewService& views;
    TrackSourceCache& sources;
    library::MusicLibrary& library;
    PlaybackService& playback;
    NotificationService& notifications;
    PlaybackSequenceState state{};
    ShuffleMode shuffleMode = ShuffleMode::Off;
    RepeatMode repeatMode = RepeatMode::Off;
    std::unique_ptr<PlaybackCursorSession> sessionPtr;
    std::optional<RestorableCursorSnapshot> optLastRestorableSnapshot;
    Signal<PlaybackSequenceState const&> changedSignal;
    Signal<PlaybackSequenceService::ShuffleModeChanged const&> shuffleModeChangedSignal;
    Signal<PlaybackSequenceService::RepeatModeChanged const&> repeatModeChangedSignal;
    Signal<> persistenceIntentChangedSignal;
    RestartDeadlineScheduler restartScheduler;
    PlaybackRestartDeadline restartDeadline;
    Subscription idleSubscription;
    Subscription nowPlayingSubscription;
    Subscription outputSubscription;
    Subscription seekSubscription;
    Subscription startedSubscription;
    Subscription pausedSubscription;
    Subscription stoppedSubscription;
    NotificationId skipNotificationId = kInvalidNotificationId;
    std::size_t skippedFailureCount = 0;
    std::atomic_bool closing{false};
    std::size_t acceptanceDepth = 0;
    std::size_t sessionInstallationDepth = 0;
    bool stoppingTransport = false;
    std::size_t restoreDepth = 0;
  };

  PlaybackSequenceService::PlaybackSequenceService(async::Executor& executor,
                                                   ViewService& views,
                                                   TrackSourceCache& sources,
                                                   library::MusicLibrary& library,
                                                   PlaybackService& playback,
                                                   NotificationService& notifications)
    : _implPtr{std::make_shared<Impl>(executor, views, sources, library, playback, notifications)}
  {
    _implPtr->start(_implPtr);
  }

  PlaybackSequenceService::~PlaybackSequenceService()
  {
    if (auto const implPtr = _implPtr; implPtr)
    {
      implPtr->shutdown();
    }
  }

  Result<> PlaybackSequenceService::playFromView(ViewId const viewId, TrackId const startTrackId)
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (implPtr->blocksSequenceCommand())
    {
      return makeError(Error::Code::InvalidState, "Playback sequence is accepting another start");
    }

    auto const acceptance = Impl::AcceptanceTransaction{*implPtr};

    auto context = implPtr->views.capturePlaybackLaunchContext(viewId);

    if (!context)
    {
      return std::unexpected{context.error()};
    }

    auto candidateSession = PlaybackCursorSession::create(*context,
                                                          startTrackId,
                                                          implPtr->sources,
                                                          implPtr->library,
                                                          implPtr->repeatMode,
                                                          implPtr->shuffleMode,
                                                          implPtr->makeCandidateChooser());

    if (!candidateSession)
    {
      return std::unexpected{candidateSession.error()};
    }

    auto request = playbackRequestForTrack(implPtr->library, startTrackId);

    if (!request)
    {
      return std::unexpected{request.error()};
    }

    auto preparedStart = implPtr->playback.stagePlayback(*request, context->sourceListId);

    if (!preparedStart)
    {
      return std::unexpected{preparedStart.error()};
    }

    if (implPtr->isClosing())
    {
      return makeError(Error::Code::InvalidState, "Playback sequence closed during preparation");
    }

    auto receipt = implPtr->playback.commitPlayback(std::move(*preparedStart));

    if (!receipt)
    {
      return std::unexpected{receipt.error()};
    }

    if (implPtr->isClosing())
    {
      return {};
    }

    if (implPtr->sessionPtr)
    {
      implPtr->sessionPtr->clearPreparedCoveredBy(receipt->cancellationBarrier);
      implPtr->captureRestorableSnapshot();
    }

    implPtr->sessionPtr = std::move(*candidateSession);

    {
      auto const installation = Impl::SessionInstallationTransaction{*implPtr};
      implPtr->startObservingCurrentSession();
    }

    implPtr->resetFailureState();
    implPtr->restartDeadline.replaceSession(std::chrono::milliseconds{0}, true);
    implPtr->reprepareNext(false);
    implPtr->synchronizeState(true);
    implPtr->notifyPersistenceIntentChanged(true);
    return {};
  }

  bool PlaybackSequenceService::hasNext() const
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    return implPtr->state.hasNext;
  }

  bool PlaybackSequenceService::hasPrevious() const
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    return implPtr->state.hasPrevious;
  }

  void PlaybackSequenceService::next()
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (implPtr->blocksSequenceCommand() || !implPtr->sessionPtr)
    {
      return;
    }

    implPtr->resetFailureState();
    std::ignore = implPtr->attemptNavigation(implPtr->sessionPtr->cursor().resolveNext(),
                                             ShuffleHistory::TransitionOrigin::Forward,
                                             Impl::NavigationDirection::Forward,
                                             true);
  }

  void PlaybackSequenceService::previous()
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (implPtr->blocksSequenceCommand() || !implPtr->sessionPtr)
    {
      return;
    }

    implPtr->resetFailureState();
    auto const shufflePrevious = implPtr->sessionPtr->cursor().shuffleMode() == ShuffleMode::On &&
                                 !implPtr->sessionPtr->cursor().previousRestartAvailable();
    auto const resolution = implPtr->sessionPtr->resolvePrevious();

    if (shufflePrevious && resolution.action == PlaybackCursor::CommandAction::NoOp)
    {
      auto const effect = implPtr->sessionPtr->refreshSemanticState();

      if (effect.semanticChanged)
      {
        implPtr->synchronizeState();
      }
    }

    std::ignore = implPtr->attemptNavigation(resolution,
                                             shufflePrevious ? ShuffleHistory::TransitionOrigin::HistoryPrevious
                                                             : ShuffleHistory::TransitionOrigin::SequentialPrevious,
                                             Impl::NavigationDirection::Backward,
                                             false);
  }

  void PlaybackSequenceService::clear()
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (implPtr->blocksSequenceCommand())
    {
      return;
    }

    implPtr->deactivateSession();
  }

  void PlaybackSequenceService::setShuffleMode(ShuffleMode const mode)
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (implPtr->blocksSequenceCommand() || implPtr->shuffleMode == mode)
    {
      return;
    }

    implPtr->shuffleMode = mode;

    if (implPtr->sessionPtr)
    {
      std::ignore = implPtr->sessionPtr->setShuffleMode(mode);
      implPtr->reprepareNext(true);
    }

    implPtr->synchronizeState();
    implPtr->shuffleModeChangedSignal.emit(ShuffleModeChanged{.mode = mode});
    implPtr->notifyPersistenceIntentChanged();
  }

  void PlaybackSequenceService::setRepeatMode(RepeatMode const mode)
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (implPtr->blocksSequenceCommand() || implPtr->repeatMode == mode)
    {
      return;
    }

    implPtr->repeatMode = mode;

    if (implPtr->sessionPtr)
    {
      std::ignore = implPtr->sessionPtr->setRepeatMode(mode);
      implPtr->reprepareNext(true);
    }

    implPtr->synchronizeState();
    implPtr->repeatModeChangedSignal.emit(RepeatModeChanged{.mode = mode});
    implPtr->notifyPersistenceIntentChanged();
  }

  PlaybackSequenceState const& PlaybackSequenceService::state() const
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    return implPtr->state;
  }

  Subscription PlaybackSequenceService::onChanged(std::move_only_function<void(PlaybackSequenceState const&)> handler)
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    return implPtr->changedSignal.connect(std::move(handler));
  }

  Subscription PlaybackSequenceService::onShuffleModeChanged(
    std::move_only_function<void(ShuffleModeChanged const&)> handler)
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    return implPtr->shuffleModeChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackSequenceService::onRepeatModeChanged(
    std::move_only_function<void(RepeatModeChanged const&)> handler)
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    return implPtr->repeatModeChangedSignal.connect(std::move(handler));
  }

  bool PlaybackSequenceService::hasActivePlaybackSession() const
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    return implPtr->sessionPtr != nullptr;
  }

  bool PlaybackSequenceService::capturePlaybackSessionSnapshot(PlaybackLaunchContext& launchContext,
                                                               TrackId& currentTrackId,
                                                               std::size_t& anchorIndex) const
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (implPtr->sessionPtr)
    {
      auto const& cursor = implPtr->sessionPtr->cursor();
      launchContext = cursor.launchContext();
      currentTrackId = cursor.currentTrackId();
      anchorIndex = cursor.anchor().anchorIndex();
      return true;
    }

    if (!implPtr->optLastRestorableSnapshot)
    {
      return false;
    }

    launchContext = implPtr->optLastRestorableSnapshot->launchContext;
    currentTrackId = implPtr->optLastRestorableSnapshot->currentTrackId;
    anchorIndex = implPtr->optLastRestorableSnapshot->anchor.anchorIndex();
    return true;
  }

  Result<std::unique_ptr<PlaybackCursorSession>> PlaybackSequenceService::preparePlaybackSessionRestore(
    PlaybackLaunchContext launchContext,
    TrackId const currentTrackId,
    std::size_t const anchorIndex,
    ShuffleMode const restoredShuffleMode,
    RepeatMode const restoredRepeatMode)
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    return PlaybackCursorSession::createForRestore(std::move(launchContext),
                                                   currentTrackId,
                                                   anchorIndex,
                                                   implPtr->sources,
                                                   implPtr->library,
                                                   restoredRepeatMode,
                                                   restoredShuffleMode,
                                                   implPtr->makeCandidateChooser());
  }

  // Executor-affinity failure deliberately terminates this otherwise contained noexcept commit.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  void PlaybackSequenceService::commitPlaybackSessionRestore(std::unique_ptr<PlaybackCursorSession> sessionPtr,
                                                             ShuffleMode const restoredShuffleMode,
                                                             RepeatMode const restoredRepeatMode,
                                                             std::chrono::milliseconds const elapsed) noexcept
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();

    if (!sessionPtr)
    {
      return;
    }

    auto const acceptance = Impl::AcceptanceTransaction{*implPtr};
    auto const restoration = Impl::RestoreTransaction{*implPtr};
    implPtr->sessionPtr = std::move(sessionPtr);
    implPtr->optLastRestorableSnapshot.reset();
    implPtr->shuffleMode = restoredShuffleMode;
    implPtr->repeatMode = restoredRepeatMode;
    implPtr->resetFailureState();
    runAcceptedRestoreStepSafely("projection observation",
                                 [&]
                                 {
                                   auto const installation = Impl::SessionInstallationTransaction{*implPtr};
                                   implPtr->startObservingCurrentSession();
                                 });
    runAcceptedRestoreStepSafely("restart deadline", [&] { implPtr->restartDeadline.replaceSession(elapsed, false); });
    runAcceptedRestoreStepSafely("prepared successor", [&] { implPtr->reprepareNext(false); });
    runAcceptedRestoreStepSafely("state publication", [&] { implPtr->synchronizeState(true); });
    publishSequenceObserverSafely(
      "shuffle-mode-changed",
      [&] { implPtr->shuffleModeChangedSignal.emit(ShuffleModeChanged{.mode = restoredShuffleMode}); });

    if (!implPtr->isClosing())
    {
      publishSequenceObserverSafely(
        "repeat-mode-changed",
        [&] { implPtr->repeatModeChangedSignal.emit(RepeatModeChanged{.mode = restoredRepeatMode}); });
    }
  }

  void PlaybackSequenceService::forgetPlaybackSessionSnapshot()
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    implPtr->optLastRestorableSnapshot.reset();
  }

  Subscription PlaybackSequenceService::onPersistenceIntentChanged(std::move_only_function<void()> handler)
  {
    auto const implPtr = _implPtr;
    implPtr->ensureOnExecutor();
    return implPtr->persistenceIntentChangedSignal.connect(std::move(handler));
  }
} // namespace ao::rt
