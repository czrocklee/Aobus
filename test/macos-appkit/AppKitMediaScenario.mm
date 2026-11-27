// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitScenarioSupport.h"
#include "app/macos-appkit/AppKitText.h"
#include "app/macos-appkit/LibraryEditorModel.h"
#include "app/macos-appkit/MediaPlayerAdapter.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/async/Executor.h>
#include <ao/async/Runtime.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/resource/ResourceByteMemoryCache.h>
#include <ao/uimodel/library/track/TrackDisplayIndex.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#import <MediaPlayer/MediaPlayer.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <vector>

@interface AobusMediaPublicationObserver : NSObject
- (instancetype)initWithCenter:(MPNowPlayingInfoCenter*)center;
- (NSUInteger)publicationCount;
@end

@implementation AobusMediaPublicationObserver {
  MPNowPlayingInfoCenter* _center;
  NSUInteger _publicationCount;
}

- (instancetype)initWithCenter:(MPNowPlayingInfoCenter*)center
{
  self = [super init];

  if (self != nil)
  {
    _center = center;
    [_center addObserver:self forKeyPath:@"nowPlayingInfo" options:0 context:nullptr];
  }

  return self;
}

- (void)dealloc
{
  [_center removeObserver:self forKeyPath:@"nowPlayingInfo" context:nullptr];
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context
{
  if (object == _center && [keyPath isEqualToString:@"nowPlayingInfo"] != NO)
  {
    ++_publicationCount;
    return;
  }

  [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
}

- (NSUInteger)publicationCount
{
  return _publicationCount;
}
@end

namespace ao::appkit::test
{
  namespace
  {
    constexpr auto kResumeClockThreshold = std::chrono::milliseconds{250};

    void requireStatus(MediaRemoteCommandHandler const& handler,
                       MediaRemoteCommandEvent const& event,
                       MediaRemoteCommandStatus expected)
    {
      auto const actual = handler(event);
      AO_INVARIANT(actual == expected, "Media command admission must report its actual disposition");
    }

    void drainMediaCallbacks()
    {
      auto const reachedPtr = std::make_shared<bool>(false);
      ::dispatch_async(::dispatch_get_main_queue(), ^{ *reachedPtr = true; });
      requireWaitUntil([&] { return *reachedPtr; }, "Previously admitted media deliveries must drain");
    }

    void executeRemote(LibrarySession& session, MediaRemoteCommandEvent const& event)
    {
      auto& media = session.mediaPlayer();
      auto const before = media.diagnostics();
      requireStatus(media.remoteCommandHandler(), event, MediaRemoteCommandStatus::Success);
      requireWaitUntil([&] { return media.diagnostics().settledCommandCount == before.settledCommandCount + 1; },
                       "An admitted media command must settle on the main thread");
      AO_INVARIANT(media.diagnostics().handledCommandCount == before.handledCommandCount + 1,
                   "An actionable media command must be handled exactly once");
    }

    void waitForTransport(LibrarySession& session, audio::Transport transport)
    {
      requireWaitUntil([&] { return session.runtime().playback().snapshot().transport.transport == transport; },
                       "Remote transport changes must reach the playback snapshot");
    }

    std::array<MPRemoteCommand*, 13> unsupportedNativeCommands()
    {
      auto* const center = MPRemoteCommandCenter.sharedCommandCenter;
      return {
        center.enableLanguageOptionCommand,
        center.disableLanguageOptionCommand,
        center.changePlaybackRateCommand,
        center.changeRepeatModeCommand,
        center.changeShuffleModeCommand,
        center.skipForwardCommand,
        center.skipBackwardCommand,
        center.seekForwardCommand,
        center.seekBackwardCommand,
        center.ratingCommand,
        center.likeCommand,
        center.dislikeCommand,
        center.bookmarkCommand,
      };
    }

    void requireNativeCommandAvailability(MediaPlayerAdapter const& media)
    {
      auto* const center = MPRemoteCommandCenter.sharedCommandCenter;
      auto requireCommand = [&](MPRemoteCommand* nativeCommand, MediaRemoteCommand command)
      {
        AO_INVARIANT((nativeCommand.enabled != NO) == media.isCommandAvailable(command),
                     "Native command availability must follow its identity, not registration order");
      };
      requireCommand(center.playCommand, MediaRemoteCommand::Play);
      requireCommand(center.pauseCommand, MediaRemoteCommand::Pause);
      requireCommand(center.togglePlayPauseCommand, MediaRemoteCommand::PlayPause);
      requireCommand(center.previousTrackCommand, MediaRemoteCommand::Previous);
      requireCommand(center.nextTrackCommand, MediaRemoteCommand::Next);
      requireCommand(center.stopCommand, MediaRemoteCommand::Stop);
      requireCommand(center.changePlaybackPositionCommand, MediaRemoteCommand::ChangePosition);

      for (auto const& command : unsupportedNativeCommands())
      {
        AO_INVARIANT(command.enabled == NO, "Unsupported native media commands must remain disabled");
      }
    }

    void requireNoPlayTarget(MediaPlayerAdapter const& media)
    {
      auto const before = media.diagnostics();
      auto const handler = media.remoteCommandHandler();
      requireNativeCommandAvailability(media);

      for (auto const command : {MediaRemoteCommand::Play, MediaRemoteCommand::PlayPause})
      {
        AO_INVARIANT(!media.isCommandAvailable(command), "Remote Play must be unavailable without a target");
        requireStatus(handler, {.command = command}, MediaRemoteCommandStatus::NoActionableNowPlayingItem);
      }

      AO_INVARIANT(media.diagnostics().admittedCommandCount == before.admittedCommandCount,
                   "Remote Play without a target must not enqueue work");
    }

    void exerciseIdle(MediaPlayerAdapter& media)
    {
      requireNativeCommandAvailability(media);
      auto const before = media.diagnostics();
      AO_INVARIANT(before.registeredCommandCount == 7 && !before.retired,
                   "The session must register all seven media commands exactly once");
      AO_INVARIANT(MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo == nil,
                   "An empty playback subject must clear native Now Playing information");
      auto const handler = media.remoteCommandHandler();
      requireStatus(
        handler, {.command = MediaRemoteCommand::Pause}, MediaRemoteCommandStatus::NoActionableNowPlayingItem);

      // The first unrepresentable millisecond value must fail even without a seekable subject.
      constexpr double kCastLimitSeconds = 0x1p63 / 1000.0;
      requireStatus(handler,
                    {.command = MediaRemoteCommand::ChangePosition, .optPositionSeconds = 1.0},
                    MediaRemoteCommandStatus::NoActionableNowPlayingItem);

      for (auto position : {std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::max(),
                            kCastLimitSeconds,
                            -1.0})
      {
        requireStatus(handler,
                      {.command = MediaRemoteCommand::ChangePosition, .optPositionSeconds = position},
                      MediaRemoteCommandStatus::CommandFailed);
      }

      requireStatus(handler, {.command = MediaRemoteCommand::ChangePosition}, MediaRemoteCommandStatus::CommandFailed);
      requireStatus(handler,
                    {.command = MediaRemoteCommand::Play, .optPositionSeconds = 1.0},
                    MediaRemoteCommandStatus::CommandFailed);
      AO_INVARIANT(media.diagnostics().admittedCommandCount == before.admittedCommandCount,
                   "Rejected media commands must not enqueue work");
    }

    void exerciseVisibleSelectionPlay(LibrarySession& session)
    {
      auto& playback = session.runtime().playback();
      requireWaitUntil(
        [&]
        {
          auto const& transport = playback.snapshot().transport;
          return transport.ready && !transport.output.optPendingOutputSelection;
        },
        "The idle Play fixture must have a ready output");
      auto const* selectedRow = session.rowAt(1);
      AO_INVARIANT(selectedRow != nullptr && session.selection().empty(),
                   "The idle Play fixture must have at least two projected rows but no selection");
      auto const selectedId = selectedRow->id;
      auto const before = playback.snapshot().transport;
      AO_INVARIANT(before.transport == audio::Transport::Idle && !before.hasDeferredResume &&
                     before.nowPlaying.trackId == kInvalidTrackId,
                   "Only a new start may satisfy the idle Play fixture");
      AO_INVARIANT(!session.state().transport[static_cast<std::size_t>(uimodel::PlaybackCommand::PlayPause)].enabled,
                   "The primary Play control must be disabled without a selected row");
      session.execute(uimodel::PlaybackCommand::PlayPause);
      drainMediaCallbacks();
      AO_INVARIANT(playback.snapshot().transport.nowPlaying.trackId == kInvalidTrackId,
                   "Primary Play must not start an unselected row");

      auto& media = session.mediaPlayer();
      requireNoPlayTarget(media);
      auto* const observer =
        [[AobusMediaPublicationObserver alloc] initWithCenter:MPNowPlayingInfoCenter.defaultCenter];
      session.select({selectedId});
      AO_INVARIANT(
        media.isCommandAvailable(MediaRemoteCommand::Play) && media.isCommandAvailable(MediaRemoteCommand::PlayPause),
        "A selection-only change must enable Play and PlayPause with a visible target");
      requireNativeCommandAvailability(media);
      AO_INVARIANT(
        playback.snapshot().transport.positionRevision == before.positionRevision && observer.publicationCount == 0,
        "Selection alone must not reanchor playback or publish Now Playing");

      auto const beforePlay = media.diagnostics();
      requireStatus(
        media.remoteCommandHandler(), {.command = MediaRemoteCommand::Play}, MediaRemoteCommandStatus::Success);
      // Clear the target in this main-thread turn, before the queued Play can run.
      session.select({});
      drainMediaCallbacks();
      auto const afterPlay = media.diagnostics();
      AO_INVARIANT(afterPlay.settledCommandCount == beforePlay.settledCommandCount + 1 &&
                     afterPlay.handledCommandCount == beforePlay.handledCommandCount,
                   "Admitted Play must settle without being handled after its target is cleared");
      auto const& idle = playback.snapshot().transport;
      AO_INVARIANT(idle.transport == audio::Transport::Idle && idle.nowPlaying.trackId == kInvalidTrackId,
                   "Remote Play must not start playback from a cleared selection");

      session.select({selectedId});
      executeRemote(session, {.command = MediaRemoteCommand::Play});
      waitForTransport(session, audio::Transport::Playing);
      AO_INVARIANT(playback.snapshot().transport.nowPlaying.trackId == selectedId &&
                     session.selection() == std::vector{selectedId},
                   "Remote Play must start the selected track, not row zero");
    }

    void exerciseFilteredRemotePlay(LibrarySession& session)
    {
      auto& runtime = session.runtime();
      auto& playback = runtime.playback();
      auto const viewId = runtime.workspace().snapshot().activeViewId;
      AO_INVARIANT(session.displayIndex().rowCount() >= 4, "Filtered Play requires four distinct fixture tracks");
      auto const firstId = session.rowAt(0)->id;
      auto const* hiddenRow = session.rowAt(1);
      auto const hiddenId = hiddenRow->id;
      auto const filter = std::format("$title != \"{}\"", hiddenRow->title);
      auto const selectedId = session.rowAt(2)->id;
      auto const stagedId = session.rowAt(3)->id;
      session.select({hiddenId, selectedId, firstId});

      bool delivered = false;
      runtime.async().callbackExecutor().defer(
        [&]
        {
          auto const filterRes = runtime.views().setFilter(viewId, filter);
          AO_INVARIANT(filterRes && session.displayIndexOf(hiddenId),
                       "The runtime filter must commit before the browser receives the projection update");
          // Keep a valid start pending while the remote turn runs. Trying any row instead of doing nothing
          // would supersede it, even if the runtime rejects an attempted start of the hidden row.
          auto const startRes = playback.commands().startFromView(viewId, stagedId);
          AO_INVARIANT(startRes, "The control start must be admitted before the remote command");
          auto& media = session.mediaPlayer();
          auto const before = media.diagnostics();
          requireStatus(
            media.remoteCommandHandler(), {.command = MediaRemoteCommand::Play}, MediaRemoteCommandStatus::Success);
          session.select({hiddenId});
          drainMediaCallbacks();
          AO_INVARIANT(media.diagnostics().settledCommandCount == before.settledCommandCount + 1 &&
                         media.diagnostics().handledCommandCount == before.handledCommandCount &&
                         session.displayIndexOf(hiddenId) &&
                         playback.snapshot().transport.transport == audio::Transport::Idle,
                       "Remote Play must decline at delivery time when the selection is hidden");
          delivered = true;
        });
      requireWaitUntil([&] { return delivered && !session.displayIndexOf(hiddenId); },
                       "The filtered projection must reach the browser after remote delivery");
      waitForTransport(session, audio::Transport::Playing);
      AO_INVARIANT(playback.snapshot().transport.nowPlaying.trackId == stagedId,
                   "Remote Play must leave the pending start alone when the delivery-time selection is hidden");
      executeRemote(session, {.command = MediaRemoteCommand::Stop});
      waitForTransport(session, audio::Transport::Idle);

      auto& media = session.mediaPlayer();
      AO_INVARIANT(session.selection() == std::vector{hiddenId}, "Filtering must preserve the remembered selection");
      requireNoPlayTarget(media);

      session.select({hiddenId, selectedId, firstId});
      AO_INVARIANT(
        media.isCommandAvailable(MediaRemoteCommand::Play) && media.isCommandAvailable(MediaRemoteCommand::PlayPause),
        "Remote start commands must re-enable when a selected track is visible");
      requireNativeCommandAvailability(media);
      executeRemote(session, {.command = MediaRemoteCommand::Play});
      waitForTransport(session, audio::Transport::Playing);
      AO_INVARIANT(playback.snapshot().transport.nowPlaying.trackId == selectedId,
                   "Remote Play must use the first projected selection in selection order");
      executeRemote(session, {.command = MediaRemoteCommand::Stop});
      waitForTransport(session, audio::Transport::Idle);
      session.select({hiddenId});
      auto const emptyRes = runtime.views().setFilter(viewId, R"($title = "Missing Track")");
      AO_INVARIANT(emptyRes);
      requireWaitUntil([&] { return session.displayIndex().rowCount() == 0; },
                       "The empty-projection filter must settle before checking remote availability");
      requireNoPlayTarget(media);

      auto const clearRes = runtime.views().setFilter(viewId, "");
      AO_INVARIANT(clearRes);
      requireWaitUntil([&] { return session.displayIndexOf(hiddenId).has_value(); },
                       "The rest of the media scenario must use the unfiltered fixture");
      AO_INVARIANT(
        media.isCommandAvailable(MediaRemoteCommand::Play) && media.isCommandAvailable(MediaRemoteCommand::PlayPause),
        "Revealing the remembered selection must restore remote Play without changing the selection or transport");
      requireNativeCommandAvailability(media);
      session.select({});
      requireNoPlayTarget(media);
    }

    void exerciseMetadata(LibrarySession& session)
    {
      auto const& transport = session.runtime().playback().snapshot().transport;
      auto* const center = MPNowPlayingInfoCenter.defaultCenter;
      auto* const info = center.nowPlayingInfo;
      AO_INVARIANT(info != nil, "Playback must publish native metadata without a visible window");
      AO_INVARIANT([info[MPMediaItemPropertyTitle] isEqual:nativeText(transport.nowPlaying.title)] != NO &&
                     [info[MPMediaItemPropertyArtist] isEqual:nativeText(transport.nowPlaying.artist)] != NO &&
                     [info[MPMediaItemPropertyAlbumTitle] isEqual:nativeText(transport.nowPlaying.album)] != NO,
                   "Native metadata must describe the runtime's playback subject");
      AO_INVARIANT(std::abs([info[MPMediaItemPropertyPlaybackDuration] doubleValue] -
                            std::chrono::duration<double>{transport.duration}.count()) < 0.1,
                   "Now Playing duration must use seconds");
      AO_INVARIANT(center.playbackState == MPNowPlayingPlaybackStatePlaying &&
                     [info[MPNowPlayingInfoPropertyPlaybackRate] doubleValue] == 1.0,
                   "Native playback state and rate must agree with active playback");
    }

    void exerciseForeignPause(LibrarySession& session)
    {
      bool closeGuardObserved = false;
      auto sub = session.runtime().playback().events().onSnapshot(
        [&](rt::PlaybackSnapshot const& snapshot)
        {
          if (snapshot.transport.transport == audio::Transport::Paused)
          {
            AO_INVARIANT(NSThread.isMainThread != NO && session.mediaPlayer().isPerforming() && !session.canClose(),
                         "A foreign command must execute on main and defer teardown until publication unwinds");
            closeGuardObserved = true;
          }
        });
      auto const resultPtr = std::make_shared<std::atomic<bool>>(false);
      auto const handler = session.mediaPlayer().remoteCommandHandler();
      ::dispatch_async(::dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        requireStatus(handler, {.command = MediaRemoteCommand::Pause}, MediaRemoteCommandStatus::Success);
        resultPtr->store(true, std::memory_order_release);
      });
      requireWaitUntil([&] { return resultPtr->load(std::memory_order_acquire) && closeGuardObserved; },
                       "Foreign-thread pause must complete while the main window is hidden");
      AO_INVARIANT(session.canClose(), "The close guard must release after remote command delivery");
      auto* const center = MPNowPlayingInfoCenter.defaultCenter;
      AO_INVARIANT(center.playbackState == MPNowPlayingPlaybackStatePaused &&
                     [center.nowPlayingInfo[MPNowPlayingInfoPropertyPlaybackRate] doubleValue] == 0.0,
                   "Pause must publish native paused state and zero rate");
    }

    void exerciseTransport(LibrarySession& session)
    {
      auto& playback = session.runtime().playback();
      constexpr auto kPausedOffset = std::chrono::milliseconds{500};
      playback.commands().seek(kPausedOffset);
      requireWaitUntil([&] { return playback.snapshot().transport.elapsed == kPausedOffset; },
                       "The paused fixture must hold a nonzero offset");
      auto const paused = playback.snapshot().transport;
      auto* const observer =
        [[AobusMediaPublicationObserver alloc] initWithCenter:MPNowPlayingInfoCenter.defaultCenter];
      auto const otherId = session.rowAt(0)->id;
      AO_INVARIANT(otherId != paused.nowPlaying.trackId, "The selection must compete with the paused subject");
      session.select({});
      session.select({otherId});
      AO_INVARIANT(observer.publicationCount == 0, "Selection changes must not republish Now Playing");
      executeRemote(session, {.command = MediaRemoteCommand::Pause});
      AO_INVARIANT(playback.snapshot().transport.positionRevision == paused.positionRevision,
                   "Duplicate remote Pause must not change the playback position anchor");
      executeRemote(session, {.command = MediaRemoteCommand::Play});
      waitForTransport(session, audio::Transport::Playing);
      auto const& playing = playback.snapshot().transport;
      AO_INVARIANT(playing.nowPlaying.trackId == paused.nowPlaying.trackId &&
                     playing.occurrenceId == paused.occurrenceId && playing.elapsed >= paused.elapsed &&
                     playing.elapsed < paused.elapsed + kResumeClockThreshold,
                   "Remote Play must resume the paused occurrence at its offset instead of starting the selection");
      auto const playingRevision = playing.positionRevision;
      executeRemote(session, {.command = MediaRemoteCommand::Play});
      AO_INVARIANT(session.runtime().playback().snapshot().transport.positionRevision == playingRevision,
                   "Duplicate remote Play must not restart the track");
      executeRemote(session, {.command = MediaRemoteCommand::PlayPause});
      waitForTransport(session, audio::Transport::Paused);
      executeRemote(session, {.command = MediaRemoteCommand::PlayPause});
      waitForTransport(session, audio::Transport::Playing);
      auto const firstId = session.runtime().playback().snapshot().transport.nowPlaying.trackId;
      executeRemote(session, {.command = MediaRemoteCommand::Next});
      requireWaitUntil([&] { return session.runtime().playback().snapshot().transport.nowPlaying.trackId != firstId; },
                       "Remote Next must advance the playback subject");
      waitForTransport(session, audio::Transport::Playing);
      executeRemote(session, {.command = MediaRemoteCommand::Previous});
      requireWaitUntil([&] { return session.runtime().playback().snapshot().transport.nowPlaying.trackId == firstId; },
                       "Remote Previous must return to the preceding subject");
      waitForTransport(session, audio::Transport::Playing);
    }

    void exerciseConsecutiveNext(LibrarySession& session)
    {
      auto& media = session.mediaPlayer();
      auto const handler = media.remoteCommandHandler();
      auto const before = media.diagnostics();
      auto const optIndex =
        session.displayIndexOf(session.runtime().playback().snapshot().transport.nowPlaying.trackId);
      AO_INVARIANT(optIndex && *optIndex + 2 < session.displayIndex().displayCount(),
                   "Consecutive Next commands need two more ordered fixture tracks");
      auto const expectedId = session.rowAt(*optIndex + 2)->id;

      // Admit both commands before yielding to their main-queue deliveries.
      requireStatus(handler, {.command = MediaRemoteCommand::Next}, MediaRemoteCommandStatus::Success);
      requireStatus(handler, {.command = MediaRemoteCommand::Next}, MediaRemoteCommandStatus::Success);
      drainMediaCallbacks();
      auto const after = media.diagnostics();
      AO_INVARIANT(after.settledCommandCount == before.settledCommandCount + 2 &&
                     after.handledCommandCount == before.handledCommandCount + 2,
                   "Both queued Next commands must be handled");
      AO_INVARIANT(session.runtime().playback().snapshot().transport.nowPlaying.trackId == expectedId,
                   "Two queued Next commands must advance by two tracks");
      waitForTransport(session, audio::Transport::Playing);
    }

    void exerciseBackloggedSeek(LibrarySession& session, MediaPlayerAdapter& media, bool const retireAfterHandoff)
    {
      auto& playback = session.runtime().playback();
      playback.commands().pause();
      waitForTransport(session, audio::Transport::Paused);
      playback.commands().setMuted(false);
      playback.commands().seek(std::chrono::milliseconds{250});
      auto const started = playback.snapshot().transport;
      auto const before = media.diagnostics();
      auto const handler = media.remoteCommandHandler();
      auto snapshots = std::vector<rt::PlaybackTransportSnapshot>{};
      auto lastTransport = started;
      bool queuedMute = false;
      bool handedOff = false;
      auto const subscription = playback.events().onSnapshot(
        [&](rt::PlaybackSnapshot const& snapshot)
        {
          // A track start's Core Audio system-graph observation lands
          // asynchronously and may publish a quality-only snapshot here. It is
          // orthogonal to the command backlog, so it neither counts nor queues.
          auto withoutQuality = snapshot.transport;
          withoutQuality.quality = lastTransport.quality;
          bool const qualityOnly = withoutQuality == lastTransport;
          lastTransport = snapshot.transport;

          if (qualityOnly)
          {
            return;
          }

          snapshots.push_back(snapshot.transport);

          if (!queuedMute)
          {
            queuedMute = true;
            playback.commands().setMuted(true);
          }
        });

      session.runtime().async().callbackExecutor().defer(
        [&]
        {
          playback.commands().setVolume(started.volume.level < 0.5F ? 0.75F : 0.25F);
          AO_INVARIANT(queuedMute && !playback.snapshot().transport.volume.muted,
                       "The media seek regression must leave an orthogonal runtime backlog");
          requireStatus(handler,
                        {.command = MediaRemoteCommand::ChangePosition, .optPositionSeconds = 1.0},
                        MediaRemoteCommandStatus::Success);
          requireStatus(handler,
                        {.command = MediaRemoteCommand::ChangePosition, .optPositionSeconds = 1.5},
                        MediaRemoteCommandStatus::Success);
          // Keep the run-loop executor's current drain occupied while its
          // separate main-queue media callbacks hand off both runtime commands.
          drainMediaCallbacks();
          AO_INVARIANT(!playback.snapshot().transport.volume.muted &&
                         playback.snapshot().transport.finalSeekRevision == started.finalSeekRevision,
                       "Native delivery must precede the backlogged runtime commands");
          AO_INVARIANT(media.diagnostics().handledCommandCount == before.handledCommandCount + 2,
                       "Valid media seeks must be handed off despite an orthogonal runtime backlog");

          if (retireAfterHandoff)
          {
            media.retire();
          }

          handedOff = true;
        });
      requireWaitUntil(
        [&]
        {
          return handedOff &&
                 playback.snapshot().transport.finalSeekRevision.value == started.finalSeekRevision.value + 2;
        },
        "Runtime-owned media seeks must finish even if their adapter retires after handoff");
      AO_INVARIANT(snapshots.size() == 4 && snapshots[1].volume.muted && snapshots[1].elapsed == started.elapsed &&
                     snapshots[2].elapsed == std::chrono::seconds{1} &&
                     snapshots[3].elapsed == std::chrono::milliseconds{1500} &&
                     snapshots[3].occurrenceId == started.occurrenceId,
                   "Backlogged media seeks must commit their captured positions in FIFO order");
    }

    void exerciseDeferredResume(LibrarySession& session)
    {
      auto& runtime = session.runtime();
      auto& playback = runtime.playback();
      auto const paused = playback.snapshot().transport;
      AO_INVARIANT(paused.transport == audio::Transport::Paused && paused.elapsed > std::chrono::milliseconds{0},
                   "Checkpoint a paused track at its nonzero seek offset");
      auto const otherId = session.rowAt(0)->id;
      AO_INVARIANT(otherId != paused.nowPlaying.trackId);
      session.select({otherId});
      auto const savedRes = runtime.savePlaybackSession();
      AO_INVARIANT(savedRes, "The paused subject and seek offset must be checkpointed");
      executeRemote(session, {.command = MediaRemoteCommand::Stop});
      waitForTransport(session, audio::Transport::Idle);
      auto* const center = MPNowPlayingInfoCenter.defaultCenter;
      AO_INVARIANT(center.nowPlayingInfo == nil, "Stop must clear Now Playing before restoring the subject");
      auto const restoredRes = runtime.restorePlaybackSession();
      AO_INVARIANT(restoredRes && restoredRes->restored, "The saved paused subject must be restorable");
      AO_INVARIANT(playback.snapshot().succession.currentTrackId == paused.nowPlaying.trackId,
                   "Restore must supply a cursor to clear independently of its held audio");
      auto* const observer = [[AobusMediaPublicationObserver alloc] initWithCenter:center];
      playback.commands().clearSequence();
      auto const& restored = playback.snapshot();
      AO_INVARIANT(restored.transport.transport == audio::Transport::Idle && restored.transport.hasDeferredResume &&
                     restored.succession.currentTrackId == kInvalidTrackId,
                   "The held audio must be independent of the succession cursor");
      auto* const info = center.nowPlayingInfo;
      AO_INVARIANT(center.playbackState == MPNowPlayingPlaybackStateStopped &&
                     [info[MPMediaItemPropertyTitle] isEqual:nativeText(paused.nowPlaying.title)] != NO &&
                     std::abs([info[MPNowPlayingInfoPropertyElapsedPlaybackTime] doubleValue] -
                              std::chrono::duration<double>{paused.elapsed}.count()) < 0.001 &&
                     [info[MPNowPlayingInfoPropertyPlaybackRate] isEqual:@0.0] != NO,
                   "Idle Now Playing must retain the held title and offset at zero rate before remote Play");
      AO_INVARIANT(observer.publicationCount == 0, "Clearing only the sequence must not republish Now Playing");
      executeRemote(session, {.command = MediaRemoteCommand::Play});
      waitForTransport(session, audio::Transport::Playing);
      auto const& resumed = playback.snapshot().transport;
      AO_INVARIANT(resumed.nowPlaying.trackId == paused.nowPlaying.trackId && resumed.elapsed >= paused.elapsed &&
                     resumed.elapsed < paused.elapsed + kResumeClockThreshold,
                   "Remote Play must restore held audio at its offset instead of starting the selected track");
    }

    rt::PlaybackTransportSnapshot startPausedSeekBoundaryTrack(LibrarySession& session,
                                                               std::chrono::milliseconds duration)
    {
      auto trackId = kInvalidTrackId;

      for (std::size_t index = 0; index < session.displayIndex().displayCount(); ++index)
      {
        if (auto const* row = session.rowAt(index); row != nullptr && row->duration == duration)
        {
          trackId = row->id;
          break;
        }
      }

      AO_INVARIANT(
        trackId != kInvalidTrackId, "The media fixture needs a track tagged with duration {} ms", duration.count());
      session.play(trackId);
      waitForTransport(session, audio::Transport::Playing);
      executeRemote(session, {.command = MediaRemoteCommand::Pause});
      waitForTransport(session, audio::Transport::Paused);
      auto const paused = session.runtime().playback().snapshot().transport;
      AO_INVARIANT(paused.nowPlaying.trackId == trackId && paused.duration == duration && paused.hasSeekableSubject() &&
                     paused.elapsed < paused.duration && !paused.hasDeferredResume,
                   "The seek boundary needs the requested occurrence paused before its end");
      return paused;
    }

    void seekAndRequireHeldPosition(LibrarySession& session,
                                    rt::PlaybackOccurrenceId occurrenceId,
                                    double positionSeconds,
                                    std::chrono::milliseconds expectedElapsed)
    {
      auto& playback = session.runtime().playback();
      auto const expectedRevision = playback.snapshot().transport.finalSeekRevision.value + 1;
      executeRemote(session, {.command = MediaRemoteCommand::ChangePosition, .optPositionSeconds = positionSeconds});
      requireWaitUntil([&] { return playback.snapshot().transport.finalSeekRevision.value >= expectedRevision; },
                       "A handled boundary seek must reach the runtime");
      auto const& sought = playback.snapshot().transport;
      AO_INVARIANT(sought.occurrenceId == occurrenceId && sought.transport == audio::Transport::Paused &&
                     sought.hasDeferredResume && sought.elapsed == expectedElapsed &&
                     sought.finalSeekRevision.value == expectedRevision,
                   "Media seek at {} s must commit the expected position on the captured occurrence",
                   positionSeconds);
    }

    void exerciseSeekDurationBoundary(LibrarySession& session)
    {
      auto const paused = startPausedSeekBoundaryTrack(session, kMediaSeekFixtureDuration);
      auto* const info = MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo;
      auto const endSeconds = [info[MPMediaItemPropertyPlaybackDuration] doubleValue];
      // The published seconds round-trip to 16001.999999999998 ms, so blind truncation misses exact end.
      AO_INVARIANT(endSeconds == 16.002, "Now Playing must publish the 16002 ms boundary fixture as 16.002 seconds");
      constexpr double kHalfMillisecondSeconds = 0.0005;

      // Exact end holds the paused occurrence without rendering. The next seek can then verify millisecond
      // truncation on the held cursor without depending on the audio backend's frame-based elapsed clock.
      seekAndRequireHeldPosition(session, paused.occurrenceId, endSeconds, paused.duration);
      seekAndRequireHeldPosition(session,
                                 paused.occurrenceId,
                                 endSeconds - kHalfMillisecondSeconds,
                                 paused.duration - std::chrono::milliseconds{1});

      auto& playback = session.runtime().playback();
      auto& media = session.mediaPlayer();
      auto const handler = media.remoteCommandHandler();
      // Even the first representable value beyond the published end must be rejected.
      for (auto const positionSeconds :
           {std::nextafter(endSeconds, std::numeric_limits<double>::infinity()), endSeconds + kHalfMillisecondSeconds})
      {
        auto const before = media.diagnostics();
        auto const insideEnd = playback.snapshot().transport;
        auto const status =
          handler({.command = MediaRemoteCommand::ChangePosition, .optPositionSeconds = positionSeconds});
        AO_INVARIANT(status == MediaRemoteCommandStatus::CommandFailed,
                     "A media seek at {} s beyond the end must fail admission",
                     positionSeconds);
        drainMediaCallbacks();
        auto const after = media.diagnostics();
        AO_INVARIANT(after.admittedCommandCount == before.admittedCommandCount &&
                       after.settledCommandCount == before.settledCommandCount &&
                       after.handledCommandCount == before.handledCommandCount,
                     "A rejected boundary seek must not enqueue or deliver work");
        auto const& unchanged = playback.snapshot().transport;
        AO_INVARIANT(unchanged.occurrenceId == paused.occurrenceId && unchanged.elapsed == insideEnd.elapsed &&
                       unchanged.positionRevision == insideEnd.positionRevision &&
                       unchanged.finalSeekRevision == insideEnd.finalSeekRevision,
                     "A rejected boundary seek must leave the held occurrence and position unchanged");
      }

      executeRemote(session, {.command = MediaRemoteCommand::Stop});
      waitForTransport(session, audio::Transport::Idle);
    }

    void exerciseInteriorSeekAtEnd(LibrarySession& session)
    {
      auto const paused = startPausedSeekBoundaryTrack(session, kMediaInteriorSeekFixtureDuration);
      auto* const info = MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo;
      auto const endSeconds = [info[MPMediaItemPropertyPlaybackDuration] doubleValue];
      auto const beforeEndSeconds = std::nextafter(endSeconds, 0.0);
      AO_INVARIANT(endSeconds == 15.992 && beforeEndSeconds < endSeconds &&
                     beforeEndSeconds * 1000.0 == static_cast<double>(paused.duration.count()),
                   "The preceding native double must be interior but round to the endpoint in milliseconds");

      // Hold the cursor at the end first: a wrongly quantized interior seek would leave it there.
      seekAndRequireHeldPosition(session, paused.occurrenceId, endSeconds, paused.duration);
      seekAndRequireHeldPosition(
        session, paused.occurrenceId, beforeEndSeconds, paused.duration - std::chrono::milliseconds{1});
      executeRemote(session, {.command = MediaRemoteCommand::Stop});
      waitForTransport(session, audio::Transport::Idle);
    }

    void exerciseSeek(LibrarySession& session)
    {
      auto& media = session.mediaPlayer();
      auto const handler = media.remoteCommandHandler();
      auto const duration = std::chrono::duration<double>{session.state().position.duration}.count();
      requireStatus(handler,
                    {.command = MediaRemoteCommand::ChangePosition, .optPositionSeconds = duration + 1},
                    MediaRemoteCommandStatus::CommandFailed);
      auto before = media.diagnostics();
      requireStatus(handler,
                    {.command = MediaRemoteCommand::ChangePosition, .optPositionSeconds = duration * 0.1},
                    MediaRemoteCommandStatus::Success);
      // Deliberately change the subject before allowing the admitted main-queue delivery.
      session.execute(uimodel::PlaybackCommand::Stop);
      drainMediaCallbacks();
      waitForTransport(session, audio::Transport::Idle);
      AO_INVARIANT(media.diagnostics().settledCommandCount == before.settledCommandCount + 1 &&
                     media.diagnostics().handledCommandCount == before.handledCommandCount,
                   "An admitted seek for a replaced playback occurrence must not be handled");
      executeRemote(session, {.command = MediaRemoteCommand::Play});
      waitForTransport(session, audio::Transport::Playing);

      before = media.diagnostics();
      auto const replayId = session.runtime().playback().snapshot().transport.nowPlaying.trackId;
      auto const priorRevision = session.runtime().playback().snapshot().transport.positionRevision;
      auto* const sessionBorrow = &session;
      auto const replayedPtr = std::make_shared<bool>(false);
      // Keep this serial main-queue turn occupied while the runtime's run-loop
      // executor commits the replay. The admitted seek can run only afterward.
      ::dispatch_async(::dispatch_get_main_queue(), ^{
        requireStatus(handler,
                      {.command = MediaRemoteCommand::ChangePosition, .optPositionSeconds = duration * 0.1},
                      MediaRemoteCommandStatus::Success);
        sessionBorrow->play(replayId);
        requireWaitUntil(
          [&]
          {
            auto const& transport = sessionBorrow->runtime().playback().snapshot().transport;
            return transport.nowPlaying.trackId == replayId && transport.positionRevision != priorRevision &&
                   transport.transport == audio::Transport::Playing;
          },
          "Same-track replay must commit before the queued media seek is delivered");
        *replayedPtr = true;
      });
      requireWaitUntil([&] { return *replayedPtr; }, "The controlled replay turn must complete");
      drainMediaCallbacks();
      AO_INVARIANT(media.diagnostics().handledCommandCount == before.handledCommandCount,
                   "Replaying the same TrackId must reject a seek delivery admitted for its prior occurrence");
      waitForTransport(session, audio::Transport::Playing);

      before = media.diagnostics();
      auto const revision = session.runtime().playback().snapshot().transport.finalSeekRevision.value;
      requireStatus(handler,
                    {.command = MediaRemoteCommand::ChangePosition, .optPositionSeconds = duration * 0.2},
                    MediaRemoteCommandStatus::Success);
      requireStatus(handler,
                    {.command = MediaRemoteCommand::ChangePosition, .optPositionSeconds = duration * 0.25},
                    MediaRemoteCommandStatus::Success);
      drainMediaCallbacks();
      AO_INVARIANT(media.diagnostics().handledCommandCount == before.handledCommandCount + 2 &&
                     session.runtime().playback().snapshot().transport.finalSeekRevision.value == revision + 2,
                   "Consecutive seeks on one occurrence must each publish a final seek update");
      AO_INVARIANT(session.runtime().playback().snapshot().transport.elapsed.count() + 500 >= duration * 250,
                   "The final media seek must reach its requested timeline position");
      executeRemote(session, {.command = MediaRemoteCommand::Stop});
      waitForTransport(session, audio::Transport::Idle);
    }

    void exerciseCommandAvailability(LibrarySession& session)
    {
      session.runtime().playback().commands().setRepeatMode(rt::RepeatMode::Off);
      auto const count = session.displayIndex().displayCount();
      AO_INVARIANT(count > 0, "The command-availability fixture must contain tracks");
      auto const* lastRow = session.rowAt(count - 1);
      AO_INVARIANT(lastRow != nullptr, "The last display row must identify the sequence's final track");
      session.play(lastRow->id);
      waitForTransport(session, audio::Transport::Playing);
      auto& media = session.mediaPlayer();
      AO_INVARIANT(
        !media.isCommandAvailable(MediaRemoteCommand::Next) && media.isCommandAvailable(MediaRemoteCommand::Stop),
        "The final-track fixture must distinguish navigation from transport availability");
      requireNativeCommandAvailability(media);
      executeRemote(session, {.command = MediaRemoteCommand::Pause});
      waitForTransport(session, audio::Transport::Paused);
      requireNativeCommandAvailability(media);
      executeRemote(session, {.command = MediaRemoteCommand::Stop});
      waitForTransport(session, audio::Transport::Idle);
      requireNativeCommandAvailability(media);
    }

    MediaRemoteCommandHandler exerciseRetirement(LibrarySession& session)
    {
      auto& media = session.mediaPlayer();
      auto const trackId = session.rowAt(0)->id;
      session.select({trackId});
      auto const handler = media.remoteCommandHandler();
      auto const before = media.diagnostics();
      AO_INVARIANT(before.registeredCommandCount == 7, "Snapshot updates must not duplicate command registration");
      requireStatus(handler, {.command = MediaRemoteCommand::Play}, MediaRemoteCommandStatus::Success);
      session.sealMediaPlayerAdmission();
      session.sealMediaPlayerAdmission();
      drainMediaCallbacks();
      auto const after = media.diagnostics();
      AO_INVARIANT(after.retired && after.registeredCommandCount == 0 &&
                     after.handledCommandCount == before.handledCommandCount &&
                     after.settledCommandCount == before.settledCommandCount,
                   "Retirement must unregister targets and fence already queued native deliveries");
      session.select({});
      session.select({trackId});
      requireStatus(handler, {.command = MediaRemoteCommand::Play}, MediaRemoteCommandStatus::CommandFailed);
      requireNativeCommandAvailability(media);
      AO_INVARIANT(MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo == nil &&
                     MPNowPlayingInfoCenter.defaultCenter.playbackState == MPNowPlayingPlaybackStateStopped,
                   "Retirement must clear the process's Now Playing publication");
      return handler;
    }

    void exerciseNowPlayingClock(LibrarySession& session, NSWindow* window)
    {
      // The session's original adapter is retired, so this controlled-clock
      // adapter is the sole native publisher over the same production runtime.
      auto& playback = session.runtime().playback();
      std::int32_t startAttempts = 0;
      auto actions = uimodel::PlaybackActions{playback,
                                              [&]
                                              {
                                                ++startAttempts;
                                                return false;
                                              }};
      actions.bindFocusedSelection(session.runtime().workspace(), session.runtime().views());
      auto now = std::chrono::steady_clock::time_point{};
      auto media = MediaPlayerAdapter{playback, actions, session.runtime().resourceBytes(), [&] { return now; }};
      auto const trackId = session.rowAt(0)->id;
      session.select({trackId});
      auto const before = media.diagnostics();
      requireStatus(
        media.remoteCommandHandler(), {.command = MediaRemoteCommand::Play}, MediaRemoteCommandStatus::Success);
      drainMediaCallbacks();
      auto const after = media.diagnostics();
      AO_INVARIANT(startAttempts == 1 && after.settledCommandCount == before.settledCommandCount + 1 &&
                     after.handledCommandCount == before.handledCommandCount,
                   "An admitted Play must settle unhandled when the selection start is rejected");

      session.play(trackId);
      waitForTransport(session, audio::Transport::Playing);
      auto const started = playback.snapshot().transport;
      auto* const center = MPNowPlayingInfoCenter.defaultCenter;
      auto requireElapsed = [&](std::chrono::milliseconds expectedElapsed)
      {
        AO_INVARIANT(std::abs([center.nowPlayingInfo[MPNowPlayingInfoPropertyElapsedPlaybackTime] doubleValue] -
                              std::chrono::duration<double>{expectedElapsed}.count()) < 0.001,
                     "Now Playing must retain the playback clock across metadata-only publications");
      };
      auto rename = [&](std::string const& title)
      {
        auto& editor = session.editor();
        auto const beginRes = editor.beginProperties(std::array{trackId});
        AO_INVARIANT(beginRes, "The clock regression must edit its playing fixture");
        auto const& fields = editor.state().fields;
        auto const field = std::ranges::find(
          fields, rt::TrackField::Title, [](LibraryEditorField const& value) { return value.spec.field; });
        AO_INVARIANT(field != fields.end(), "Properties must expose the title field");
        editor.editField(static_cast<std::size_t>(field - fields.begin()), title);
        editor.save();
        requireWaitUntil([&] { return editor.state().completed; }, "The metadata edit must commit");
        editor.cancel();
        requireWaitUntil([&] { return playback.snapshot().transport.nowPlaying.title == title; },
                         "The committed title must reach the playback snapshot");
        AO_INVARIANT([center.nowPlayingInfo[MPMediaItemPropertyTitle] isEqual:nativeText(title)] != NO,
                     "The adapter must publish the edited title");
      };

      now += std::chrono::seconds{2};
      // Selection changes must not move the anchor used by the next metadata publication.
      session.select({});
      session.select({trackId});
      rename("Media clock title");
      AO_INVARIANT(playback.snapshot().transport.positionRevision == started.positionRevision,
                   "A title edit must not create a playback clock anchor");
      requireElapsed(started.elapsed + std::chrono::seconds{2});

      now += std::chrono::seconds{1};
      exerciseSelectedArtwork(session, window, trackId);
      requireWaitUntil([&] { return center.nowPlayingInfo[MPMediaItemPropertyArtwork] != nil; },
                       "The new cover must reach native Now Playing");
      requireElapsed(started.elapsed + std::chrono::seconds{3});

      playback.commands().pause();
      waitForTransport(session, audio::Transport::Paused);
      auto const pausedElapsed = playback.snapshot().transport.elapsed;
      now += std::chrono::seconds{2};
      rename("Paused media clock title");
      requireElapsed(pausedElapsed);

      playback.commands().resume();
      waitForTransport(session, audio::Transport::Playing);
      playback.commands().seek(std::chrono::seconds{1});
      now += std::chrono::seconds{1};
      rename("Sought media clock title");
      requireElapsed(std::chrono::seconds{2});

      playback.commands().pause();
      waitForTransport(session, audio::Transport::Paused);
      auto const paused = playback.snapshot().transport;
      auto const readyPtr = std::make_shared<bool>(false);
      auto cachedRequest = session.runtime().resourceBytes().request(
        paused.nowPlaying.coverArtId, [readyPtr](rt::ResourceBytes bytes) { *readyPtr = !bytes.empty(); });
      AO_INVARIANT(*readyPtr && !cachedRequest, "The artwork publication regression must start with cached bytes");
      media.retire();
      auto* const observer = [[AobusMediaPublicationObserver alloc] initWithCenter:center];
      auto cachedMedia = MediaPlayerAdapter{playback, actions, session.runtime().resourceBytes(), [&] { return now; }};
      AO_INVARIANT(observer.publicationCount == 1,
                   "A synchronous artwork cache hit must publish native Now Playing exactly once, observed {}",
                   static_cast<std::uint64_t>(observer.publicationCount));
      AO_INVARIANT(
        center.nowPlayingInfo[MPMediaItemPropertyArtwork] != nil &&
          [center.nowPlayingInfo[MPMediaItemPropertyTitle] isEqual:nativeText(paused.nowPlaying.title)] != NO,
        "The single cache-hit publication must contain the current metadata and artwork");
      requireElapsed(paused.elapsed);
      exerciseBackloggedSeek(session, cachedMedia, true);
      playback.commands().stop();
      waitForTransport(session, audio::Transport::Idle);
      AO_INVARIANT(center.nowPlayingInfo == nil, "Stop must clear the controlled-clock publication");
    }
  } // namespace

  std::int32_t runMediaScenario(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot)
  {
    auto* const window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 400)
                                                     styleMask:NSWindowStyleMaskTitled
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
    window.releasedWhenClosed = NO;
    [window makeKeyAndOrderFront:nil];
    settleNativeCallbacks();
    auto retiredHandler = MediaRemoteCommandHandler{};

    for (auto const& command : unsupportedNativeCommands())
    {
      command.enabled = YES;
    }

    {
      auto fixture = SessionFixture{musicRoot, stateRoot};
      auto& session = fixture.session();
      // Sequence ordinals require a flat fixture, independently of the frontend's new-view recommendation.
      auto const presentationRes = session.setPresentation(ao::rt::BuiltinPreset::Library);
      AO_INVARIANT(presentationRes);
      requireWaitUntil([&] { return session.displayIndex().displayCount() == session.displayIndex().rowCount(); },
                       "The media sequence fixture must settle into its explicit flat presentation");
      exerciseIdle(session.mediaPlayer());
      [window orderOut:nil];
      AO_INVARIANT(window.visible == NO, "Media commands must work with the application window hidden");
      exerciseVisibleSelectionPlay(session);
      exerciseMetadata(session);
      exerciseForeignPause(session);
      exerciseTransport(session);
      exerciseConsecutiveNext(session);
      exerciseBackloggedSeek(session, session.mediaPlayer(), false);
      exerciseDeferredResume(session);
      exerciseSeek(session);
      exerciseSeekDurationBoundary(session);
      exerciseInteriorSeekAtEnd(session);
      exerciseFilteredRemotePlay(session);
      exerciseCommandAvailability(session);
      retiredHandler = exerciseRetirement(session);
      exerciseNowPlayingClock(session, window);
    }
    // This callback owns only the retired admission state, never the destroyed session.
    requireStatus(retiredHandler, {.command = MediaRemoteCommand::Play}, MediaRemoteCommandStatus::CommandFailed);
    drainMediaCallbacks();
    [window close];
    APP_LOG_INFO("MEDIA PLAYER PASS: metadata, hidden-window commands, Play targets and rejected starts, stale seeks, "
                 "and retirement");
    return 0;
  }
} // namespace ao::appkit::test
