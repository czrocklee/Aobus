// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Types.h>
#include <ao/audio/flow/Graph.h>

#include "CorePrimitives.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt
{
  inline constexpr std::string_view kDefaultTrackPresentationId = "songs";

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

  struct PlaybackState final
  {
    audio::Transport transport = audio::Transport::Idle;
    TrackId trackId{};
    ListId sourceListId{};
    std::string trackTitle{};
    std::string trackArtist{};
    std::uint32_t positionMs = 0;
    std::uint32_t durationMs = 0;
    float volume = 1.0F;
    bool muted = false;
    bool volumeAvailable = false;
    bool ready = false;

    OutputSelection selectedOutput{};
    std::vector<OutputBackendSnapshot> availableOutputs{};
    audio::flow::Graph flow{};
    audio::Quality quality = audio::Quality::Unknown;
    std::string qualityTooltip{};
    std::uint64_t revision = 0;
  };

  struct LayoutState final
  {
    ViewId activeViewId{};
    std::vector<ViewId> openViews{};
    std::vector<ViewId> navigationStack{};
    std::uint64_t revision = 0;
  };

  enum class NotificationSeverity : std::uint8_t
  {
    Info,
    Warning,
    Error,
  };

  struct NotificationEntry final
  {
    NotificationId id{};
    NotificationSeverity severity = NotificationSeverity::Info;
    std::string message{};
    bool sticky = false;
    std::optional<std::chrono::milliseconds> optTimeout{};
  };

  struct NotificationFeedState final
  {
    std::vector<NotificationEntry> entries{};
    std::uint64_t revision = 0;
  };

  enum class TrackGroupKey : std::uint8_t
  {
    None,
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Composer,
    Work,
    Year,
  };

  enum class TrackSortField : std::uint8_t
  {
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Composer,
    Work,
    Year,
    DiscNumber,
    TrackNumber,
    Title,
    Duration,
  };

  struct TrackSortTerm final
  {
    TrackSortField field = TrackSortField::Title;
    bool ascending = true;

    bool operator==(TrackSortTerm const&) const = default;
  };

  enum class TrackPresentationField : std::uint8_t
  {
    Title,
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Composer,
    Work,
    Year,
    DiscNumber,
    TrackNumber,
    Duration,
    Tags,
  };

  struct TrackListPresentationState final
  {
    std::string presentationId = std::string{kDefaultTrackPresentationId};
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::vector<TrackPresentationField> visibleFields{};
    std::vector<TrackPresentationField> redundantFields{};

    bool operator==(TrackListPresentationState const&) const = default;
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
    TrackListPresentationState presentation{};
    std::uint64_t revision = 0;
  };

  struct TrackListViewConfig final
  {
    ListId listId{};
    std::string filterExpression{};
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> sortBy{};
    std::vector<TrackId> selection{};
    TrackListPresentationState presentation{};
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
    std::optional<std::string> optGenre{};
    std::optional<std::string> optComposer{};
    std::optional<std::string> optWork{};
  };

  struct UpdateTrackMetadataReply final
  {
    std::vector<TrackId> mutatedIds;
  };

  struct EditTrackTagsReply final
  {
    std::vector<TrackId> mutatedIds;
  };

  struct ImportFilesReply final
  {
    std::size_t importedTrackCount = 0;
  };

  struct CreateTrackListViewReply final
  {
    ViewId viewId{};
  };

  struct SessionSnapshot final
  {
    std::string lastLibraryPath;
    std::string lastBackend;
    std::string lastProfile;
    std::string lastOutputDeviceId;
    std::vector<TrackListViewConfig> openViews;
    std::optional<std::size_t> optActiveViewIndex;
  };
} // namespace ao::rt
