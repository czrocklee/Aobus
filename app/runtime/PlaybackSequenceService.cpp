// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackCursorSession.h"
#include "runtime/playback/PlaybackRestartDeadline.h"
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
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <gsl-lite/gsl-lite.hpp>

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

    Result<PlaybackService::PlaybackRequest> playbackRequestForTrack(library::MusicLibrary const& library,
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

  struct PlaybackSequenceService::Impl final
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

    class [[nodiscard]] AcceptanceTransaction final
    {
    public:
      explicit AcceptanceTransaction(Impl& owner) noexcept
        : _owner{owner}
      {
        _owner.acceptanceDepth.fetch_add(1, std::memory_order_acq_rel);
      }

      ~AcceptanceTransaction() { _owner.acceptanceDepth.fetch_sub(1, std::memory_order_acq_rel); }

      AcceptanceTransaction(AcceptanceTransaction const&) = delete;
      AcceptanceTransaction& operator=(AcceptanceTransaction const&) = delete;
      AcceptanceTransaction(AcceptanceTransaction&&) = delete;
      AcceptanceTransaction& operator=(AcceptanceTransaction&&) = delete;

    private:
      Impl& _owner;
    };

    class [[nodiscard]] ObserverPublicationScope final
    {
    public:
      explicit ObserverPublicationScope(Impl& owner) noexcept
        : _owner{owner}
      {
        _owner.observerPublicationDepth.fetch_add(1, std::memory_order_acq_rel);
      }

      ~ObserverPublicationScope() { _owner.observerPublicationDepth.fetch_sub(1, std::memory_order_acq_rel); }

      ObserverPublicationScope(ObserverPublicationScope const&) = delete;
      ObserverPublicationScope& operator=(ObserverPublicationScope const&) = delete;
      ObserverPublicationScope(ObserverPublicationScope&&) = delete;
      ObserverPublicationScope& operator=(ObserverPublicationScope&&) = delete;

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
         library::MusicLibrary const& library,
         PlaybackService& playback,
         NotificationService& notifications,
         async::Runtime& asyncRuntime)
      : executor{executor}
      , views{views}
      , sources{sources}
      , library{library}
      , playback{playback}
      , notifications{notifications}
      , restartDeadline{asyncRuntime,
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
    bool isAcceptingStart() const noexcept { return acceptanceDepth.load(std::memory_order_acquire) != 0; }
    bool isInstallingSession() const noexcept { return sessionInstallationDepth.load(std::memory_order_acquire) != 0; }
    bool isRestoringSession() const noexcept { return restoreDepth.load(std::memory_order_acquire) != 0; }
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
        next.sourceListId = cursor.launchSpec().sourceListId;
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
        auto const publication = ObserverPublicationScope{*this};

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
        .launchSpec = cursor.launchSpec(),
        .currentTrackId = cursor.currentTrackId(),
        .anchor = cursor.anchor(),
      };
    }

    void notifyPersistenceIntentChanged(bool const containObserverExceptions = false)
    {
      if (!isRestoringSession() && !isClosing())
      {
        auto const publication = ObserverPublicationScope{*this};

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

      if (auto const barrier = playback.stopSequence(); sessionPtr)
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
          .lifetime = NotificationLifetime::transient(),
          .content = NotificationContentState{.topic = NotificationTopic::PlaybackSequence},
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
      auto const message = skippedTracksMessage(skippedFailureCount);

      if (!optSkipReportKey)
      {
        ++nextSkipReportKey;
        optSkipReportKey.emplace(std::format("playback-sequence.skipped-tracks.{}", nextSkipReportKey));
      }

      std::ignore = notifications.createOrUpdate(
        *optSkipReportKey,
        NotificationRequest{
          .severity = NotificationSeverity::Warning,
          .message = message,
          .lifetime = NotificationLifetime::sessionHistory(),
          .content = NotificationContentState{.topic = NotificationTopic::PlaybackSequence},
        });
    }

    void reportFailureLimit()
    {
      notifications.post(NotificationRequest{
        .severity = NotificationSeverity::Error,
        .message = playbackStoppedMessage(),
        .lifetime = NotificationLifetime::untilDismissed(),
        .content = NotificationContentState{.topic = NotificationTopic::PlaybackSequence},
      });
    }

    void reportTerminalTrackFailure(PlaybackFailure const& failure)
    {
      notifications.post(NotificationRequest{
        .severity = NotificationSeverity::Error,
        .message = terminalTrackFailureMessage(failure),
        .lifetime = NotificationLifetime::untilDismissed(),
        .content = NotificationContentState{.topic = NotificationTopic::PlaybackSequence},
      });
    }

    void startObservingCurrentSession()
    {
      sessionPtr->startObserving(
        [this](PlaybackCursor::MutationEffect const effect, bool const sourceInvalidated)
        {
          if (!isClosing())
          {
            handleProjectionBatch(effect, sourceInvalidated);
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
        auto prepared = playback.prepareSequenceNext(successor, session.cursor().launchSpec().sourceListId);

        if (prepared)
        {
          registry.activate(prepared->token,
                            prepared->issuedGeneration,
                            session.anchorFor(successor, session.cursor().anchor().anchorIndex()));
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
      auto receipt = playback.playSequenceTrack(trackId, sessionPtr->cursor().launchSpec().sourceListId);

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

    void handleNowPlayingChanged(PlaybackService::NowPlayingChanged const& event)
    {
      if (isAcceptingStart() || !sessionPtr || event.trackId == kInvalidTrackId)
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

      resetFailureState();
      restartDeadline.currentTrackChanged(std::chrono::milliseconds{0}, true);
      reprepareNext(false);
      synchronizeState(blocksSequenceCommand());
      notifyPersistenceIntentChanged(blocksSequenceCommand());
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
        synchronizeState(blocksSequenceCommand());
      }
    }

    void start()
    {
      playback.bindPlaybackFailureRecovery(
        [this](PlaybackFailure const& failure)
        {
          if (!isClosing())
          {
            return handlePlaybackFailure(failure);
          }

          return PlaybackFailureDisposition::Unhandled;
        });
      idleSubscription = playback.onIdle(
        [this]
        {
          if (!isClosing() && sessionPtr && !stoppingTransport && playback.state().transport == audio::Transport::Idle)
          {
            advanceNatural();
          }
        });
      nowPlayingSubscription = playback.onNowPlayingChanged(
        [this](PlaybackService::NowPlayingChanged const& event)
        {
          if (!isClosing())
          {
            handleNowPlayingChanged(event);
          }
        });
      outputSubscription = playback.onOutputDeviceChanged(
        [this](OutputDeviceSelection const&)
        {
          if (!isClosing())
          {
            reprepareNext(true);
          }
        });
      seekSubscription = playback.onSeekUpdate(
        [this](PlaybackService::SeekUpdate const& event)
        {
          if (!isClosing() && sessionPtr && event.mode == PlaybackService::SeekMode::Final)
          {
            restartDeadline.seek(playback.elapsed());
            reprepareNext(true);
          }
        });
      startedSubscription = playback.onStarted(
        [this]
        {
          if (!isClosing() && sessionPtr && !isAcceptingStart())
          {
            restartDeadline.resume(playback.elapsed());
          }
        });
      pausedSubscription = playback.onPaused(
        [this]
        {
          if (!isClosing() && sessionPtr)
          {
            restartDeadline.pause(playback.elapsed());
          }
        });
      stoppedSubscription = playback.onStopped(
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
    library::MusicLibrary const& library;
    PlaybackService& playback;
    NotificationService& notifications;
    PlaybackSequenceState state{};
    ShuffleMode shuffleMode = ShuffleMode::Off;
    RepeatMode repeatMode = RepeatMode::Off;
    std::unique_ptr<PlaybackCursorSession> sessionPtr;
    std::optional<RestorableCursorSnapshot> optLastRestorableSnapshot;
    async::Signal<PlaybackSequenceState const&> changedSignal;
    async::Signal<PlaybackSequenceService::ShuffleModeChanged const&> shuffleModeChangedSignal;
    async::Signal<PlaybackSequenceService::RepeatModeChanged const&> repeatModeChangedSignal;
    async::Signal<> persistenceIntentChangedSignal;
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
    std::atomic_size_t acceptanceDepth{0};
    std::atomic_size_t observerPublicationDepth{0};
    std::atomic_size_t sessionInstallationDepth{0};
    bool stoppingTransport = false;
    std::atomic_size_t restoreDepth{0};
  };

  PlaybackSequenceService::PlaybackSequenceService(async::Executor& executor,
                                                   ViewService& views,
                                                   TrackSourceCache& sources,
                                                   library::MusicLibrary const& library,
                                                   PlaybackService& playback,
                                                   NotificationService& notifications,
                                                   async::Runtime& asyncRuntime)
    : _implPtr{std::make_unique<Impl>(executor, views, sources, library, playback, notifications, asyncRuntime)}
  {
    _implPtr->start();
  }

  PlaybackSequenceService::~PlaybackSequenceService()
  {
    gsl_Expects(_implPtr != nullptr);
    gsl_Expects(_implPtr->acceptanceDepth.load(std::memory_order_acquire) == 0);
    gsl_Expects(_implPtr->observerPublicationDepth.load(std::memory_order_acquire) == 0);
  }

  Result<> PlaybackSequenceService::playFromView(ViewId const viewId, TrackId const startTrackId)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();

    if (impl->blocksSequenceCommand())
    {
      return makeError(Error::Code::InvalidState, "Playback sequence is accepting another start");
    }

    auto const acceptance = Impl::AcceptanceTransaction{*impl};

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

    auto preparedStart = impl->playback.stagePlayback(*request, launchSpec->sourceListId);

    if (!preparedStart)
    {
      return std::unexpected{preparedStart.error()};
    }

    if (impl->isClosing())
    {
      return makeError(Error::Code::InvalidState, "Playback sequence closed during preparation");
    }

    auto receipt = impl->playback.commitPlayback(std::move(*preparedStart));

    if (!receipt)
    {
      return std::unexpected{receipt.error()};
    }

    if (impl->isClosing())
    {
      return {};
    }

    if (impl->sessionPtr)
    {
      impl->sessionPtr->clearPreparedCoveredBy(receipt->cancellationBarrier);
      impl->captureRestorableSnapshot();
    }

    impl->sessionPtr = std::move(*candidateSession);

    {
      auto const installation = Impl::SessionInstallationTransaction{*impl};
      impl->startObservingCurrentSession();
    }

    impl->resetFailureState();
    impl->restartDeadline.replaceSession(std::chrono::milliseconds{0}, true);
    impl->reprepareNext(false);
    impl->synchronizeState(true);
    impl->notifyPersistenceIntentChanged(true);
    return {};
  }

  bool PlaybackSequenceService::hasNext() const
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->state.hasNext;
  }

  bool PlaybackSequenceService::hasPrevious() const
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->state.hasPrevious;
  }

  void PlaybackSequenceService::next()
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();

    if (impl->blocksSequenceCommand() || !impl->sessionPtr)
    {
      return;
    }

    impl->resetFailureState();
    std::ignore = impl->attemptNavigation(impl->sessionPtr->cursor().resolveNext(),
                                          ShuffleHistory::TransitionOrigin::Forward,
                                          Impl::NavigationDirection::Forward,
                                          true);
  }

  void PlaybackSequenceService::previous()
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();

    if (impl->blocksSequenceCommand() || !impl->sessionPtr)
    {
      return;
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

    std::ignore = impl->attemptNavigation(resolution,
                                          shufflePrevious ? ShuffleHistory::TransitionOrigin::HistoryPrevious
                                                          : ShuffleHistory::TransitionOrigin::SequentialPrevious,
                                          Impl::NavigationDirection::Backward,
                                          false);
  }

  void PlaybackSequenceService::clear()
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();

    if (impl->blocksSequenceCommand())
    {
      return;
    }

    impl->deactivateSession();
  }

  void PlaybackSequenceService::setShuffleMode(ShuffleMode const mode)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();

    if (impl->blocksSequenceCommand() || impl->shuffleMode == mode)
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

    {
      auto const publication = Impl::ObserverPublicationScope{*impl};
      impl->shuffleModeChangedSignal.emit(ShuffleModeChanged{.mode = mode});
    }

    impl->notifyPersistenceIntentChanged();
  }

  void PlaybackSequenceService::setRepeatMode(RepeatMode const mode)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();

    if (impl->blocksSequenceCommand() || impl->repeatMode == mode)
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

    {
      auto const publication = Impl::ObserverPublicationScope{*impl};
      impl->repeatModeChangedSignal.emit(RepeatModeChanged{.mode = mode});
    }

    impl->notifyPersistenceIntentChanged();
  }

  PlaybackSequenceState const& PlaybackSequenceService::state() const
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->state;
  }

  async::Subscription PlaybackSequenceService::onChanged(
    std::move_only_function<void(PlaybackSequenceState const&)> handler)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->changedSignal.connect(std::move(handler));
  }

  async::Subscription PlaybackSequenceService::onShuffleModeChanged(
    std::move_only_function<void(ShuffleModeChanged const&)> handler)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->shuffleModeChangedSignal.connect(std::move(handler));
  }

  async::Subscription PlaybackSequenceService::onRepeatModeChanged(
    std::move_only_function<void(RepeatModeChanged const&)> handler)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->repeatModeChangedSignal.connect(std::move(handler));
  }

  bool PlaybackSequenceService::hasActivePlaybackSession() const
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->sessionPtr != nullptr;
  }

  bool PlaybackSequenceService::capturePlaybackSessionSnapshot(PlaybackLaunchSpec& launchSpec,
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

  Result<std::unique_ptr<PlaybackCursorSession>> PlaybackSequenceService::preparePlaybackSessionRestore(
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
  void PlaybackSequenceService::commitPlaybackSessionRestore(std::unique_ptr<PlaybackCursorSession> sessionPtr,
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

    auto const acceptance = Impl::AcceptanceTransaction{*impl};
    auto const restoration = Impl::RestoreTransaction{*impl};
    impl->sessionPtr = std::move(sessionPtr);
    impl->optLastRestorableSnapshot.reset();
    impl->shuffleMode = restoredShuffleMode;
    impl->repeatMode = restoredRepeatMode;
    impl->resetFailureState();
    runAcceptedRestoreStepSafely("projection observation",
                                 [&]
                                 {
                                   auto const installation = Impl::SessionInstallationTransaction{*impl};
                                   impl->startObservingCurrentSession();
                                 });
    runAcceptedRestoreStepSafely("restart deadline", [&] { impl->restartDeadline.replaceSession(elapsed, false); });
    runAcceptedRestoreStepSafely("prepared successor", [&] { impl->reprepareNext(false); });
    runAcceptedRestoreStepSafely("state publication", [&] { impl->synchronizeState(true); });
    publishSequenceObserverSafely(
      "shuffle-mode-changed",
      [&]
      {
        auto const publication = Impl::ObserverPublicationScope{*impl};
        impl->shuffleModeChangedSignal.emit(ShuffleModeChanged{.mode = restoredShuffleMode});
      });

    if (!impl->isClosing())
    {
      publishSequenceObserverSafely("repeat-mode-changed",
                                    [&]
                                    {
                                      auto const publication = Impl::ObserverPublicationScope{*impl};
                                      impl->repeatModeChangedSignal.emit(RepeatModeChanged{.mode = restoredRepeatMode});
                                    });
    }
  }

  void PlaybackSequenceService::discardPlaybackSessionSnapshot()
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    impl->optLastRestorableSnapshot.reset();
  }

  async::Subscription PlaybackSequenceService::onPersistenceIntentChanged(std::move_only_function<void()> handler)
  {
    auto* const impl = _implPtr.get();
    impl->ensureOnExecutor();
    return impl->persistenceIntentChangedSignal.connect(std::move(handler));
  }
} // namespace ao::rt
