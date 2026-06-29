// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include <ao/CoreIds.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt
{
  struct MetadataPatch final
  {
    std::optional<std::string> optTitle{};
    std::optional<std::string> optArtist{};
    std::optional<std::string> optAlbum{};
    std::optional<std::string> optAlbumArtist{};
    std::optional<std::string> optGenre{};
    std::optional<std::string> optComposer{};
    std::optional<std::string> optWork{};
    std::optional<std::string> optMovement{};
    std::optional<std::uint16_t> optYear{};
    std::optional<std::uint16_t> optTrackNumber{};
    std::optional<std::uint16_t> optTrackTotal{};
    std::optional<std::uint16_t> optDiscNumber{};
    std::optional<std::uint16_t> optDiscTotal{};
    std::optional<std::uint16_t> optMovementNumber{};
    std::optional<std::uint16_t> optMovementTotal{};

    std::map<std::string, std::optional<std::string>> customUpdates{};
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
} // namespace ao::rt
