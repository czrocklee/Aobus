// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <ryml.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::rt
{
  struct WorkspaceSessionState;
}

namespace ao::rt::detail
{
  inline constexpr std::uint32_t kWorkspacePresentationVersion = 1;

  // Persistence DTOs isolate the versioned wire shape from live workspace state.
  struct StoredTrackSortTerm final
  {
    std::string field{};
    std::string direction{};
  };

  struct StoredTrackPresentationSpec final
  {
    std::string id{};
    std::string group{};
    std::vector<StoredTrackSortTerm> sort{};
    std::vector<std::string> visibleFields{};
    std::vector<std::string> redundantFields{};
  };

  struct StoredTrackListViewConfig final
  {
    std::uint32_t listId = 0;
    std::string filterExpression{};
    StoredTrackPresentationSpec presentation{};
  };

  struct StoredCustomTrackPresentationPreset final
  {
    std::string label{};
    std::string basePresetId{};
    StoredTrackPresentationSpec spec{};
  };

  struct WorkspaceSessionDocument final
  {
    std::uint32_t presentationVersion = kWorkspacePresentationVersion;
    std::vector<StoredTrackListViewConfig> openViews{};
    std::uint32_t activeListId = 0;
    std::vector<StoredCustomTrackPresentationPreset> customPresets{};
  };

  Result<WorkspaceSessionDocument> toWorkspaceSessionDocument(WorkspaceSessionState const& state);
  Result<WorkspaceSessionState> workspaceSessionStateFromDocument(WorkspaceSessionDocument const& document);

  struct WorkspaceSessionYamlSchema final
  {
    Result<> serialize(ryml::NodeRef node, WorkspaceSessionState const& state) const;
    Result<WorkspaceSessionState> deserialize(ryml::ConstNodeRef node, WorkspaceSessionState const& seed) const;
  };
} // namespace ao::rt::detail
