// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackPresentation.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>
#include <ao/uimodel/library/track/TrackFilterView.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>
#include <ao/uimodel/playback/output/VolumeViewModel.h>
#include <ao/uimodel/playback/seek/PlaybackPosition.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ao::appkit
{
  inline constexpr std::size_t kPlaybackCommandCapacity = 8;

  // Native views sample this main-thread state; runtime observers never enter AppKit.
  struct DesktopViewState final
  {
    std::uint64_t tableRevision = 0;
    std::uint64_t listRevision = 0;
    uimodel::NowPlayingViewState nowPlaying{};
    uimodel::OutputDeviceViewState output{};
    uimodel::VolumeViewState volume{};
    uimodel::PlaybackPositionViewState position{};
    rt::PlaybackPositionRevision positionRevision{};
    uimodel::AobusSoulViewState soul{};
    uimodel::TrackFilterViewState filter{};
    uimodel::TrackPresentationPickerState presentation{};
    std::optional<rt::TrackPresetId> optListPresentationId{};
    uimodel::ActivityStatusViewState activity{};
    std::array<uimodel::TransportViewState, kPlaybackCommandCapacity> transport{};
    rt::ResourceBytes playingCover{};
    rt::ResourceBytes selectedCover{};
    bool scanning = false;
  };
} // namespace ao::appkit
