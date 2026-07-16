// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <string>
#include <vector>

namespace ao::uimodel
{
  class TrackAuthoringSession;

  struct TagEditRequest final
  {
    std::vector<TrackId> selectedIds = {};
    std::vector<std::string> tagsToAdd = {};
    std::vector<std::string> tagsToRemove = {};
  };

  struct TagEditResult final
  {
    bool applied = false;
    bool rejected = false;
    bool stale = false;
    std::string notificationText;
  };

  class TagEditWorkflow final
  {
  public:
    explicit TagEditWorkflow(TrackAuthoringSession& session);

    TagEditResult apply(TagEditRequest const& request);

  private:
    TrackAuthoringSession& _session;
  };
} // namespace ao::uimodel
