// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Player.h>
#include <ao/audio/Transport.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/ViewService.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
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
        .trackId = {},
        .elapsed = status.engine.elapsed,
        .duration = status.engine.duration,
        .volume = status.volume,
        .muted = status.muted,
        .volumeAvailable = status.volumeAvailable,
        .volumeIsHardwareAssisted = status.volumeIsHardwareAssisted,
        .ready = status.isReady,
        .selectedOutputDevice =
          OutputDeviceSelection{
            .backendId = status.engine.backendId,
            .deviceId = status.engine.currentDeviceId,
            .profileId = status.engine.profileId,
          },
        .availableOutputBackends = std::move(outputBackends),
        .flow = status.flow,
        .quality = status.quality,
        .qualityAssessments = status.qualityAssessments,
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

    std::optional<PlaybackService::PlaybackRequest> playbackRequestForTrack(library::MusicLibrary& library,
                                                                            TrackId trackId)
    {
      auto const txn = library.readTransaction();
      auto reader = library.tracks().reader(txn);
      auto const optView = storageValueOrNullopt(
        reader.get(trackId, library::TrackStore::Reader::LoadMode::Both), "Failed to build playback request");

      if (!optView)
      {
        return std::nullopt;
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
        .trackId = trackId,
        .input =
          audio::PlaybackInput{
            .duration = property.duration(),
            .sampleRateHint = property.sampleRate().raw(),
            .channelsHint = property.channels().raw(),
            .bitDepthHint = property.bitDepth().raw(),
          },
        .title = std::string{metadata.title()},
        .artist = std::string{library.dictionary().getOrDefault(metadata.artistId())},
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

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    async::IExecutor& executor;
    PlaybackState state;
    std::unique_ptr<audio::Player> playerPtr;
    ViewService& views;
    library::MusicLibrary& library;
    TrackId currentTrackId = kInvalidTrackId;
    ListId currentSourceListId = kInvalidListId;
    ShuffleMode shuffleMode = ShuffleMode::Off;
    RepeatMode repeatMode = RepeatMode::Off;
    std::string currentTrackTitle{};
    std::string currentTrackArtist{};
    std::chrono::milliseconds currentTrackDuration{0};
    std::string lastPlaybackError{};
    std::vector<PreparedPlaybackRequest> preparedRequests;
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
      state.trackId = currentTrackId;
      state.sourceListId = currentSourceListId;
      state.trackTitle = currentTrackTitle;
      state.trackArtist = currentTrackArtist;
      state.shuffleMode = shuffleMode;
      state.repeatMode = repeatMode;

      if (state.duration == std::chrono::milliseconds{0})
      {
        state.duration = currentTrackDuration;
      }

      if (!sameOutputDevice(previousState.selectedOutputDevice, state.selectedOutputDevice))
      {
        logOutputDeviceTransition(previousState.selectedOutputDevice, state.selectedOutputDevice);
      }

      if (state.transport == audio::Transport::Error)
      {
        recordPlaybackError(previousState.transport, status.engine, state.selectedOutputDevice);
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

    void publishCurrentRequest(PlaybackService::PlaybackRequest const& request, ListId sourceListId)
    {
      currentTrackId = request.trackId;
      currentSourceListId = sourceListId;
      currentTrackTitle = request.title;
      currentTrackArtist = request.artist;
      currentTrackDuration = request.input.duration;
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
        publishCurrentRequest(optPrepared->request, optPrepared->sourceListId);
        refreshState();
        nowPlayingChangedSignal.emit(PlaybackService::NowPlayingChanged{
          .trackId = optPrepared->request.trackId,
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

    explicit Impl(async::IExecutor& callbackExecutor, ViewService& viewService, library::MusicLibrary& musicLibrary)
      : executor{callbackExecutor}
      , playerPtr{std::make_unique<audio::Player>(callbackExecutor)}
      , views{viewService}
      , library{musicLibrary}
    {
      // Player marshals these callbacks onto the executor thread, so they run on
      // the same thread as the control commands below and can refresh state and
      // emit synchronously without any gate or further dispatch.
      playerPtr->setOnTrackEnded([this] { handleTrackEnded(); });

      playerPtr->setOnTrackAdvanced([this](audio::Engine::TrackAdvanced const& event) { handleTrackAdvanced(event); });

      playerPtr->setOnStateChanged([this] { refreshState(); });

      playerPtr->setOnOutputDevicesChanged(
        [this](std::vector<audio::IBackendProvider::Status> const&)
        {
          refreshState();

          // Auto-select first available default output device if none is selected yet.
          if (!state.selectedOutputDevice.backendId.empty() || state.availableOutputBackends.empty())
          {
            outputDevicesChangedSignal.emit();
            return;
          }

          auto const optSelection = defaultOutputDeviceSelection(state.availableOutputBackends);

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
          outputDeviceChangedSignal.emit(state.selectedOutputDevice);
          outputDevicesChangedSignal.emit();
        });

      playerPtr->setOnQualityChanged(
        [this](audio::Quality, bool)
        {
          refreshState();
          qualityChangedSignal.emit(PlaybackService::QualityChanged{.quality = state.quality, .ready = state.ready});
        });
    }

    ~Impl()
    {
      if (hasOutputDevice(state.selectedOutputDevice))
      {
        APP_LOG_INFO("Audio output device released: backend={} device={} profile={}",
                     state.selectedOutputDevice.backendId,
                     state.selectedOutputDevice.deviceId,
                     state.selectedOutputDevice.profileId);
      }

      // Tear the player down first: Player::~Impl drains its own callback gate
      // (joining the Engine worker and neutralizing any executor-deferred outward
      // callback), so no backend/source event can re-enter the service after this.
      playerPtr.reset();
    }
  };

  PlaybackService::PlaybackService(async::IExecutor& executor, ViewService& views, library::MusicLibrary& library)
    : _implPtr{std::make_unique<Impl>(executor, views, library)}
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

  PlaybackState const& PlaybackService::state() const
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->state;
  }

  bool PlaybackService::play(PlaybackRequest const& request, ListId const sourceListId)
  {
    auto& impl = *_implPtr;
    impl.ensureOnExecutor();
    impl.ensureReady();

    // Signal "about to play" so the UI resets the seekbar before the
    // blocking Engine::play call freezes the main thread.
    impl.discardPreparedRequests();
    impl.clearPreparedNext();
    impl.preparingSignal.emit();

    if (auto const result = impl.playerPtr->play(impl.makePlaybackItem(request.input)); !result)
    {
      APP_LOG_WARN("Playback not started: {}", result.error().message);
      impl.refreshState();
      return false;
    }

    impl.publishCurrentRequest(request, sourceListId);
    impl.refreshState();
    impl.startedSignal.emit();
    impl.nowPlayingChangedSignal.emit(PlaybackService::NowPlayingChanged{
      .trackId = request.trackId,
      .sourceListId = sourceListId,
    });
    return true;
  }

  bool PlaybackService::playTrack(TrackId const trackId, ListId const sourceListId)
  {
    try
    {
      auto const optRequest = playbackRequestForTrack(_implPtr->library, trackId);

      if (!optRequest)
      {
        return false;
      }

      return play(*optRequest, sourceListId);
    }
    catch (std::exception const&)
    {
      return false;
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
      auto const optRequest = playbackRequestForTrack(_implPtr->library, trackId);

      if (!optRequest)
      {
        _implPtr->clearPreparedNext();
        return false;
      }

      return prepareNext(*optRequest, sourceListId);
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
    _implPtr->ensureOnExecutor();
    _implPtr->playerPtr->resume();
    _implPtr->refreshState();
    _implPtr->startedSignal.emit();
  }

  void PlaybackService::stop()
  {
    _implPtr->ensureOnExecutor();
    _implPtr->currentTrackId = {};
    _implPtr->currentSourceListId = {};
    _implPtr->currentTrackTitle.clear();
    _implPtr->currentTrackArtist.clear();
    _implPtr->currentTrackDuration = std::chrono::milliseconds{0};
    _implPtr->discardPreparedRequests();
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
    _implPtr->shuffleMode = mode;
    _implPtr->refreshState();
    _implPtr->shuffleModeChangedSignal.emit(ShuffleModeChanged{.mode = mode});
  }

  void PlaybackService::setRepeatMode(RepeatMode const mode)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->repeatMode = mode;
    _implPtr->refreshState();
    _implPtr->repeatModeChangedSignal.emit(RepeatModeChanged{.mode = mode});
  }

  void PlaybackService::seek(std::chrono::milliseconds const elapsed, SeekMode const mode)
  {
    _implPtr->ensureOnExecutor();

    if (mode == SeekMode::Final)
    {
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
    // onOutputDevicesChanged (which emits state.selectedOutputDevice) and reports what the
    // engine actually selected. The two coincide while Engine::setBackend is
    // synchronous; if it ever becomes async, this still reflects reality.
    _implPtr->outputDeviceChangedSignal.emit(_implPtr->state.selectedOutputDevice);
  }

  void PlaybackService::setVolume(float const volume)
  {
    _implPtr->ensureOnExecutor();

    if (auto const result = _implPtr->playerPtr->setVolume(volume); !result)
    {
      APP_LOG_ERROR("Failed to set volume: {}", result.error().message);
    }

    _implPtr->refreshState();
    _implPtr->volumeChangedSignal.emit(volume);
  }

  void PlaybackService::setMuted(bool const muted)
  {
    _implPtr->ensureOnExecutor();

    if (auto const result = _implPtr->playerPtr->setMuted(muted); !result)
    {
      APP_LOG_ERROR("Failed to set muted state: {}", result.error().message);
    }

    _implPtr->refreshState();
    _implPtr->mutedChangedSignal.emit(_implPtr->state.muted);
  }

  void PlaybackService::revealPlayingTrack()
  {
    revealTrack(_implPtr->state.trackId, kInvalidViewId, _implPtr->state.sourceListId);
  }

  void PlaybackService::revealTrack(TrackId const trackId, ViewId const preferredViewId, ListId const preferredListId)
  {
    _implPtr->ensureOnExecutor();
    _implPtr->revealTrackRequestedSignal.emit(PlaybackService::RevealTrackRequested{
      .trackId = trackId, .preferredListId = preferredListId, .preferredViewId = preferredViewId});
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
