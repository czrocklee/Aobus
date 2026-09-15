// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MediaPlayerAdapter.h"

#include "AppKitText.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/resource/ResourceByteMemoryCache.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#import <AppKit/AppKit.h>
#import <MediaPlayer/MediaPlayer.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace ao::appkit
{
  namespace
  {
    constexpr auto kCommandCount = std::size_t{7};

    constexpr std::size_t commandIndex(MediaRemoteCommand const command) noexcept
    {
      return static_cast<std::size_t>(command);
    }

    static_assert(commandIndex(MediaRemoteCommand::ChangePosition) + 1 == kCommandCount);

    bool isActiveTransport(audio::Transport const transport) noexcept
    {
      return transport == audio::Transport::Opening || transport == audio::Transport::Buffering ||
             transport == audio::Transport::Playing || transport == audio::Transport::Seeking;
    }

    MPNowPlayingPlaybackState playbackState(audio::Transport const transport) noexcept
    {
      switch (transport)
      {
        case audio::Transport::Opening:
        case audio::Transport::Buffering:
        case audio::Transport::Playing:
        case audio::Transport::Seeking: return MPNowPlayingPlaybackStatePlaying;
        case audio::Transport::Paused: return MPNowPlayingPlaybackStatePaused;
        case audio::Transport::Stopping:
        case audio::Transport::Idle: return MPNowPlayingPlaybackStateStopped;
        case audio::Transport::Error: return MPNowPlayingPlaybackStateInterrupted;
      }

      return MPNowPlayingPlaybackStateUnknown;
    }

    MPRemoteCommandHandlerStatus nativeStatus(MediaRemoteCommandStatus const status) noexcept
    {
      switch (status)
      {
        case MediaRemoteCommandStatus::Success: return MPRemoteCommandHandlerStatusSuccess;
        case MediaRemoteCommandStatus::NoActionableNowPlayingItem:
          return MPRemoteCommandHandlerStatusNoActionableNowPlayingItem;
        case MediaRemoteCommandStatus::CommandFailed: return MPRemoteCommandHandlerStatusCommandFailed;
      }

      return MPRemoteCommandHandlerStatusCommandFailed;
    }

    std::optional<uimodel::PlaybackCommand> playbackCommand(MediaRemoteCommand const command) noexcept
    {
      switch (command)
      {
        case MediaRemoteCommand::Play: return uimodel::PlaybackCommand::Play;
        case MediaRemoteCommand::Pause: return uimodel::PlaybackCommand::Pause;
        case MediaRemoteCommand::PlayPause: return uimodel::PlaybackCommand::PlayPause;
        case MediaRemoteCommand::Previous: return uimodel::PlaybackCommand::Previous;
        case MediaRemoteCommand::Next: return uimodel::PlaybackCommand::Next;
        case MediaRemoteCommand::Stop: return uimodel::PlaybackCommand::Stop;
        case MediaRemoteCommand::ChangePosition: return std::nullopt;
      }

      return std::nullopt;
    }

    struct NativeCommandRegistration final
    {
      MPRemoteCommand* command = nil;
      id target = nil;
    };

    struct AdmittedCommand final
    {
      MediaRemoteCommand command = MediaRemoteCommand::PlayPause;
      rt::PlaybackOccurrenceId expectedOccurrenceId{};
      std::optional<std::chrono::milliseconds> optPosition;
    };
  } // namespace

  struct MediaPlayerAdapter::Impl final
  {
    struct AdmissionSnapshot final
    {
      rt::PlaybackOccurrenceId occurrenceId{};
      std::chrono::milliseconds duration{0};
      std::array<bool, kCommandCount> available{};
    };

    struct AdmissionGate final
    {
      mutable std::mutex mutex;
      Impl* owner = nullptr;
      AdmissionSnapshot snapshot{};
      std::uint64_t admittedCommandCount = 0;
      std::uint64_t settledCommandCount = 0;
      std::uint64_t handledCommandCount = 0;
      std::size_t registeredCommandCount = 0;
      std::size_t activeDeliveryCount = 0;
      bool retired = false;
    };

    struct [[nodiscard]] NativeSession final
    {
      explicit NativeSession(std::shared_ptr<AdmissionGate> gateValuePtr)
        : gatePtr{std::move(gateValuePtr)}
      {
      }
      ~NativeSession() { retire(); }

      NativeSession(NativeSession const&) = delete;
      NativeSession& operator=(NativeSession const&) = delete;
      NativeSession(NativeSession&&) = delete;
      NativeSession& operator=(NativeSession&&) = delete;

      void retire() noexcept
      {
        if (retired)
        {
          return;
        }

        retired = true;

        {
          auto lock = std::scoped_lock{gatePtr->mutex};
          gatePtr->retired = true;
          gatePtr->owner = nullptr;
          gatePtr->snapshot = {};
        }

        for (auto& registration : commands)
        {
          auto* const command = registration.command;
          command.enabled = NO;
          [command removeTarget:registration.target];
          registration = {};
        }

        {
          auto lock = std::scoped_lock{gatePtr->mutex};
          gatePtr->registeredCommandCount = 0;
        }

        auto* const center = MPNowPlayingInfoCenter.defaultCenter;
        center.nowPlayingInfo = nil;
        center.playbackState = MPNowPlayingPlaybackStateStopped;
      }

      std::shared_ptr<AdmissionGate> gatePtr;
      std::array<NativeCommandRegistration, kCommandCount> commands{};
      bool retired = false;
    };

    Impl(rt::PlaybackService& playbackValue,
         uimodel::PlaybackActions& actionsValue,
         rt::ResourceByteMemoryCache& resourceBytesValue,
         Clock clockValue)
      : playback{playbackValue}
      , actions{actionsValue}
      , commands{playback.commands()}
      , resourceBytes{resourceBytesValue}
      , clock{std::move(clockValue)}
      , gatePtr{std::make_shared<AdmissionGate>()}
      , nativeSession{gatePtr}
    {
      AO_EXPECTS(NSThread.isMainThread);
      AO_EXPECTS(clock);
      gatePtr->owner = this;
      registerCommands();
      snapshotSub =
        playback.events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot) { handleSnapshot(snapshot); });
      handleSnapshot(playback.snapshot());
    }

    ~Impl() { retire(); }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    // The queued block must own the shared pointer, not capture a caller's reference storage.
    static MediaRemoteCommandStatus admit(std::shared_ptr<AdmissionGate> gatePtr,
                                          MediaRemoteCommand const command,
                                          std::optional<std::chrono::milliseconds> const optPosition) noexcept
    {
      auto admitted = AdmittedCommand{.command = command, .optPosition = optPosition};

      {
        auto lock = std::scoped_lock{gatePtr->mutex};

        if (gatePtr->retired)
        {
          return MediaRemoteCommandStatus::CommandFailed;
        }

        auto const index = commandIndex(command);

        if (index >= kCommandCount)
        {
          return MediaRemoteCommandStatus::CommandFailed;
        }

        auto const& snapshot = gatePtr->snapshot;

        if (!snapshot.available[index])
        {
          return MediaRemoteCommandStatus::NoActionableNowPlayingItem;
        }

        if (command == MediaRemoteCommand::ChangePosition)
        {
          if (!optPosition || *optPosition < std::chrono::milliseconds{0} ||
              snapshot.duration <= std::chrono::milliseconds{0} || *optPosition > snapshot.duration)
          {
            return MediaRemoteCommandStatus::CommandFailed;
          }

          admitted.expectedOccurrenceId = snapshot.occurrenceId;
        }

        ++gatePtr->admittedCommandCount;
      }

      ::dispatch_async(dispatch_get_main_queue(), ^{
        Impl* owner = nullptr;

        {
          auto lock = std::scoped_lock{gatePtr->mutex};

          if (!gatePtr->retired)
          {
            owner = gatePtr->owner;

            if (owner != nullptr)
            {
              ++gatePtr->activeDeliveryCount;
            }
          }
        }

        if (owner == nullptr)
        {
          return;
        }

        auto const finishDelivery = [&]
        {
          auto lock = std::scoped_lock{gatePtr->mutex};
          AO_INVARIANT(gatePtr->activeDeliveryCount > 0);
          --gatePtr->activeDeliveryCount;
        };

        try
        {
          auto const handled = owner->tryHandleAdmitted(admitted);

          {
            auto lock = std::scoped_lock{gatePtr->mutex};
            ++gatePtr->settledCommandCount;

            if (handled && !gatePtr->retired)
            {
              ++gatePtr->handledCommandCount;
            }
          }

          finishDelivery();
        }
        catch (...)
        {
          finishDelivery();
          AO_FATAL_EXCEPTION(std::current_exception(), "AppKit MediaPlayer command callback");
        }
      });
      return MediaRemoteCommandStatus::Success;
    }

    void registerCommands()
    {
      auto* const center = MPRemoteCommandCenter.sharedCommandCenter;
      // Native commands default to enabled, including those without a handler.
      center.enableLanguageOptionCommand.enabled = NO;
      center.disableLanguageOptionCommand.enabled = NO;
      center.changePlaybackRateCommand.enabled = NO;
      center.changeRepeatModeCommand.enabled = NO;
      center.changeShuffleModeCommand.enabled = NO;
      center.skipForwardCommand.enabled = NO;
      center.skipBackwardCommand.enabled = NO;
      center.seekForwardCommand.enabled = NO;
      center.seekBackwardCommand.enabled = NO;
      center.ratingCommand.enabled = NO;
      center.likeCommand.enabled = NO;
      center.dislikeCommand.enabled = NO;
      center.bookmarkCommand.enabled = NO;

      registerCommand(center.playCommand, MediaRemoteCommand::Play);
      registerCommand(center.pauseCommand, MediaRemoteCommand::Pause);
      registerCommand(center.togglePlayPauseCommand, MediaRemoteCommand::PlayPause);
      registerCommand(center.stopCommand, MediaRemoteCommand::Stop);

      registerCommand(center.previousTrackCommand, MediaRemoteCommand::Previous);
      registerCommand(center.nextTrackCommand, MediaRemoteCommand::Next);
      registerCommand(center.changePlaybackPositionCommand, MediaRemoteCommand::ChangePosition);
    }

    void registerCommand(MPRemoteCommand* command, MediaRemoteCommand const mediaCommand)
    {
      auto& registration = nativeSession.commands[commandIndex(mediaCommand)];
      AO_INVARIANT(registration.command == nil, "A native media command must be registered exactly once");
      auto const handler = makeRemoteCommandHandler(gatePtr);
      id const target = [command addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* event) {
        auto nativeEvent = MediaRemoteCommandEvent{.command = mediaCommand};

        if (mediaCommand == MediaRemoteCommand::ChangePosition)
        {
          if ([event isKindOfClass:MPChangePlaybackPositionCommandEvent.class] == NO)
          {
            return MPRemoteCommandHandlerStatusCommandFailed;
          }

          nativeEvent.optPositionSeconds = static_cast<MPChangePlaybackPositionCommandEvent*>(event).positionTime;
        }

        return nativeStatus(handler(nativeEvent));
      }];
      AO_INVARIANT(target != nil, "Native media command registration must return an owned target");
      registration = {.command = command, .target = target};

      {
        auto lock = std::scoped_lock{gatePtr->mutex};
        ++gatePtr->registeredCommandCount;
      }
    }

    static MediaRemoteCommandHandler makeRemoteCommandHandler(std::shared_ptr<AdmissionGate> gatePtr)
    {
      return [gatePtr = std::move(gatePtr)](MediaRemoteCommandEvent const& event) noexcept
      {
        auto optPosition = std::optional<std::chrono::milliseconds>{};

        if (event.command == MediaRemoteCommand::ChangePosition)
        {
          constexpr double kMillisecondsPerSecond = 1000.0;
          constexpr auto kMaximumMilliseconds = static_cast<double>(std::numeric_limits<std::int64_t>::max());

          if (!event.optPositionSeconds || !std::isfinite(*event.optPositionSeconds) ||
              *event.optPositionSeconds < 0.0 ||
              *event.optPositionSeconds >= kMaximumMilliseconds / kMillisecondsPerSecond)
          {
            return MediaRemoteCommandStatus::CommandFailed;
          }

          optPosition =
            std::chrono::milliseconds{static_cast<std::int64_t>(*event.optPositionSeconds * kMillisecondsPerSecond)};
        }
        else if (event.optPositionSeconds)
        {
          return MediaRemoteCommandStatus::CommandFailed;
        }

        return admit(gatePtr, event.command, optPosition);
      };
    }

    bool tryHandleAdmitted(AdmittedCommand const& admitted)
    {
      AO_EXPECTS(NSThread.isMainThread);

      auto const& snapshot = playback.snapshot();
      auto const transport = snapshot.transport.transport;

      if (admitted.command == MediaRemoteCommand::ChangePosition)
      {
        if (!admitted.optPosition || snapshot.transport.occurrenceId != admitted.expectedOccurrenceId)
        {
          return false;
        }

        commands.seek(admitted.expectedOccurrenceId, *admitted.optPosition);
        return true;
      }

      if (admitted.command == MediaRemoteCommand::Play && isActiveTransport(transport))
      {
        return true;
      }

      if (admitted.command == MediaRemoteCommand::Pause && transport == audio::Transport::Paused)
      {
        return true;
      }

      auto const optCommand = playbackCommand(admitted.command);
      return optCommand && actions.tryExecute(*optCommand);
    }

    void handleSnapshot(rt::PlaybackSnapshot const& snapshot)
    {
      AO_EXPECTS(NSThread.isMainThread);

      if (retired)
      {
        return;
      }

      auto nextAdmission = AdmissionSnapshot{};
      auto const& transport = snapshot.transport;
      nextAdmission.occurrenceId = transport.occurrenceId;
      nextAdmission.duration = transport.duration;
      nextAdmission.available[commandIndex(MediaRemoteCommand::Play)] =
        isActiveTransport(transport.transport) || actions.isEnabled(uimodel::PlaybackCommand::Play);
      nextAdmission.available[commandIndex(MediaRemoteCommand::Pause)] =
        transport.transport == audio::Transport::Paused || actions.isEnabled(uimodel::PlaybackCommand::Pause);
      nextAdmission.available[commandIndex(MediaRemoteCommand::PlayPause)] =
        actions.isEnabled(uimodel::PlaybackCommand::PlayPause);
      nextAdmission.available[commandIndex(MediaRemoteCommand::Previous)] =
        actions.isEnabled(uimodel::PlaybackCommand::Previous);
      nextAdmission.available[commandIndex(MediaRemoteCommand::Next)] =
        actions.isEnabled(uimodel::PlaybackCommand::Next);
      nextAdmission.available[commandIndex(MediaRemoteCommand::Stop)] =
        actions.isEnabled(uimodel::PlaybackCommand::Stop);
      nextAdmission.available[commandIndex(MediaRemoteCommand::ChangePosition)] =
        transport.occurrenceId.value != 0 && transport.nowPlaying.trackId != kInvalidTrackId &&
        transport.duration > std::chrono::milliseconds{0};

      {
        auto lock = std::scoped_lock{gatePtr->mutex};
        gatePtr->snapshot = nextAdmission;
      }

      for (std::size_t index = 0; index < nativeSession.commands.size(); ++index)
      {
        auto* const command = nativeSession.commands[index].command;
        command.enabled = static_cast<BOOL>(nextAdmission.available[index]);
      }

      auto const clockChanged = !hasLastSnapshot || transport.transport != lastSnapshot.transport.transport ||
                                transport.positionRevision != lastSnapshot.transport.positionRevision ||
                                transport.duration != lastSnapshot.transport.duration;

      if (clockChanged)
      {
        // Metadata-only snapshots can carry an old elapsed sample, not a new clock anchor.
        clockElapsed = transport.elapsed;
        clockReceiptTime = clock();
      }

      auto const coverChanged = displayedCoverId != transport.nowPlaying.coverArtId;
      auto const publicationChanged = clockChanged || transport.nowPlaying != lastSnapshot.transport.nowPlaying ||
                                      snapshot.succession.currentTrackId != lastSnapshot.succession.currentTrackId;
      lastSnapshot = snapshot;
      hasLastSnapshot = true;

      if (coverChanged)
      {
        artworkRequest.reset();
        displayedCoverId = transport.nowPlaying.coverArtId;
        artwork = nil;

        if (displayedCoverId != kInvalidResourceId)
        {
          auto const requestedId = displayedCoverId;
          artworkRequest = resourceBytes.request(requestedId,
                                                 [this, requestedId](rt::ResourceBytes bytes)
                                                 {
                                                   if (!retired && displayedCoverId == requestedId && !bytes.empty())
                                                   {
                                                     auto const encoded = bytes.view();
                                                     auto* const data = [NSData dataWithBytes:encoded.data()
                                                                                       length:encoded.size()];
                                                     artwork = [[NSImage alloc] initWithData:data];
                                                     publishNowPlaying(lastSnapshot);
                                                   }
                                                 });

          if (!artworkRequest)
          {
            // A synchronous cache hit already published the updated snapshot.
            return;
          }
        }
      }

      if (publicationChanged || coverChanged)
      {
        publishNowPlaying(snapshot);
      }
    }

    void publishNowPlaying(rt::PlaybackSnapshot const& snapshot) const
    {
      AO_EXPECTS(NSThread.isMainThread);
      auto* const center = MPNowPlayingInfoCenter.defaultCenter;
      auto const& transport = snapshot.transport;
      auto const resumableSubject = transport.nowPlaying.trackId != kInvalidTrackId &&
                                    snapshot.succession.currentTrackId == transport.nowPlaying.trackId;
      center.playbackState = playbackState(transport.transport);

      if (transport.nowPlaying.trackId == kInvalidTrackId ||
          (transport.transport == audio::Transport::Idle && !resumableSubject))
      {
        center.nowPlayingInfo = nil;
        return;
      }

      auto* const info = [NSMutableDictionary dictionary];
      auto elapsed = std::max(clockElapsed, std::chrono::milliseconds{0});

      if (transport.transport == audio::Transport::Playing)
      {
        elapsed += std::chrono::duration_cast<std::chrono::milliseconds>(clock() - clockReceiptTime);
      }

      if (transport.duration > std::chrono::milliseconds{0})
      {
        elapsed = std::min(elapsed, transport.duration);
      }

      info[MPMediaItemPropertyTitle] = nativeText(transport.nowPlaying.title);
      info[MPMediaItemPropertyArtist] = nativeText(transport.nowPlaying.artist);
      info[MPMediaItemPropertyAlbumTitle] = nativeText(transport.nowPlaying.album);
      info[MPMediaItemPropertyPlaybackDuration] = @(std::chrono::duration<double>{transport.duration}.count());
      info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(std::chrono::duration<double>{elapsed}.count());
      info[MPNowPlayingInfoPropertyPlaybackRate] = @(transport.transport == audio::Transport::Playing ? 1.0 : 0.0);

      if (artwork != nil && displayedCoverId == transport.nowPlaying.coverArtId)
      {
        NSImage* const capturedArtwork = [artwork copy];
        info[MPMediaItemPropertyArtwork] =
          [[MPMediaItemArtwork alloc] initWithBoundsSize:capturedArtwork.size
                                          requestHandler:^NSImage*(CGSize) { return capturedArtwork; }];
      }

      center.nowPlayingInfo = info;
    }

    void retire() noexcept
    {
      AO_EXPECTS(NSThread.isMainThread);

      if (retired)
      {
        return;
      }

      retired = true;

      nativeSession.retire();
      snapshotSub.reset();
      artworkRequest.reset();
      displayedCoverId = kInvalidResourceId;
      artwork = nil;
    }

    MediaPlayerDiagnostics diagnostics() const noexcept
    {
      auto lock = std::scoped_lock{gatePtr->mutex};
      return {
        .registeredCommandCount = gatePtr->registeredCommandCount,
        .admittedCommandCount = gatePtr->admittedCommandCount,
        .settledCommandCount = gatePtr->settledCommandCount,
        .handledCommandCount = gatePtr->handledCommandCount,
        .retired = gatePtr->retired,
      };
    }

    rt::PlaybackService& playback;
    uimodel::PlaybackActions& actions;
    rt::PlaybackCommands& commands;
    rt::ResourceByteMemoryCache& resourceBytes;
    Clock clock;
    std::shared_ptr<AdmissionGate> gatePtr;
    NativeSession nativeSession;
    async::Subscription snapshotSub;
    rt::ResourceByteMemoryCache::Request artworkRequest;
    rt::PlaybackSnapshot lastSnapshot{};
    std::chrono::steady_clock::time_point clockReceiptTime{};
    std::chrono::milliseconds clockElapsed{0};
    ResourceId displayedCoverId = kInvalidResourceId;
    NSImage* artwork = nil;
    bool hasLastSnapshot = false;
    bool retired = false;
  };

  MediaPlayerAdapter::MediaPlayerAdapter(rt::PlaybackService& playback,
                                         uimodel::PlaybackActions& actions,
                                         rt::ResourceByteMemoryCache& resourceBytes,
                                         Clock clock)
    : _implPtr{std::make_unique<Impl>(playback, actions, resourceBytes, std::move(clock))}
  {
  }

  MediaPlayerAdapter::~MediaPlayerAdapter() = default;

  MediaRemoteCommandHandler MediaPlayerAdapter::remoteCommandHandler() const
  {
    return Impl::makeRemoteCommandHandler(_implPtr->gatePtr);
  }

  bool MediaPlayerAdapter::isCommandAvailable(MediaRemoteCommand const command) const noexcept
  {
    auto lock = std::scoped_lock{_implPtr->gatePtr->mutex};
    auto const index = commandIndex(command);
    return index < kCommandCount && !_implPtr->gatePtr->retired && _implPtr->gatePtr->snapshot.available[index];
  }

  MediaPlayerDiagnostics MediaPlayerAdapter::diagnostics() const noexcept
  {
    return _implPtr->diagnostics();
  }

  bool MediaPlayerAdapter::isPerforming() const noexcept
  {
    auto lock = std::scoped_lock{_implPtr->gatePtr->mutex};
    return _implPtr->gatePtr->activeDeliveryCount > 0;
  }

  void MediaPlayerAdapter::retire() noexcept
  {
    _implPtr->retire();
  }
} // namespace ao::appkit
