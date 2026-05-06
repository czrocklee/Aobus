// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Types.h>

#include "CorePrimitives.h"
#include "StateTypes.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ao::model
{
  class TrackIdList;
}

namespace ao::app
{
  struct PlayTrack final
  {
    using Reply = void;

    ao::audio::TrackPlaybackDescriptor descriptor{};
    ao::ListId sourceListId{};
  };

  struct PlaySelectionInView final
  {
    using Reply = ao::TrackId;

    ViewId viewId{};
  };

  struct PlaySelectionInFocusedView final
  {
    using Reply = ao::TrackId;
  };

  struct PausePlayback final
  {
    using Reply = void;
  };

  struct ResumePlayback final
  {
    using Reply = void;
  };

  struct StopPlayback final
  {
    using Reply = void;
  };

  struct SeekPlayback final
  {
    using Reply = void;

    std::uint32_t positionMs = 0;
  };

  struct SetPlaybackOutput final
  {
    using Reply = void;

    ao::audio::BackendId backendId{};
    ao::audio::DeviceId deviceId{};
    ao::audio::ProfileId profileId{};
  };

  struct SetPlaybackVolume final
  {
    using Reply = void;

    float volume = 1.0f;
  };

  struct SetPlaybackMuted final
  {
    using Reply = void;

    bool muted = false;
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
    std::vector<ao::TrackId> mutatedIds{};
  };

  struct UpdateTrackMetadata final
  {
    using Reply = UpdateTrackMetadataReply;

    std::vector<ao::TrackId> trackIds{};
    MetadataPatch patch{};
  };

  struct EditTrackTagsReply final
  {
    std::vector<ao::TrackId> mutatedIds{};
  };

  struct EditTrackTags final
  {
    using Reply = EditTrackTagsReply;

    std::vector<ao::TrackId> trackIds{};
    std::vector<std::string> tagsToAdd{};
    std::vector<std::string> tagsToRemove{};
  };

  struct ImportFilesReply final
  {
    std::size_t importedTrackCount = 0;
  };

  struct ImportFiles final
  {
    using Reply = ImportFilesReply;

    std::vector<std::filesystem::path> paths{};
  };

  struct CreateTrackListViewReply final
  {
    ViewId viewId{};
  };

  struct CreateTrackListView final
  {
    using Reply = CreateTrackListViewReply;

    TrackListViewConfig initial{};
    bool attached = true;
    std::shared_ptr<ao::model::TrackIdList> source{};
  };

  struct AttachView final
  {
    using Reply = void;

    ViewId viewId{};
  };

  struct DetachView final
  {
    using Reply = void;

    ViewId viewId{};
  };

  struct DestroyView final
  {
    using Reply = void;

    ViewId viewId{};
  };

  struct OpenListInView final
  {
    using Reply = void;

    ViewId viewId{};
    ao::ListId listId{};
  };

  struct SetViewFilter final
  {
    using Reply = void;

    ViewId viewId{};
    std::string filterExpression{};
  };

  struct SetViewGrouping final
  {
    using Reply = void;

    ViewId viewId{};
    TrackGroupKey groupBy = TrackGroupKey::None;
  };

  struct SetViewSort final
  {
    using Reply = void;

    ViewId viewId{};
    std::vector<TrackSortTerm> sortBy{};
  };

  struct SetViewSelection final
  {
    using Reply = void;

    ViewId viewId{};
    std::vector<ao::TrackId> selection{};
  };

  struct SetFocusedView final
  {
    using Reply = void;

    ViewId viewId{};
  };

  struct RefreshPlaybackState final
  {
    using Reply = void;
  };

  struct RevealPlayingTrack final
  {
    using Reply = void;
  };

  struct PostNotification final
  {
    using Reply = NotificationId;

    NotificationSeverity severity = NotificationSeverity::Info;
    std::string message{};
    bool sticky = false;
    std::optional<std::chrono::milliseconds> optTimeout{};
  };

  struct DismissNotification final
  {
    using Reply = void;

    NotificationId id{};
  };
}
