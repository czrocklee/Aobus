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

  struct PlaybackFaultTransition final
  {
    std::optional<FaultSnapshot> optFault{};
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

  struct ViewDestroyed final
  {
    ViewId viewId{};
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
