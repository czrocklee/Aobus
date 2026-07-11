// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "runtime/playback/PlaybackSessionRevision.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/Signal.h>
#include <ao/rt/Subscription.h>

#include <chrono>
#include <functional>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class ConfigStore;
  class Library;
  class PlaybackSequenceService;
  class PlaybackService;

  struct PlaybackSessionRestoreOutcome final
  {
    bool restored = false;
    TrackId trackId = kInvalidTrackId;
    ListId sourceListId = kInvalidListId;
  };

  /** Executor-affine owner of serialized playback intent and dirty acknowledgement. */
  class PlaybackSessionPersistence final
  {
  public:
    PlaybackSessionPersistence(ConfigStore& config,
                               Library& library,
                               library::MusicLibrary& storage,
                               PlaybackSequenceService& sequence,
                               PlaybackService& playback);
    ~PlaybackSessionPersistence();

    PlaybackSessionPersistence(PlaybackSessionPersistence const&) = delete;
    PlaybackSessionPersistence& operator=(PlaybackSessionPersistence const&) = delete;
    PlaybackSessionPersistence(PlaybackSessionPersistence&&) = delete;
    PlaybackSessionPersistence& operator=(PlaybackSessionPersistence&&) = delete;

    Result<> save();
    Result<PlaybackSessionRestoreOutcome> restore();
    Result<> forget();
    Subscription onDirty(std::move_only_function<void()> handler);

  private:
    void markDirty();
    bool hasActiveSession() const;
    bool hasRestorableSession() const;

    ConfigStore& _config;
    Library& _library;
    library::MusicLibrary& _storage;
    PlaybackSequenceService& _sequence;
    PlaybackService& _playback;
    Signal<> _dirtySignal;
    Subscription _sequenceIntentSubscription;
    Subscription _volumeSubscription;
    Subscription _mutedSubscription;
    Subscription _seekSubscription;
    PlaybackSessionRevision _sessionRevision;
    bool _forgotten = false;
    bool _restoring = false;
    std::chrono::milliseconds _intentPosition{0};
    float _volumeIntent = 1.0F;
    bool _mutedIntent = false;
  };
} // namespace ao::rt
