// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/audio/Types.h>

#include "CorePrimitives.h"
#include "StateTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ao::app
{
  // -- Playback events --------------------------------------------------------

  struct PlaybackTransportChanged final
  {
    ao::audio::Transport transport = ao::audio::Transport::Idle;
  };

  struct NowPlayingTrackChanged final
  {
    ao::TrackId trackId{};
    ao::ListId sourceListId{};
  };

  struct PlaybackOutputChanged final
  {
    OutputSelection selection{};
  };

  struct PlaybackStopped final
  {};

  struct PlaybackDevicesChanged final
  {};

  struct PlaybackQualityChanged final
  {
    ao::audio::Quality quality = ao::audio::Quality::Unknown;
    bool ready = false;
  };

  // -- Library events ----------------------------------------------------------

  struct TracksMutated final
  {
    std::vector<ao::TrackId> trackIds{};
  };

  struct ListsMutated final
  {
    std::vector<ao::ListId> listIds{};
  };

  struct LibraryImportCompleted final
  {
    std::size_t importedTrackCount = 0;
  };

  struct ImportProgressUpdated final
  {
    double fraction = 0.0;
    std::string message{};
  };

  // -- View events -------------------------------------------------------------

  struct FocusedViewChanged final
  {
    ViewId viewId{};
  };

  struct ViewSelectionChanged final
  {
    ViewId viewId{};
    std::vector<ao::TrackId> selection{};
  };

  struct ViewDestroyed final
  {
    ViewId viewId{};
  };

  struct ViewFilterChanged final
  {
    ViewId viewId{};
    std::string filterExpression{};
  };

  struct ViewSortChanged final
  {
    ViewId viewId{};
    std::vector<TrackSortTerm> sortBy{};
  };

  struct ViewGroupingChanged final
  {
    ViewId viewId{};
    TrackGroupKey groupBy = TrackGroupKey::None;
  };

  struct ViewListChanged final
  {
    ViewId viewId{};
    ao::ListId listId{};
  };

  struct SessionRestored final
  {
    std::string libraryPath{};
  };

  struct RevealTrackRequested final
  {
    ao::TrackId trackId{};
    ao::ListId preferredListId{};
    ViewId preferredViewId{};
  };

  // -- Notification events -----------------------------------------------------

  struct NotificationPosted final
  {
    NotificationId id{};
  };

  struct NotificationDismissed final
  {
    NotificationId id{};
  };
}
