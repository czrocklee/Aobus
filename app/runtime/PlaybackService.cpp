// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Player.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/Transport.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackSessionState.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/ViewService.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    bool isTerminalTrackTransport(audio::Transport transport) noexcept
    {
      return transport == audio::Transport::Idle || transport == audio::Transport::Error;
    }

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

    PlaybackState buildPlaybackState(audio::Player::Status const& status)
    {
      auto outputBackends = std::vector<OutputBackendSnapshot>{};
      outputBackends.reserve(status.availableBackends.size());

      for (auto const& backendStatus : status.availableBackends)
      {
        outputBackends.push_back(toOutputBackendSnapshot(backendStatus));
      }

      return PlaybackState{
        .transport = status.engine.transport,
        .elapsed = status.engine.elapsed,
        .duration = status.engine.duration,
        .ready = status.isReady,
        .volume =
          VolumeState{
            .level = status.volume,
            .muted = status.muted,
            .available = status.volumeAvailable,
            .hardwareAssisted = status.volumeIsHardwareAssisted,
          },
        .output =
          OutputState{
            .selectedDevice =
              OutputDeviceSelection{
                .backendId = status.engine.backendId,
                .deviceId = status.engine.currentDeviceId,
                .profileId = status.engine.profileId,
              },
            .availableBackends = std::move(outputBackends),
          },
        .quality =
          QualityState{
            .sourceQuality = status.sourceQuality,
            .pipelineQuality = status.pipelineQuality,
            .overall = status.quality,
            .fullyVerified = status.qualityFullyVerified,
            .assessments = status.qualityAssessments,
          },
      };
    }

    bool hasOutputDevice(OutputDeviceSelection const& outputDevice)
    {
      return !outputDevice.backendId.empty();
    }

    bool sameOutputDevice(OutputDeviceSelection const& lhs, OutputDeviceSelection const& rhs)
    {
      return lhs.backendId == rhs.backendId && lhs.deviceId == rhs.deviceId && lhs.profileId == rhs.profileId;
    }

    std::optional<OutputDeviceSelection> defaultOutputDeviceSelection(
      std::vector<OutputBackendSnapshot> const& backends)
    {
      for (auto const& backend : backends)
      {
        for (auto const& device : backend.devices)
        {
          for (auto const& profile : backend.supportedProfiles)
          {
            if (supportsOutputProfile(device, profile.id))
            {
              return OutputDeviceSelection{
                .backendId = backend.id,
                .deviceId = device.id,
                .profileId = profile.id,
              };
            }
          }
        }
      }

      return std::nullopt;
    }

    std::string_view playbackErrorMessage(audio::Engine::Status const& status)
    {
      return status.statusText.empty() ? std::string_view{"Audio playback failed"}
                                       : std::string_view{status.statusText};
    }

    PlaybackFailureKind toPlaybackFailureKind(audio::Engine::PlaybackFailureKind const kind) noexcept
    {
      switch (kind)
      {
        case audio::Engine::PlaybackFailureKind::TrackOpen: return PlaybackFailureKind::TrackOpen;
        case audio::Engine::PlaybackFailureKind::Decode: return PlaybackFailureKind::Decode;
        case audio::Engine::PlaybackFailureKind::RouteActivation: return PlaybackFailureKind::RouteActivation;
        case audio::Engine::PlaybackFailureKind::DeviceLost: return PlaybackFailureKind::DeviceLost;
      }

      return PlaybackFailureKind::RouteActivation;
    }

    bool isOutputFailureKind(PlaybackFailureKind kind) noexcept
    {
      return kind == PlaybackFailureKind::RouteActivation || kind == PlaybackFailureKind::DeviceLost;
    }

    bool isTrackFailureKind(PlaybackFailureKind kind) noexcept
    {
      return kind == PlaybackFailureKind::TrackOpen || kind == PlaybackFailureKind::Decode;
    }

    bool shouldPostDefaultFailureNotification(PlaybackFailure const& failure, bool const hasFailureSubscriber) noexcept
    {
      return !hasFailureSubscriber || !isTrackFailureKind(failure.kind);
    }

    std::chrono::milliseconds clampSessionElapsed(std::chrono::milliseconds elapsed,
                                                  std::chrono::milliseconds duration) noexcept
    {
      if (elapsed <= std::chrono::milliseconds{0})
      {
        return std::chrono::milliseconds{0};
      }

      if (duration > std::chrono::milliseconds{0} && elapsed > duration)
      {
        return duration;
      }

      return elapsed;
    }

    std::string playbackFailureReason(Error const& error)
    {
      return error.message.empty() ? std::string{"unknown error"} : error.message;
    }

    std::string playbackFailureTrackLabel(PlaybackFailure const& failure)
    {
      if (!failure.title.empty())
      {
        return failure.title;
      }

      if (failure.trackId != kInvalidTrackId)
      {
        return "track " + std::to_string(failure.trackId.raw());
      }

      return "playback";
    }

    std::string playbackFailureNotificationMessage(PlaybackFailure const& failure)
    {
      auto const reason = playbackFailureReason(failure.error);

      switch (failure.kind)
      {
        case PlaybackFailureKind::TrackOpen:
          return "Could not play " + playbackFailureTrackLabel(failure) + ": " + reason;
        case PlaybackFailureKind::Decode:
          return "Playback failed for " + playbackFailureTrackLabel(failure) + ": " + reason;
        case PlaybackFailureKind::RouteActivation: return "Could not start playback: " + reason;
        case PlaybackFailureKind::DeviceLost: return "Playback device failed: " + reason;
      }

      return "Playback failed: " + reason;
    }

    void logOutputDeviceSelected(OutputDeviceSelection const& outputDevice)
    {
      APP_LOG_INFO("Audio output device selected: backend={} device={} profile={}",
                   outputDevice.backendId,
                   outputDevice.deviceId,
                   outputDevice.profileId);
    }

    void logOutputDeviceCleared(OutputDeviceSelection const& outputDevice)
    {
      APP_LOG_INFO("Audio output device cleared: backend={} device={} profile={}",
                   outputDevice.backendId,
                   outputDevice.deviceId,
                   outputDevice.profileId);
    }

    void logOutputDeviceSwitched(OutputDeviceSelection const& previous, OutputDeviceSelection const& current)
    {
      APP_LOG_INFO(
        "Audio output device switched: previous_backend={} previous_device={} previous_profile={} backend={} "
        "device={} profile={}",
        previous.backendId,
        previous.deviceId,
        previous.profileId,
        current.backendId,
        current.deviceId,
        current.profileId);
    }

    void logOutputDeviceTransition(OutputDeviceSelection const& previous, OutputDeviceSelection const& current)
    {
      auto const previousHas = hasOutputDevice(previous);

      if (auto const currentHas = hasOutputDevice(current); previousHas && currentHas)
      {
        logOutputDeviceSwitched(previous, current);
      }
      else if (currentHas)
      {
        logOutputDeviceSelected(current);
      }
      else if (previousHas)
      {
        logOutputDeviceCleared(previous);
      }
    }

    Result<PlaybackService::PlaybackRequest> playbackRequestForTrack(library::MusicLibrary& library, TrackId trackId)
    {
      auto const txn = library.readTransaction();
      auto reader = library.tracks().reader(txn);
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

    [[noreturn]] void failExecutorAffinity(std::source_location const& loc)
    {
      APP_LOG_CRITICAL("PlaybackService thread-affinity violation: '{}' invoked off the executor thread ({}:{})",
                       loc.function_name(),
                       loc.file_name(),
                       loc.line());

      if (auto const& logger = Log::appLogger(); logger)
      {
        logger->flush();
      }

      std::abort();
    }
  } // namespace

  struct PlaybackService::Impl final
  {
    struct PreparedPlaybackRequest final
    {
      PlaybackService::PlaybackRequest request;
      ListId sourceListId = kInvalidListId;
      audio::Engine::PlaybackItemId itemId;
    };

    struct PlaybackFailureNotification final
    {
      PlaybackFailureKind kind = PlaybackFailureKind::TrackOpen;
      TrackId trackId = kInvalidTrackId;
      NotificationId notificationId = kInvalidNotificationId;
    };

    struct PlaybackRequestContext final
    {
      PlaybackService::PlaybackRequest request;
      ListId sourceListId = kInvalidListId;
    };

    struct DeferredResumeRequest final
    {
      PlaybackService::PlaybackRequest request;
      ListId sourceListId = kInvalidListId;
      std::chrono::milliseconds elapsed{0};
    };

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    async::IExecutor& executor;
    PlaybackState state;
    std::unique_ptr<audio::Player> playerPtr;
    ViewService& views;
    library::MusicLibrary& library;
    NotificationService& notifications;
    PlaybackService::PlaybackRequest currentRequest;
    audio::Engine::PlaybackItemId currentPlaybackItemId;
    ShuffleMode shuffleMode = ShuffleMode::Off;
    RepeatMode repeatMode = RepeatMode::Off;
    std::string lastPlaybackError{};
    std::optional<PlaybackFailureNotification> optLastPlaybackFailureNotification;
    std::vector<PreparedPlaybackRequest> preparedRequests;
    std::optional<DeferredResumeRequest> optDeferredResume;
    std::optional<PlaybackSessionState> optLastRestorableSession;
    std::uint64_t nextPlaybackItemId = 1;
    Signal<> preparingSignal;
    Signal<> startedSignal;
    Signal<> pausedSignal;
    Signal<> idleSignal;
    Signal<PlaybackService::NowPlayingChanged const&> nowPlayingChangedSignal;
    Signal<OutputDeviceSelection const&> outputDeviceChangedSignal;
    Signal<> stoppedSignal;
    Signal<> outputDevicesChangedSignal;
    Signal<PlaybackService::QualityChanged const&> qualityChangedSignal;
    Signal<float> volumeChangedSignal;
    Signal<bool> mutedChangedSignal;
    Signal<PlaybackService::RevealTrackRequested const&> revealTrackRequestedSignal;
    Signal<PlaybackService::SeekUpdate const&> seekUpdateSignal;
    Signal<PlaybackService::ShuffleModeChanged const&> shuffleModeChangedSignal;
    Signal<PlaybackService::RepeatModeChanged const&> repeatModeChangedSignal;
    Signal<PlaybackFailure const&> playbackFailureSignal;

    // Facade affinity contract: every public mutator and state() must run on the
    // executor's owning thread. `state` is written only here (control commands)
    // and on the executor-marshalled Player callbacks, so confining all callers
    // to one thread upholds the single-writer invariant with no locking.
    //
    // This guard is always on, not a debug-only assert: a cross-thread call is a
    // data race that would silently corrupt `state` (and risk a use-after-free on
    // the subscription lists) in a release build, so we fail fast with a logged
    // abort rather than press on. isCurrent() is a cheap thread-id comparison.
    // ImmediateExecutor reports isCurrent()==true unconditionally, so CLI/test
    // hosts must remain effectively single-threaded for control.
    void ensureOnExecutor(std::source_location loc = std::source_location::current()) const
    {
      if (!executor.isCurrent()) [[unlikely]]
      {
        failExecutorAffinity(loc);
      }
    }

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

      if (auto const result = playerPtr->setOutputDevice(backend.metadata.id, device.id, profileId); !result)
      {
        APP_LOG_ERROR("Failed to select audio output device: {}", result.error().message);
      }
    }

    void refreshState()
    {
      auto const previousState = state;
      auto const status = playerPtr->status();

      state = buildPlaybackState(status);
      state.nowPlaying = currentRequest.item;
      state.mode = PlaybackModeState{.shuffle = shuffleMode, .repeat = repeatMode};

      if (optDeferredResume && state.transport == audio::Transport::Idle)
      {
        state.elapsed = optDeferredResume->elapsed;
        state.duration = optDeferredResume->request.input.duration;
      }

      if (state.duration == std::chrono::milliseconds{0})
      {
        state.duration = currentRequest.input.duration;
      }

      if (!sameOutputDevice(previousState.output.selectedDevice, state.output.selectedDevice))
      {
        logOutputDeviceTransition(previousState.output.selectedDevice, state.output.selectedDevice);
      }

      if (state.transport == audio::Transport::Error)
      {
        recordPlaybackError(previousState.transport, status.engine, state.output.selectedDevice);
      }
      else
      {
        lastPlaybackError.clear();
      }
    }

    void recordPlaybackError(audio::Transport previousTransport,
                             audio::Engine::Status const& engineStatus,
                             OutputDeviceSelection const& currentOutputDevice)
    {
      auto const message = std::string{playbackErrorMessage(engineStatus)};

      if (previousTransport == audio::Transport::Error && message == lastPlaybackError)
      {
        return;
      }

      lastPlaybackError = message;
      APP_LOG_ERROR("Playback error on backend={} device={} profile={}: {}",
                    currentOutputDevice.backendId,
                    currentOutputDevice.deviceId,
                    currentOutputDevice.profileId,
                    lastPlaybackError);
    }

    void publishCurrentRequest(PlaybackService::PlaybackRequest const& request,
                               ListId sourceListId,
                               audio::Engine::PlaybackItemId itemId)
    {
      currentRequest = request;
      currentRequest.item.sourceListId = sourceListId;
      currentPlaybackItemId = itemId;
    }

    audio::Engine::PlaybackItem makePlaybackItem(audio::PlaybackInput input)
    {
      return audio::Engine::PlaybackItem{
        .id = audio::Engine::PlaybackItemId{.value = nextPlaybackItemId++},
        .input = std::move(input),
      };
    }

    void rememberPreparedRequest(PreparedPlaybackRequest request)
    {
      std::erase_if(
        preparedRequests, [&](PreparedPlaybackRequest const& existing) { return existing.itemId == request.itemId; });
      preparedRequests.push_back(std::move(request));
    }

    void forgetPreparedRequest(audio::Engine::PlaybackItemId itemId)
    {
      std::erase_if(preparedRequests, [&](PreparedPlaybackRequest const& request) { return request.itemId == itemId; });
    }

    std::optional<PreparedPlaybackRequest> takePreparedRequest(audio::Engine::PlaybackItemId itemId)
    {
      auto const it = std::ranges::find_if(
        preparedRequests, [&](PreparedPlaybackRequest const& request) { return request.itemId == itemId; });

      if (it == preparedRequests.end())
      {
        return std::nullopt;
      }

      auto request = std::move(*it);
      preparedRequests.erase(it);
      return request;
    }

    void discardPreparedRequests() { preparedRequests.clear(); }

    PlaybackSessionState currentSessionState() const
    {
      auto elapsed = state.elapsed < std::chrono::milliseconds{0} ? std::chrono::milliseconds{0} : state.elapsed;
      auto const duration =
        state.duration > std::chrono::milliseconds{0} ? state.duration : currentRequest.input.duration;

      if (duration > std::chrono::milliseconds{0} && elapsed >= duration)
      {
        elapsed = std::chrono::milliseconds{0};
      }

      return PlaybackSessionState{
        .sourceListId = currentRequest.item.sourceListId,
        .trackId = currentRequest.item.trackId,
        .positionMs = static_cast<std::uint64_t>(elapsed.count()),
        .shuffleMode = shuffleMode,
        .repeatMode = repeatMode,
        .volume = normalizePlaybackVolume(state.volume.level),
        .muted = state.volume.muted,
      };
    }

    void rememberRestorableSession()
    {
      if (currentRequest.item.trackId != kInvalidTrackId)
      {
        optLastRestorableSession = currentSessionState();
      }
    }

    PlaybackSessionState snapshotSessionState() const
    {
      if (currentRequest.item.trackId == kInvalidTrackId)
      {
        return optLastRestorableSession.value_or(PlaybackSessionState{});
      }

      return currentSessionState();
    }

    void restoreDeferredPlayback(PlaybackService::PlaybackRequest request, PlaybackSessionState const& session)
    {
      discardPreparedRequests();
      clearPreparedNext();
      auto const restoredElapsed =
        clampSessionElapsed(std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(session.positionMs)},
                            request.input.duration);
      optDeferredResume = DeferredResumeRequest{
        .request = request,
        .sourceListId = session.sourceListId,
        .elapsed = restoredElapsed,
      };

      shuffleMode = session.shuffleMode;
      repeatMode = session.repeatMode;
      applySessionVolumeAndMute(session);

      publishCurrentRequest(request, session.sourceListId, audio::Engine::PlaybackItemId{});
      refreshState();
      state.elapsed = restoredElapsed;
      state.duration = request.input.duration;
      state.transport = audio::Transport::Idle;
      rememberRestorableSession();

      announceNowPlaying(request, session.sourceListId);
      seekUpdateSignal.emit(PlaybackService::SeekUpdate{
        .elapsed = restoredElapsed,
        .mode = PlaybackService::SeekMode::Final,
      });
      shuffleModeChangedSignal.emit(PlaybackService::ShuffleModeChanged{.mode = shuffleMode});
      repeatModeChangedSignal.emit(PlaybackService::RepeatModeChanged{.mode = repeatMode});
      volumeChangedSignal.emit(state.volume.level);
      mutedChangedSignal.emit(state.volume.muted);
    }

    // Restored volume/mute intents come from disk, so surface application
    // failures instead of dropping them silently: the public setVolume path
    // logs the same error and the user-visible state reflects the player's
    // actual state, not the requested value.
    void applySessionVolumeAndMute(PlaybackSessionState const& session) const
    {
      if (auto const volumeResult = playerPtr->setVolume(session.volume); !volumeResult)
      {
        APP_LOG_WARN("PlaybackService: Restored volume apply failed - {}", volumeResult.error().message);
      }

      if (auto const muteResult = playerPtr->setMuted(session.muted); !muteResult)
      {
        APP_LOG_WARN("PlaybackService: Restored mute apply failed - {}", muteResult.error().message);
      }
    }

    void announceNowPlaying(PlaybackService::PlaybackRequest const& request, ListId sourceListId)
    {
      nowPlayingChangedSignal.emit(PlaybackService::NowPlayingChanged{
        .trackId = request.item.trackId,
        .sourceListId = sourceListId,
      });
    }

    std::optional<PlaybackRequestContext> contextForPlaybackItem(audio::Engine::PlaybackItemId itemId) const
    {
      if (itemId == currentPlaybackItemId && currentRequest.item.trackId != kInvalidTrackId)
      {
        return PlaybackRequestContext{.request = currentRequest, .sourceListId = currentRequest.item.sourceListId};
      }

      auto const it = std::ranges::find_if(
        preparedRequests, [&](PreparedPlaybackRequest const& request) { return request.itemId == itemId; });

      if (it == preparedRequests.end())
      {
        return std::nullopt;
      }

      return PlaybackRequestContext{.request = it->request, .sourceListId = it->sourceListId};
    }

    void postOrUpdateFailureNotification(PlaybackFailure const& failure)
    {
      auto const message = playbackFailureNotificationMessage(failure);

      if (optLastPlaybackFailureNotification && optLastPlaybackFailureNotification->kind == failure.kind &&
          (isOutputFailureKind(failure.kind) || optLastPlaybackFailureNotification->trackId == failure.trackId) &&
          notifications.updateMessage(optLastPlaybackFailureNotification->notificationId, message))
      {
        return;
      }

      auto const notificationId = notifications.post(NotificationRequest{
        .severity = NotificationSeverity::Error,
        .message = message,
        .sticky = !failure.recoverable,
        .content = NotificationContentState{.title = "Playback error", .iconName = "dialog-error-symbolic"},
      });
      optLastPlaybackFailureNotification =
        PlaybackFailureNotification{.kind = failure.kind, .trackId = failure.trackId, .notificationId = notificationId};
    }

    void publishPlaybackFailure(PlaybackFailure failure)
    {
      lastPlaybackError = playbackFailureReason(failure.error);
      APP_LOG_ERROR("Playback failure kind={} track={} source_list={} recoverable={} reason={}",
                    static_cast<std::uint32_t>(failure.kind),
                    failure.trackId,
                    failure.sourceListId,
                    failure.recoverable,
                    lastPlaybackError);

      auto const hasFailureSubscriber = playbackFailureSignal.hasConnectedHandlers();
      playbackFailureSignal.emit(failure);

      if (shouldPostDefaultFailureNotification(failure, hasFailureSubscriber))
      {
        postOrUpdateFailureNotification(failure);
      }
    }

    void handlePlaybackFailure(audio::Engine::PlaybackFailure const& failure)
    {
      refreshState();

      auto translated = PlaybackFailure{
        .kind = toPlaybackFailureKind(failure.kind),
        .generation = failure.generation,
        .error = failure.error,
        .recoverable = failure.recoverable,
      };

      auto const optContext = contextForPlaybackItem(failure.itemId);

      if (!optContext)
      {
        APP_LOG_WARN("Dropping stale playback failure kind={} item={} generation={} reason={}",
                     static_cast<std::uint32_t>(translated.kind),
                     failure.itemId.value,
                     failure.generation,
                     playbackFailureReason(translated.error));
        return;
      }

      translated.trackId = optContext->request.item.trackId;
      translated.sourceListId = optContext->sourceListId;
      translated.title = optContext->request.item.title;
      publishPlaybackFailure(std::move(translated));
    }

    void clearPreparedNext()
    {
      if (auto const optDisarmedItemId = playerPtr->clearPreparedNext(); optDisarmedItemId)
      {
        forgetPreparedRequest(*optDisarmedItemId);
      }
    }

    void handleTrackAdvanced(audio::Engine::TrackAdvanced const& event)
    {
      if (auto const optPrepared = takePreparedRequest(event.itemId); optPrepared)
      {
        publishCurrentRequest(optPrepared->request, optPrepared->sourceListId, optPrepared->itemId);
        refreshState();
        nowPlayingChangedSignal.emit(PlaybackService::NowPlayingChanged{
          .trackId = optPrepared->request.item.trackId,
          .sourceListId = optPrepared->sourceListId,
        });
        return;
      }

      refreshState();
    }

    void handleTrackEnded()
    {
      refreshState();

      if (isTerminalTrackTransport(state.transport))
      {
        idleSignal.emit();
      }
    }

    explicit Impl(async::IExecutor& callbackExecutor,
                  ViewService& viewService,
                  library::MusicLibrary& musicLibrary,
                  NotificationService& notificationService)
      : executor{callbackExecutor}
      , playerPtr{std::make_unique<audio::Player>(callbackExecutor)}
      , views{viewService}
      , library{musicLibrary}
      , notifications{notificationService}
    {
      // Player marshals these callbacks onto the executor thread, so they run on
      // the same thread as the control commands below and can refresh state and
      // emit synchronously without any gate or further dispatch.
      playerPtr->setOnTrackEnded([this] { handleTrackEnded(); });

      playerPtr->setOnTrackAdvanced([this](audio::Engine::TrackAdvanced const& event) { handleTrackAdvanced(event); });

      playerPtr->setOnPlaybackFailure([this](audio::Engine::PlaybackFailure const& failure)
                                      { handlePlaybackFailure(failure); });

      playerPtr->setOnStateChanged([this] { refreshState(); });

      playerPtr->setOnOutputDevicesChanged(
        [this](std::vector<audio::IBackendProvider::Status> const&)
        {
          refreshState();

          // Auto-select first available default output device if none is selected yet.
          if (!state.output.selectedDevice.backendId.empty() || state.output.availableBackends.empty())
          {
            outputDevicesChangedSignal.emit();
            return;
          }

          auto const optSelection = defaultOutputDeviceSelection(state.output.availableBackends);

          if (!optSelection)
          {
            outputDevicesChangedSignal.emit();
            return;
          }

          if (auto const result =
                playerPtr->setOutputDevice(optSelection->backendId, optSelection->deviceId, optSelection->profileId);
              !result)
          {
            APP_LOG_ERROR("Failed to select audio output device: {}", result.error().message);
          }

          refreshState();
          outputDeviceChangedSignal.emit(state.output.selectedDevice);
          outputDevicesChangedSignal.emit();
        });

      playerPtr->setOnQualityChanged(
        [this](audio::QualityResult const&, bool)
        {
          refreshState();
          qualityChangedSignal.emit(PlaybackService::QualityChanged{.quality = state.quality, .ready = state.ready});
        });
    }

    ~Impl()
    {
      if (hasOutputDevice(state.output.selectedDevice))
      {
        APP_LOG_INFO("Audio output device released: backend={} device={} profile={}",
                     state.output.selectedDevice.backendId,
                     state.output.selectedDevice.deviceId,
                     state.output.selectedDevice.profileId);
      }

      // Tear the player down first: Player::~Impl drains its own callback gate
      // (joining the Engine worker and neutralizing any executor-deferred outward
      // callback), so no backend/source event can re-enter the service after this.
      playerPtr.reset();
    }
  };

  PlaybackService::PlaybackService(async::IExecutor& executor,
                                   ViewService& views,
                                   library::MusicLibrary& library,
                                   NotificationService& notifications)
    : _implPtr{std::make_unique<Impl>(executor, views, library, notifications)}
  {
  }

  PlaybackService::~PlaybackService() = default;

  Subscription PlaybackService::onPreparing(std::move_only_function<void()> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->preparingSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onStarted(std::move_only_function<void()> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->startedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onPaused(std::move_only_function<void()> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->pausedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onIdle(std::move_only_function<void()> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->idleSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onNowPlayingChanged(std::move_only_function<void(NowPlayingChanged const&)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->nowPlayingChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onOutputDeviceChanged(
    std::move_only_function<void(OutputDeviceSelection const&)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->outputDeviceChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onStopped(std::move_only_function<void()> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->stoppedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onOutputDevicesChanged(std::move_only_function<void()> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->outputDevicesChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onQualityChanged(std::move_only_function<void(QualityChanged const&)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->qualityChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onVolumeChanged(std::move_only_function<void(float)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->volumeChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onMutedChanged(std::move_only_function<void(bool)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->mutedChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onRevealTrackRequested(
    std::move_only_function<void(RevealTrackRequested const&)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->revealTrackRequestedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onSeekUpdate(std::move_only_function<void(SeekUpdate const&)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->seekUpdateSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onShuffleModeChanged(std::move_only_function<void(ShuffleModeChanged const&)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->shuffleModeChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onRepeatModeChanged(std::move_only_function<void(RepeatModeChanged const&)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->repeatModeChangedSignal.connect(std::move(handler));
  }

  Subscription PlaybackService::onPlaybackFailure(std::move_only_function<void(PlaybackFailure const&)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->playbackFailureSignal.connect(std::move(handler));
  }

  PlaybackState const& PlaybackService::state() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->state;
  }

  Result<> PlaybackService::play(PlaybackRequest const& request,
                                 ListId const sourceListId,
                                 std::chrono::milliseconds const initialOffset)
  {
    auto& impl = *_implPtr;
    impl.ensureOnExecutor();
    impl.ensureReady();

    // Signal "about to play" so the UI resets the seekbar before the
    // blocking Engine::play call freezes the main thread.
    impl.discardPreparedRequests();
    impl.clearPreparedNext();
    impl.optDeferredResume.reset();
    impl.preparingSignal.emit();

    auto item = impl.makePlaybackItem(request.input);

    if (auto const result = impl.playerPtr->play(item, initialOffset); !result)
    {
      APP_LOG_WARN("Playback not started: {}", result.error().message);
      impl.refreshState();
      impl.postOrUpdateFailureNotification(PlaybackFailure{
        .kind = PlaybackFailureKind::RouteActivation,
        .trackId = request.item.trackId,
        .sourceListId = sourceListId,
        .error = result.error(),
        .recoverable = false,
        .title = request.item.title,
      });
      return std::unexpected{result.error()};
    }

    impl.publishCurrentRequest(request, sourceListId, item.id);
    impl.refreshState();
    impl.startedSignal.emit();
    impl.announceNowPlaying(request, sourceListId);
    return {};
  }

  Result<> PlaybackService::playTrack(TrackId const trackId, ListId const sourceListId)
  {
    try
    {
      auto const requestResult = playbackRequestForTrack(_implPtr->library, trackId);

      if (!requestResult)
      {
        return std::unexpected{requestResult.error()};
      }

      return play(*requestResult, sourceListId);
    }
    catch (std::exception const& ex)
    {
      return makeError(Error::Code::Generic, ex.what());
    }
  }

  bool PlaybackService::prepareNext(PlaybackRequest const& request, ListId const sourceListId)
  {
    auto& impl = *_implPtr;
    impl.ensureOnExecutor();
    impl.ensureReady();

    impl.clearPreparedNext();
    auto item = impl.makePlaybackItem(request.input);
    auto const result = impl.playerPtr->prepareNext(item);

    if (!result)
    {
      APP_LOG_WARN("Playback not prepared: {}", result.error().message);
      return false;
    }

    impl.rememberPreparedRequest(
      Impl::PreparedPlaybackRequest{.request = request, .sourceListId = sourceListId, .itemId = result->itemId});
    return true;
  }

  bool PlaybackService::prepareNext(TrackId const trackId, ListId const sourceListId)
  {
    _implPtr->ensureOnExecutor();

    try
    {
      auto const requestResult = playbackRequestForTrack(_implPtr->library, trackId);

      if (!requestResult)
      {
        _implPtr->clearPreparedNext();
        return false;
      }

      return prepareNext(*requestResult, sourceListId);
    }
    catch (std::exception const&)
    {
      _implPtr->clearPreparedNext();
      return false;
    }
  }

  void PlaybackService::clearPreparedNext()
  {
    _implPtr->ensureOnExecutor();
    _implPtr->clearPreparedNext();
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
    _implPtr->ensureOnExecutor();
    _implPtr->playerPtr->pause();
    _implPtr->refreshState();
    _implPtr->pausedSignal.emit();
  }

  void PlaybackService::resume()
  {
    auto& impl = *_implPtr;
    impl.ensureOnExecutor();

    // A deferred resume token only fires when we still own the idle state it
    // was armed against; an intervening player transition (e.g. an async
    // backend ready callback) means resume() should just hand control back to
    // the player rather than restart the restored track from scratch.
    if (impl.optDeferredResume && impl.state.transport == audio::Transport::Idle)
    {
      auto deferred = std::move(*impl.optDeferredResume);
      impl.optDeferredResume.reset();
      std::ignore = play(deferred.request, deferred.sourceListId, deferred.elapsed);
      return;
    }

    impl.playerPtr->resume();
    impl.refreshState();
    impl.startedSignal.emit();
  }

  void PlaybackService::stop()
  {
    _implPtr->ensureOnExecutor();
    _implPtr->refreshState();
    _implPtr->rememberRestorableSession();
    _implPtr->currentRequest = PlaybackRequest{};
    _implPtr->currentPlaybackItemId = {};
    _implPtr->discardPreparedRequests();
    _implPtr->optDeferredResume.reset();
    _implPtr->clearPreparedNext();
    _implPtr->playerPtr->stop();
    _implPtr->refreshState();
    _implPtr->stoppedSignal.emit();
    _implPtr->idleSignal.emit();
    _implPtr->nowPlayingChangedSignal.emit(PlaybackService::NowPlayingChanged{
      .trackId = kInvalidTrackId,
      .sourceListId = kInvalidListId,
    });
  }

  void PlaybackService::setShuffleMode(ShuffleMode const mode)
  {
    _implPtr->ensureOnExecutor();

    if (_implPtr->shuffleMode == mode)
    {
      return;
    }

    _implPtr->shuffleMode = mode;
    _implPtr->refreshState();
    _implPtr->shuffleModeChangedSignal.emit(ShuffleModeChanged{.mode = mode});
  }

  void PlaybackService::setRepeatMode(RepeatMode const mode)
  {
    _implPtr->ensureOnExecutor();

    if (_implPtr->repeatMode == mode)
    {
      return;
    }

    _implPtr->repeatMode = mode;
    _implPtr->refreshState();
    _implPtr->repeatModeChangedSignal.emit(RepeatModeChanged{.mode = mode});
  }

  void PlaybackService::seek(std::chrono::milliseconds const elapsed, SeekMode const mode)
  {
    _implPtr->ensureOnExecutor();

    if (mode == SeekMode::Final)
    {
      if (_implPtr->optDeferredResume && _implPtr->state.transport == audio::Transport::Idle)
      {
        // While armed with a deferred resume token, redirect seeks into the
        // token so the engine starts at the user's chosen position when resume
        // finally consumes it. Avoids opening the audio route just to seek an
        // idle pipeline.
        auto const clampedElapsed = clampSessionElapsed(elapsed, _implPtr->optDeferredResume->request.input.duration);
        _implPtr->optDeferredResume->elapsed = clampedElapsed;
        _implPtr->state.elapsed = clampedElapsed;
        _implPtr->seekUpdateSignal.emit(SeekUpdate{.elapsed = clampedElapsed, .mode = mode});
        return;
      }

      _implPtr->clearPreparedNext();
      _implPtr->playerPtr->seek(elapsed);
      // seek() does stop/flush/start with no open(), so it fires no async
      // onStateChanged; refresh the snapshot explicitly to pick up the new
      // transport/elapsed, matching every other control command.
      _implPtr->refreshState();
    }

    _implPtr->seekUpdateSignal.emit(SeekUpdate{.elapsed = elapsed, .mode = mode});
  }

  void PlaybackService::setOutputDevice(audio::BackendId const& backendId,
                                        audio::DeviceId const& deviceId,
                                        audio::ProfileId const& profileId)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->clearPreparedNext();

    if (auto const result = _implPtr->playerPtr->setOutputDevice(backendId, deviceId, profileId); !result)
    {
      APP_LOG_ERROR("Failed to set audio output device: {}", result.error().message);
    }

    _implPtr->refreshState();
    // Publish the engine-confirmed selection from the refreshed state, not the
    // raw request. This keeps the signal consistent with the auto-select path in
    // onOutputDevicesChanged (which emits state.output.selectedDevice) and reports what the
    // engine actually selected. The two coincide while Engine::setBackend is
    // synchronous; if it ever becomes async, this still reflects reality.
    _implPtr->outputDeviceChangedSignal.emit(_implPtr->state.output.selectedDevice);
  }

  void PlaybackService::setVolume(float const volume)
  {
    _implPtr->ensureOnExecutor();
    auto const normalizedVolume = normalizePlaybackVolume(volume);

    if (auto const result = _implPtr->playerPtr->setVolume(normalizedVolume); !result)
    {
      APP_LOG_ERROR("Failed to set volume: {}", result.error().message);
    }

    _implPtr->refreshState();
    _implPtr->volumeChangedSignal.emit(_implPtr->state.volume.level);
  }

  void PlaybackService::setMuted(bool const muted)
  {
    _implPtr->ensureOnExecutor();

    if (auto const result = _implPtr->playerPtr->setMuted(muted); !result)
    {
      APP_LOG_ERROR("Failed to set muted state: {}", result.error().message);
    }

    _implPtr->refreshState();
    _implPtr->mutedChangedSignal.emit(_implPtr->state.volume.muted);
  }

  void PlaybackService::revealPlayingTrack()
  {
    revealTrack(_implPtr->state.nowPlaying.trackId, kInvalidViewId, _implPtr->state.nowPlaying.sourceListId);
  }

  void PlaybackService::revealTrack(TrackId const trackId, ViewId const preferredViewId, ListId const preferredListId)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->revealTrackRequestedSignal.emit(PlaybackService::RevealTrackRequested{
      .trackId = trackId, .preferredListId = preferredListId, .preferredViewId = preferredViewId});
  }

  PlaybackSessionState PlaybackService::sessionState() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->snapshotSessionState();
  }

  void PlaybackService::saveSession(ConfigStore& store)
  {
    _implPtr->ensureOnExecutor();
    // refreshState picks up the latest elapsed/volume from the player so the
    // saved snapshot reflects reality. rememberRestorableSession is not needed
    // here: snapshotSessionState reads a live currentSessionState() when a
    // track is active, and falls back to optLastRestorableSession otherwise,
    // which stop() is responsible for capturing before clearing currentRequest.
    _implPtr->refreshState();
    auto const session = _implPtr->snapshotSessionState();

    if (session.trackId == kInvalidTrackId)
    {
      return;
    }

    store.save(kPlaybackSessionConfigGroup, session);

    if (auto const res = store.flush(); !res)
    {
      APP_LOG_ERROR("PlaybackService: Failed to flush session - {}", res.error().message);
    }
  }

  Result<> PlaybackService::restoreSession(PlaybackSessionState const& session)
  {
    auto& impl = *_implPtr;
    impl.ensureOnExecutor();
    auto const normalizedSession = normalizePlaybackSessionState(session);

    if (normalizedSession.schemaVersion != kPlaybackSessionSchemaVersion)
    {
      return makeError(Error::Code::FormatRejected, "Unsupported playback session schema version");
    }

    if (normalizedSession.trackId == kInvalidTrackId)
    {
      return makeError(Error::Code::NotFound, "No track available for playback session restore");
    }

    try
    {
      auto requestResult = playbackRequestForTrack(impl.library, normalizedSession.trackId);

      if (!requestResult)
      {
        return std::unexpected{requestResult.error()};
      }

      impl.restoreDeferredPlayback(std::move(*requestResult), normalizedSession);
      return {};
    }
    catch (std::exception const& ex)
    {
      return makeError(Error::Code::Generic, ex.what());
    }
  }

  void PlaybackService::addProvider(std::unique_ptr<audio::IBackendProvider> providerPtr)
  {
    _implPtr->ensureOnExecutor();

    if (providerPtr != nullptr)
    {
      auto const status = providerPtr->status();
      APP_LOG_INFO("Audio backend provider registered: backend={} name='{}' devices={}",
                   status.metadata.id,
                   status.metadata.name,
                   status.devices.size());
    }

    _implPtr->playerPtr->addProvider(std::move(providerPtr));
  }
} // namespace ao::rt
