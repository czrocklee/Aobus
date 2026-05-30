// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "TrackField.h"
#include "TrackPresentation.h"
#include <ao/Type.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Types.h>
#include <ao/audio/flow/Graph.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  struct OutputProfileSnapshot final
  {
    audio::ProfileId id{};
    std::string name{};
    std::string description{};

    bool operator==(OutputProfileSnapshot const&) const = default;
  };

  struct OutputDeviceSnapshot final
  {
    audio::DeviceId id{};
    std::string displayName{};
    std::string description{};
    bool isDefault = false;
    audio::BackendId backendId{};
    audio::DeviceCapabilities capabilities{};

    bool operator==(OutputDeviceSnapshot const&) const = default;
  };

  struct OutputBackendSnapshot final
  {
    audio::BackendId id{};
    std::string name{};
    std::string description{};
    std::string iconName{};
    std::vector<OutputProfileSnapshot> supportedProfiles{};
    std::vector<OutputDeviceSnapshot> devices{};

    bool operator==(OutputBackendSnapshot const&) const = default;
  };

  struct OutputSelection final
  {
    audio::BackendId backendId{};
    audio::DeviceId deviceId{};
    audio::ProfileId profileId{};

    bool operator==(OutputSelection const&) const = default;
  };

  enum class ShuffleMode : std::uint8_t
  {
    Off,
    On,
  };

  enum class RepeatMode : std::uint8_t
  {
    Off,
    One,
    All,
  };

  struct PlaybackState final
  {
    audio::Transport transport = audio::Transport::Idle;
    TrackId trackId{};
    ListId sourceListId = kInvalidListId;
    ViewId sourceViewId = kInvalidViewId;
    std::string trackTitle{};
    std::string trackArtist{};
    std::uint32_t positionMs = 0;
    std::uint32_t durationMs = 0;
    float volume = 1.0F;
    bool muted = false;
    bool volumeAvailable = false;
    bool ready = false;

    ShuffleMode shuffleMode = ShuffleMode::Off;
    RepeatMode repeatMode = RepeatMode::Off;

    OutputSelection selectedOutput{};
    std::vector<OutputBackendSnapshot> availableOutputs{};
    audio::flow::Graph flow{};
    audio::Quality quality = audio::Quality::Unknown;
    std::string qualityTooltip{};
    std::uint64_t revision = 0;
  };

  struct LayoutState final
  {
    ViewId activeViewId = kInvalidViewId;
    std::vector<ViewId> openViews{};
    std::uint64_t revision = 0;
  };

  enum class NotificationSeverity : std::uint8_t
  {
    Info,
    Warning,
    Error,
  };

  inline constexpr std::string_view kDefaultNotificationTemplate = "notification.message";

  enum class NotificationProgressMode : std::uint8_t
  {
    Indeterminate,
    Fraction,
  };

  struct NotificationProgressState final
  {
    NotificationProgressMode mode = NotificationProgressMode::Indeterminate;
    double fraction = 0.0;
    std::string label{};
  };

  struct NotificationAction final
  {
    std::string id{};
    std::string label{};
  };

  struct NotificationContentState final
  {
    std::string templateId = std::string{kDefaultNotificationTemplate};
    std::string title{};
    std::string iconName{};
    std::vector<NotificationAction> actions{};
    std::optional<NotificationProgressState> optProgress{};
  };

  struct NotificationRequest final
  {
    NotificationSeverity severity = NotificationSeverity::Info;
    std::string message{};
    bool sticky = false;
    std::optional<std::chrono::milliseconds> optTimeout{};
    NotificationContentState content{};
  };

  struct NotificationEntry final
  {
    NotificationId id{};
    NotificationSeverity severity = NotificationSeverity::Info;
    std::string message{};
    bool sticky = false;
    std::optional<std::chrono::milliseconds> optTimeout{};
    NotificationContentState content{};
  };

  struct NotificationFeedState final
  {
    std::vector<NotificationEntry> entries{};
    std::uint64_t revision = 0;
  };

  enum class ViewLifecycleState : std::uint8_t
  {
    Attached,
    Detached,
    Destroyed,
  };

  enum class ViewKind : std::uint8_t
  {
    TrackList,
  };

  struct TrackListViewState final
  {
    ViewId id{};
    ViewLifecycleState lifecycle = ViewLifecycleState::Detached;
    ListId listId{};
    std::string filterExpression{};
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::vector<TrackId> selection{};
    TrackPresentationSpec presentation{};
    std::uint64_t revision = 0;
  };

  struct TrackListViewConfig final
  {
    ListId listId{};
    std::string filterExpression{};
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::optional<TrackPresentationSpec> optPresentation{};
  };

  struct ViewRecord final
  {
    ViewId id{};
    ViewKind kind = ViewKind::TrackList;
    ViewLifecycleState lifecycle = ViewLifecycleState::Detached;
  };

  enum class GlobalViewKind : std::uint8_t
  {
    AllTracks,
  };

  struct MetadataPatch final
  {
    std::optional<std::string> optTitle{};
    std::optional<std::string> optArtist{};
    std::optional<std::string> optAlbum{};
    std::optional<std::string> optAlbumArtist{};
    std::optional<std::string> optGenre{};
    std::optional<std::string> optComposer{};
    std::optional<std::string> optWork{};
    std::optional<std::uint16_t> optYear{};
    std::optional<std::uint16_t> optTrackNumber{};
    std::optional<std::uint16_t> optTotalTracks{};
    std::optional<std::uint16_t> optDiscNumber{};
    std::optional<std::uint16_t> optTotalDiscs{};
  };

  struct UpdateTrackMetadataReply final
  {
    std::vector<TrackId> mutatedIds;
  };

  struct EditTrackTagsReply final
  {
    std::vector<TrackId> mutatedIds;
  };

  struct CreateTrackListViewReply final
  {
    ViewId viewId{};
  };

  struct AppPrefsState final
  {
    std::string lastLibraryPath;
    std::string lastBackend;
    std::string lastProfile;
    std::string lastOutputDeviceId;
    std::string lastLayoutPreset;
  };

  struct SessionState final
  {
    std::vector<TrackListViewConfig> openViews;
    ListId activeListId = kInvalidListId;
    std::vector<CustomTrackPresentationPreset> customPresets;
  };
} // namespace ao::rt
