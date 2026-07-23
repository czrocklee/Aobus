// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackSuccession.h"

#include "runtime/playback/PlaybackCursorSession.h"
#include "runtime/playback/PlaybackRestartDeadline.h"
#include "runtime/playback/PlaybackTransport.h"
#include "runtime/playback/ProjectionAnchor.h"
#include "runtime/playback/ShuffleHistory.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/Signal.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Transport.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/PreparedPlayback.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    constexpr std::size_t kMaxConsecutivePlaybackFailures = 3;

    Result<PlaybackTransport::PlaybackRequest> playbackRequestForTrack(library::MusicLibrary const& library,
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
      auto optFilePath = std::optional<std::filesystem::path>{};

      if (auto const uriText = property.uri(); !uriText.empty())
      {
        auto uri = library::LibraryUri::parse(uriText);

        if (!uri)
        {
          return makeError(
            Error::Code::CorruptData,
            std::format("track {} contains an invalid library URI: {}", trackId.raw(), uri.error().message));
        }

        auto resolved = uri->resolveUnder(library.rootPath());

        if (!resolved)
        {
          return std::unexpected{resolved.error()};
        }

        optFilePath = std::move(*resolved);
      }

      auto request = PlaybackTransport::PlaybackRequest{
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

    [[noreturn]] void failExecutorAffinity(std::source_location const& location)
    {
      APP_LOG_CRITICAL("PlaybackSuccession thread-affinity violation: '{}' invoked off the executor thread "
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

  struct PlaybackSuccession::Impl final
  {
    enum class NavigationDirection : std::uint8_t
    {
      Forward,
      Backward,
    };

    struct RestorableCursorSnapshot final
    {
      PlaybackLaunchSpec launchSpec;
      TrackId currentTrackId = kInvalidTrackId;
      ProjectionAnchor anchor;
    };

    struct PendingViewStart final
    {
      TrackId trackId = kInvalidTrackId;
      std::unique_ptr<PlaybackCursorSession> sessionPtr;
    };

    struct PendingLookahead final
    {
      TrackId trackId = kInvalidTrackId;
      ListId sourceListId = kInvalidListId;
      std::vector<TrackId> failedTrackIds;
    };

    Impl(async::Executor& executor,
         ViewService& views,
         TrackSourceCache& sources,
         library::MusicLibrary const& library,
         PlaybackTransport& transport,
         NotificationService& notifications,
         async::Runtime& asyncRuntime)
      : executor{executor}
      , views{views}
      , sources{sources}
      , library{library}
      , transport{transport}
      , notifications{notifications}
      , restartDeadline{asyncRuntime,
                        [this] { return this->transport.elapsed(); },
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

    static PlaybackSuccessionSourceState sourceStateFor(PlaybackCursorSession const* session) noexcept
    {
      if (session == nullptr)
      {
        return PlaybackSuccessionSourceState::Inactive;
      }

      return session->cursor().sourceState() == PlaybackCursor::SourceState::Live
               ? PlaybackSuccessionSourceState::Live
               : PlaybackSuccessionSourceState::Invalidated;
    }

    static bool sameSemanticTuple(PlaybackSuccessionState const& lhs, PlaybackSuccessionState const& rhs) noexcept
    {
      return lhs.sourceState == rhs.sourceState && lhs.currentTrackId == rhs.currentTrackId &&
             lhs.hasNext == rhs.hasNext && lhs.hasPrevious == rhs.hasPrevious &&
             lhs.optResolvedSuccessor == rhs.optResolvedSuccessor;
    }

    void synchronizeState()
    {
      auto next = PlaybackSuccessionState{
        .sourceState = sourceStateFor(sessionPtr.get()),
        .shuffle = shuffleMode,
        .repeat = repeatMode,
      };

      if (sessionPtr)
      {
        auto const& cursor = sessionPtr->cursor();
        auto const& semantic = cursor.semanticTuple();
        next.currentTrackId = semantic.currentTrackId;
        next.sourceListId = cursor.launchSpec().sourceListId;
        next.hasNext = semantic.hasNext;
        next.hasPrevious = semantic.hasPrevious;
        next.optResolvedSuccessor = semantic.optResolvedSuccessor;
      }

      auto const semanticChanged = !sameSemanticTuple(state, next);

      state = std::move(next);

      if (semanticChanged && !isClosing())
      {
        publishSequenceObserverSafely("state-changed", [this] { changedSignal.emit(state); });
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
        .launchSpec = cursor.launchSpec(),
        .currentTrackId = cursor.currentTrackId(),
        .anchor = cursor.anchor(),
      };
    }

    void notifyRestorableStateChanged()
    {
      if (!isClosing())
      {
        publishSequenceObserverSafely("restorable-state-changed", [this] { restorableStateChangedSignal.emit(); });
      }
    }

    void cancelPendingStart()
    {
      optPendingViewStart.reset();
      transport.cancelSuccessionStartPreparation();
    }

    void cancelPendingLookahead()
    {
      pendingLookaheadPtr.reset();
      transport.cancelSuccessionLookaheadPreparation();
    }

    bool isLookaheadCandidateCurrent(PendingLookahead const& pending) const
    {
      return !isClosing() && sessionPtr && sessionPtr->cursor().launchSpec().sourceListId == pending.sourceListId &&
             sessionPtr->cursor().sourceState() == PlaybackCursor::SourceState::Live &&
             sessionPtr->cursor().semanticTuple().optResolvedSuccessor == pending.trackId &&
             sessionPtr->indexOf(pending.trackId).has_value();
    }

    bool isPendingLookaheadCurrent(std::shared_ptr<PendingLookahead> const& pendingPtr) const
    {
      return pendingLookaheadPtr == pendingPtr && isLookaheadCandidateCurrent(*pendingPtr);
    }

    bool acceptPendingStart()
    {
      return !isClosing() && optPendingViewStart && optPendingViewStart->sessionPtr &&
             optPendingViewStart->sessionPtr->cursor().sourceState() == PlaybackCursor::SourceState::Live &&
             optPendingViewStart->sessionPtr->indexOf(optPendingViewStart->trackId).has_value();
    }

    void completePendingStart(Result<PreparedPlaybackStart> preparedStart)
    {
      if (!optPendingViewStart)
      {
        return;
      }

      if (!preparedStart)
      {
        optPendingViewStart.reset();
        return;
      }

      auto pending = std::move(*optPendingViewStart);
      optPendingViewStart.reset();
      auto barrier = transport.commitStagedPlayback(std::move(*preparedStart), false);

      if (!barrier || isClosing())
      {
        return;
      }

      if (sessionPtr)
      {
        sessionPtr->clearPreparedCoveredBy(*barrier);
        captureRestorableSnapshot();
      }

      // The candidate was built at admission time. Mode commands remain live
      // while preparation runs, so settle it against the latest preferences
      // before the candidate becomes the authoritative cursor.
      std::ignore = pending.sessionPtr->setRepeatMode(repeatMode);
      std::ignore = pending.sessionPtr->setShuffleMode(shuffleMode);
      sessionPtr = std::move(pending.sessionPtr);
      startObservingCurrentSession();
      resetFailureState();
      restartDeadline.replaceSession(std::chrono::milliseconds{0}, true);
      reprepareNext(false);
      synchronizeState();
      notifyRestorableStateChanged();

      if (!isClosing())
      {
        publishSequenceObserverSafely("explicit-start-settled", [this] { explicitStartSettledSignal.emit(); });
      }
    }

    void deactivateSession()
    {
      cancelPendingStart();
      cancelPendingLookahead();

      if (!sessionPtr)
      {
        return;
      }

      captureRestorableSnapshot();
      auto const optDisarmedToken = transport.clearSuccessionPreparedNext();
      sessionPtr->invalidatePreparedNext(optDisarmedToken);
      sessionPtr.reset();
      restartDeadline.clearSession();
      resetFailureState();
      synchronizeState();
    }

    void stopTerminal(bool postNotification)
    {
      cancelPendingStart();
      cancelPendingLookahead();

      if (!sessionPtr)
      {
        return;
      }

      captureRestorableSnapshot();
      stoppingTransport = true;

      if (auto const barrier = transport.stopSuccession(); sessionPtr)
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
          .message = NotificationReport{.templateId = NotificationReportTemplate::PlaybackSequenceFinished},
          .lifetime = NotificationLifetime::transient(),
        });
      }
    }

    void resetFailureState() noexcept
    {
      optSkipReportKey.reset();
      skippedFailureCount = 0;

      if (sessionPtr)
      {
        sessionPtr->resetConsecutiveFailureCount();
      }
    }

    void reportSkippedTrack()
    {
      ++skippedFailureCount;

      if (!optSkipReportKey)
      {
        ++nextSkipReportKey;
        optSkipReportKey.emplace(std::format("playback-sequence.skipped-tracks.{}", nextSkipReportKey));
      }

      notifications.createOrUpdate(
        *optSkipReportKey,
        NotificationRequest{
          .severity = NotificationSeverity::Warning,
          .message = NotificationReport{.templateId = NotificationReportTemplate::PlaybackTracksSkipped,
                                        .count = skippedFailureCount},
          .lifetime = NotificationLifetime::history(),
        });
    }

    void reportFailureLimit()
    {
      notifications.post(NotificationRequest{
        .severity = NotificationSeverity::Error,
        .message = NotificationReport{.templateId = NotificationReportTemplate::PlaybackStoppedAfterFailures,
                                      .count = kMaxConsecutivePlaybackFailures},
        .lifetime = NotificationLifetime::pinned(),
      });
    }

    void reportTerminalTrackFailure(PlaybackFailure const& failure)
    {
      notifications.post(NotificationRequest{
        .severity = NotificationSeverity::Error,
        .message = NotificationReport{.templateId = NotificationReportTemplate::PlaybackStoppedForTrack,
                                      .trackId = failure.trackId,
                                      .subject = failure.title,
                                      .detail = failure.error.message},
        .lifetime = NotificationLifetime::pinned(),
      });
    }

    void startObservingCurrentSession()
    {
      sessionPtr->startObserving(
        [this](PlaybackCursor::Changes const changes, bool const sourceInvalidated)
        {
          if (!isClosing())
          {
            handleProjectionBatch(changes, sourceInvalidated);
          }
        });
    }

    void handleProjectionBatch(PlaybackCursor::Changes const changes, bool const sourceInvalidated)
    {
      if (!sessionPtr)
      {
        return;
      }

      if (sourceInvalidated)
      {
        auto const optDisarmedToken = transport.clearSuccessionPreparedNext();
        sessionPtr->invalidatePreparedNext(optDisarmedToken);
        captureRestorableSnapshot();
      }
      else
      {
        reprepareNext(false);
      }

      if (changes.semanticChanged || sourceInvalidated)
      {
        synchronizeState();
      }

      if (changes.restorableStateChanged)
      {
        notifyRestorableStateChanged();
      }
    }

    bool retryLookahead(PendingLookahead pending)
    {
      if (!isLookaheadCandidateCurrent(pending) || sessionPtr->cursor().shuffleMode() != ShuffleMode::On)
      {
        return false;
      }

      pending.failedTrackIds.push_back(pending.trackId);

      if (pending.failedTrackIds.size() >= kMaxConsecutivePlaybackFailures)
      {
        return false;
      }

      auto const optRetry = sessionPtr->rerollShuffleForward(pending.failedTrackIds);

      if (!optRetry)
      {
        return false;
      }

      if (auto const effect = sessionPtr->refreshSemanticState(); effect.semanticChanged)
      {
        synchronizeState();
      }

      pending.trackId = *optRetry;
      return prepareLookahead(std::move(pending));
    }

    bool completeLookahead(std::shared_ptr<PendingLookahead> const& pendingPtr, Result<PreparedNextToken> prepared)
    {
      if (pendingLookaheadPtr != pendingPtr)
      {
        return false;
      }

      auto pending = std::move(*pendingPtr);
      pendingLookaheadPtr.reset();

      if (!prepared)
      {
        return prepared.error().code != Error::Code::Conflict && retryLookahead(std::move(pending));
      }

      if (!isLookaheadCandidateCurrent(pending))
      {
        return false;
      }

      sessionPtr->preparedNextRegistry().activate(
        *prepared, sessionPtr->anchorFor(pending.trackId, sessionPtr->cursor().anchor().anchorIndex()));
      return true;
    }

    bool prepareLookahead(PendingLookahead pending)
    {
      auto const successor = pending.trackId;
      auto const sourceListId = pending.sourceListId;
      auto pendingPtr = std::make_shared<PendingLookahead>(std::move(pending));
      auto const pendingWeakPtr = std::weak_ptr{pendingPtr};
      pendingLookaheadPtr = pendingPtr;

      auto admitted = transport.prepareSuccessionNextAsync(
        successor,
        sourceListId,
        [this, pendingWeakPtr]
        {
          auto const lockedPendingPtr = pendingWeakPtr.lock();
          return lockedPendingPtr && isPendingLookaheadCurrent(lockedPendingPtr);
        },
        [this, pendingWeakPtr](Result<PreparedNextToken> prepared)
        {
          if (auto const lockedPendingPtr = pendingWeakPtr.lock(); lockedPendingPtr)
          {
            std::ignore = completeLookahead(lockedPendingPtr, std::move(prepared));
          }
        });

      if (!admitted)
      {
        return completeLookahead(pendingPtr, std::unexpected{admitted.error()});
      }

      return true;
    }

    bool reprepareNext(bool const force)
    {
      if (!sessionPtr)
      {
        cancelPendingLookahead();
        std::ignore = transport.clearSuccessionPreparedNext();
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

      cancelPendingLookahead();
      auto const optDisarmedToken = transport.clearSuccessionPreparedNext();
      registry.invalidate(optDisarmedToken);

      if (!optSuccessor)
      {
        return false;
      }

      auto const successor = *optSuccessor;
      auto const sourceListId = session.cursor().launchSpec().sourceListId;
      return prepareLookahead(
        PendingLookahead{.trackId = successor, .sourceListId = sourceListId, .failedTrackIds = {}});
    }

    Result<> startTrack(TrackId const trackId, ShuffleHistory::TransitionOrigin const origin)
    {
      if (!sessionPtr)
      {
        return makeError(Error::Code::InvalidState, "No active playback sequence");
      }

      auto barrier = transport.playSuccessionTrack(trackId, sessionPtr->cursor().launchSpec().sourceListId);

      if (!barrier)
      {
        return std::unexpected{barrier.error()};
      }

      if (isClosing())
      {
        return {};
      }

      sessionPtr->clearPreparedCoveredBy(*barrier);
      auto adopted = sessionPtr->adoptCurrent(trackId, std::nullopt, origin);

      if (!adopted)
      {
        return std::unexpected{adopted.error()};
      }

      resetFailureState();
      restartDeadline.currentTrackChanged(std::chrono::milliseconds{0}, true);
      reprepareNext(false);
      synchronizeState();
      notifyRestorableStateChanged();
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

    std::optional<TrackId> nextShuffleFailureCandidate(ShuffleHistory::TransitionOrigin const origin,
                                                       NavigationDirection const direction,
                                                       std::span<TrackId const> attemptedTrackIds) const
    {
      if (!sessionPtr)
      {
        return std::nullopt;
      }

      if (origin == ShuffleHistory::TransitionOrigin::HistoryPrevious)
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
            return retry.trackId;
          }
        }

        return std::nullopt;
      }

      if (direction != NavigationDirection::Forward)
      {
        return std::nullopt;
      }

      std::ignore = sessionPtr->rerollShuffleForward(attemptedTrackIds);
      std::ignore = sessionPtr->refreshSemanticState();

      if (auto const retry = sessionPtr->cursor().resolveNext();
          retry.action == PlaybackCursor::CommandAction::StartTrack &&
          !std::ranges::contains(attemptedTrackIds, retry.trackId))
      {
        return retry.trackId;
      }

      return std::nullopt;
    }

    std::optional<TrackId> nextNavigationFailureCandidate(TrackId const failedTrackId,
                                                          ShuffleHistory::TransitionOrigin const origin,
                                                          NavigationDirection const direction,
                                                          std::span<TrackId const> attemptedTrackIds) const
    {
      if (sessionPtr && sessionPtr->cursor().shuffleMode() == ShuffleMode::On)
      {
        return nextShuffleFailureCandidate(origin, direction, attemptedTrackIds);
      }

      return nextFailureCandidate(failedTrackId, direction, attemptedTrackIds);
    }

    void settleExhaustedNavigation(bool const stopWhenExhausted)
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
    }

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

        auto const optCandidate =
          nextNavigationFailureCandidate(candidate, candidateOrigin, direction, attemptedTrackIds);

        if (!optCandidate)
        {
          settleExhaustedNavigation(stopWhenExhausted);
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

    void handleNowPlayingChanged(PlaybackTransport::NowPlayingChanged const& event)
    {
      if (!sessionPtr || event.trackId == kInvalidTrackId)
      {
        return;
      }

      if (event.sourceListId != sessionPtr->cursor().launchSpec().sourceListId)
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

      // A natural transition that settles before a pending explicit start
      // establishes the winning live context. Prevent that older candidate
      // from committing over the accepted transition.
      cancelPendingStart();
      resetFailureState();
      restartDeadline.currentTrackChanged(std::chrono::milliseconds{0}, true);
      reprepareNext(false);
      synchronizeState();
      notifyRestorableStateChanged();
    }

    PlaybackFailureDisposition handlePlaybackFailure(PlaybackFailure const& failure)
    {
      if (!sessionPtr || failure.sourceListId != sessionPtr->cursor().launchSpec().sourceListId)
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
        synchronizeState();
      }
    }

    void start()
    {
      transport.bindPlaybackFailureRecovery(
        [this](PlaybackFailure const& failure)
        {
          if (!isClosing())
          {
            return handlePlaybackFailure(failure);
          }

          return PlaybackFailureDisposition::Unhandled;
        });
      idleSubscription = transport.onIdle(
        [this]
        {
          if (!isClosing() && sessionPtr && !stoppingTransport && transport.state().transport == audio::Transport::Idle)
          {
            advanceNatural();
          }
        });
      nowPlayingSubscription = transport.onNowPlayingChanged(
        [this](PlaybackTransport::NowPlayingChanged const& event)
        {
          if (!isClosing())
          {
            handleNowPlayingChanged(event);
          }
        });
      outputSubscription = transport.onOutputDeviceChanged(
        [this](OutputDeviceSelection const&)
        {
          if (!isClosing())
          {
            cancelPendingStart();
            reprepareNext(true);
          }
        });
      seekSubscription = transport.onSeekUpdate(
        [this](PlaybackTransport::SeekUpdate const& event)
        {
          if (!isClosing() && sessionPtr && event.mode == PlaybackTransport::SeekMode::Final)
          {
            cancelPendingStart();
            restartDeadline.seek(transport.elapsed());
            reprepareNext(true);
          }
          else if (!isClosing() && event.mode == PlaybackTransport::SeekMode::Final)
          {
            cancelPendingStart();
          }
        });
      startedSubscription = transport.onStarted(
        [this]
        {
          if (!isClosing() && sessionPtr)
          {
            restartDeadline.resume(transport.elapsed());
          }
        });
      pausedSubscription = transport.onPaused(
        [this]
        {
          if (!isClosing() && sessionPtr)
          {
            restartDeadline.pause(transport.elapsed());
          }
        });
      stoppedSubscription = transport.onStopped(
        [this]
        {
          if (!isClosing() && !stoppingTransport)
          {
            deactivateSession();
          }
        });
    }

    void shutdown() noexcept
    {
      if (closing.exchange(true, std::memory_order_acq_rel))
      {
        return;
      }

      cancelPendingStart();
      cancelPendingLookahead();

      transport.unbindPlaybackFailureRecovery();
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
      explicitStartSettledSignal.disconnectAll();
      shuffleModeChangedSignal.disconnectAll();
      repeatModeChangedSignal.disconnectAll();
      restorableStateChangedSignal.disconnectAll();
    }

    async::Executor& executor;
    ViewService& views;
    TrackSourceCache& sources;
    library::MusicLibrary const& library;
    PlaybackTransport& transport;
    NotificationService& notifications;
    PlaybackSuccessionState state{};
    ShuffleMode shuffleMode = ShuffleMode::Off;
    RepeatMode repeatMode = RepeatMode::Off;
    std::unique_ptr<PlaybackCursorSession> sessionPtr;
    std::optional<PendingViewStart> optPendingViewStart;
    std::shared_ptr<PendingLookahead> pendingLookaheadPtr;
    std::optional<RestorableCursorSnapshot> optLastRestorableSnapshot;
    async::Signal<PlaybackSuccessionState const&> changedSignal;
    async::Signal<> explicitStartSettledSignal;
    async::Signal<PlaybackSuccession::ShuffleModeChanged const&> shuffleModeChangedSignal;
    async::Signal<PlaybackSuccession::RepeatModeChanged const&> repeatModeChangedSignal;
    async::Signal<> restorableStateChangedSignal;
    PlaybackRestartDeadline restartDeadline;
    async::Subscription idleSubscription;
    async::Subscription nowPlayingSubscription;
    async::Subscription outputSubscription;
    async::Subscription seekSubscription;
    async::Subscription startedSubscription;
    async::Subscription pausedSubscription;
    async::Subscription stoppedSubscription;
    std::optional<NotificationReportKey> optSkipReportKey;
    std::uint64_t nextSkipReportKey = 0;
    std::size_t skippedFailureCount = 0;
    std::atomic_bool closing{false};
    bool stoppingTransport = false;
  };

  PlaybackSuccession::PlaybackSuccession(async::Executor& executor,
                                         ViewService& views,
                                         TrackSourceCache& sources,
                                         library::MusicLibrary const& library,
                                         PlaybackTransport& transport,
                                         NotificationService& notifications,
                                         async::Runtime& asyncRuntime)
    : _implPtr{std::make_unique<Impl>(executor, views, sources, library, transport, notifications, asyncRuntime)}
  {
    _implPtr->start();
  }

  PlaybackSuccession::~PlaybackSuccession() = default;

  Result<> PlaybackSuccession::playFromView(ViewId const viewId, TrackId const startTrackId)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    impl->cancelPendingStart();
    impl->cancelPendingLookahead();

    auto launchSpec = impl->views.capturePlaybackLaunchSpec(viewId);

    if (!launchSpec)
    {
      return std::unexpected{launchSpec.error()};
    }

    auto candidateSession = PlaybackCursorSession::create(*launchSpec,
                                                          startTrackId,
                                                          impl->sources,
                                                          impl->library,
                                                          impl->repeatMode,
                                                          impl->shuffleMode,
                                                          impl->makeCandidateChooser());

    if (!candidateSession)
    {
      return std::unexpected{candidateSession.error()};
    }

    auto request = playbackRequestForTrack(impl->library, startTrackId);

    if (!request)
    {
      return std::unexpected{request.error()};
    }

    impl->optPendingViewStart.emplace(Impl::PendingViewStart{
      .trackId = startTrackId,
      .sessionPtr = std::move(*candidateSession),
    });
    auto admitted = impl->transport.stageSuccessionPlaybackAsync(
      *request,
      launchSpec->sourceListId,
      [impl] { return impl->acceptPendingStart(); },
      [impl](Result<PreparedPlaybackStart> preparedStart) mutable
      { impl->completePendingStart(std::move(preparedStart)); });

    if (!admitted)
    {
      impl->optPendingViewStart.reset();
      return std::unexpected{admitted.error()};
    }

    return {};
  }

  bool PlaybackSuccession::hasNext() const
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->state.hasNext;
  }

  bool PlaybackSuccession::hasPrevious() const
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->state.hasPrevious;
  }

  bool PlaybackSuccession::next()
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    impl->cancelPendingStart();
    impl->cancelPendingLookahead();

    if (!impl->sessionPtr)
    {
      return false;
    }

    impl->resetFailureState();
    return impl->attemptNavigation(impl->sessionPtr->cursor().resolveNext(),
                                   ShuffleHistory::TransitionOrigin::Forward,
                                   Impl::NavigationDirection::Forward,
                                   true);
  }

  bool PlaybackSuccession::previous()
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    impl->cancelPendingStart();
    impl->cancelPendingLookahead();

    if (!impl->sessionPtr)
    {
      return false;
    }

    impl->resetFailureState();
    auto const shufflePrevious = impl->sessionPtr->cursor().shuffleMode() == ShuffleMode::On &&
                                 !impl->sessionPtr->cursor().previousRestartAvailable();
    auto const resolution = impl->sessionPtr->resolvePrevious();

    if (shufflePrevious && resolution.action == PlaybackCursor::CommandAction::NoOp)
    {
      if (auto const effect = impl->sessionPtr->refreshSemanticState(); effect.semanticChanged)
      {
        impl->synchronizeState();
      }
    }

    return impl->attemptNavigation(resolution,
                                   shufflePrevious ? ShuffleHistory::TransitionOrigin::HistoryPrevious
                                                   : ShuffleHistory::TransitionOrigin::SequentialPrevious,
                                   Impl::NavigationDirection::Backward,
                                   false);
  }

  void PlaybackSuccession::clear()
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();

    impl->deactivateSession();
  }

  void PlaybackSuccession::setShuffleMode(ShuffleMode const mode)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();

    if (impl->shuffleMode == mode)
    {
      return;
    }

    impl->shuffleMode = mode;

    if (impl->sessionPtr)
    {
      std::ignore = impl->sessionPtr->setShuffleMode(mode);
      impl->reprepareNext(true);
    }

    impl->synchronizeState();

    publishSequenceObserverSafely(
      "shuffle-mode-changed", [&] { impl->shuffleModeChangedSignal.emit(ShuffleModeChanged{.mode = mode}); });

    impl->notifyRestorableStateChanged();
  }

  void PlaybackSuccession::setRepeatMode(RepeatMode const mode)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();

    if (impl->repeatMode == mode)
    {
      return;
    }

    impl->repeatMode = mode;

    if (impl->sessionPtr)
    {
      std::ignore = impl->sessionPtr->setRepeatMode(mode);
      impl->reprepareNext(true);
    }

    impl->synchronizeState();

    publishSequenceObserverSafely(
      "repeat-mode-changed", [&] { impl->repeatModeChangedSignal.emit(RepeatModeChanged{.mode = mode}); });

    impl->notifyRestorableStateChanged();
  }

  PlaybackSuccessionState const& PlaybackSuccession::state() const
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->state;
  }

  async::Subscription PlaybackSuccession::onChanged(
    std::move_only_function<void(PlaybackSuccessionState const&)> handler)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->changedSignal.connect(std::move(handler));
  }

  async::Subscription PlaybackSuccession::onExplicitStartSettled(std::move_only_function<void()> handler)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->explicitStartSettledSignal.connect(std::move(handler));
  }

  async::Subscription PlaybackSuccession::onShuffleModeChanged(
    std::move_only_function<void(ShuffleModeChanged const&)> handler)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->shuffleModeChangedSignal.connect(std::move(handler));
  }

  async::Subscription PlaybackSuccession::onRepeatModeChanged(
    std::move_only_function<void(RepeatModeChanged const&)> handler)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->repeatModeChangedSignal.connect(std::move(handler));
  }

  bool PlaybackSuccession::hasActivePlaybackSession() const
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->sessionPtr != nullptr;
  }

  bool PlaybackSuccession::capturePlaybackSessionSnapshot(PlaybackLaunchSpec& launchSpec,
                                                          TrackId& currentTrackId,
                                                          std::size_t& anchorIndex) const
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();

    if (impl->sessionPtr)
    {
      auto const& cursor = impl->sessionPtr->cursor();
      launchSpec = cursor.launchSpec();
      currentTrackId = cursor.currentTrackId();
      anchorIndex = cursor.anchor().anchorIndex();
      return true;
    }

    if (!impl->optLastRestorableSnapshot)
    {
      return false;
    }

    launchSpec = impl->optLastRestorableSnapshot->launchSpec;
    currentTrackId = impl->optLastRestorableSnapshot->currentTrackId;
    anchorIndex = impl->optLastRestorableSnapshot->anchor.anchorIndex();
    return true;
  }

  Result<std::unique_ptr<PlaybackCursorSession>> PlaybackSuccession::preparePlaybackSessionRestore(
    PlaybackLaunchSpec launchSpec,
    TrackId const currentTrackId,
    std::size_t const anchorIndex,
    ShuffleMode const restoredShuffleMode,
    RepeatMode const restoredRepeatMode)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return PlaybackCursorSession::createForRestore(std::move(launchSpec),
                                                   currentTrackId,
                                                   anchorIndex,
                                                   impl->sources,
                                                   impl->library,
                                                   restoredRepeatMode,
                                                   restoredShuffleMode,
                                                   impl->makeCandidateChooser());
  }

  // Executor-affinity failure deliberately terminates this otherwise contained noexcept commit.
  void PlaybackSuccession::commitPlaybackSessionRestore(std::unique_ptr<PlaybackCursorSession> sessionPtr,
                                                        ShuffleMode const restoredShuffleMode,
                                                        RepeatMode const restoredRepeatMode,
                                                        std::chrono::milliseconds const elapsed) noexcept
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();

    if (!sessionPtr)
    {
      return;
    }

    impl->sessionPtr = std::move(sessionPtr);
    impl->optLastRestorableSnapshot.reset();
    impl->shuffleMode = restoredShuffleMode;
    impl->repeatMode = restoredRepeatMode;
    impl->resetFailureState();
    runAcceptedRestoreStepSafely("projection observation", [&] { impl->startObservingCurrentSession(); });
    runAcceptedRestoreStepSafely("restart deadline", [&] { impl->restartDeadline.replaceSession(elapsed, false); });
    runAcceptedRestoreStepSafely("prepared successor", [&] { impl->reprepareNext(false); });
    runAcceptedRestoreStepSafely("state publication", [&] { impl->synchronizeState(); });
    publishSequenceObserverSafely(
      "shuffle-mode-changed",
      [&] { impl->shuffleModeChangedSignal.emit(ShuffleModeChanged{.mode = restoredShuffleMode}); });

    if (!impl->isClosing())
    {
      publishSequenceObserverSafely(
        "repeat-mode-changed",
        [&] { impl->repeatModeChangedSignal.emit(RepeatModeChanged{.mode = restoredRepeatMode}); });
    }
  }

  void PlaybackSuccession::discardPlaybackSessionSnapshot()
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    impl->optLastRestorableSnapshot.reset();
  }

  async::Subscription PlaybackSuccession::onRestorableStateChanged(std::move_only_function<void()> handler)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->restorableStateChangedSignal.connect(std::move(handler));
  }
} // namespace ao::rt
