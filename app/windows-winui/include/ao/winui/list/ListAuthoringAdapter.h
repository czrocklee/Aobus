// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/TrackPresentation.h>

#include <cstdint>
#include <map>
#include <string_view>

namespace ao::rt
{
  struct LibraryChangeSet;
}

namespace ao::uimodel
{
  class ListPresentations;
  struct ListTreeProjection;
}

namespace ao::winui
{
  enum class ListOrderCommand : std::uint8_t
  {
    MoveUp,
    MoveDown,
    MoveToTop,
    MoveToBottom,
    Reset,
  };

  struct ListTreeRestoreState final
  {
    ListId selectedListId = kInvalidListId;
    std::map<ListId, bool> expandedById{};

    bool operator==(ListTreeRestoreState const&) const = default;
  };

  /// Whether one committed library publication changes the saved-List tree.
  bool listTreeChangeRequiresRebuild(rt::LibraryChangeSet const& changeSet) noexcept;

  /// Resolve the presentation that an authored saved List must display after save.
  rt::TrackPresentationSpec resolveListAuthoringPresentation(uimodel::ListPresentations const& listPresentations,
                                                             ListId listId,
                                                             std::string_view localExpression);

  /**
   * Resolve selection and expansion after native navigation nodes are replaced.
   *
   * Expansion is retained only for surviving ids. Newly introduced parents
   * start expanded, and a missing active List falls back to All Tracks.
   */
  ListTreeRestoreState restoreListTreeState(uimodel::ListTreeProjection const& projection,
                                            ListId activeListId,
                                            std::map<ListId, bool> const& previousExpansion);
} // namespace ao::winui
