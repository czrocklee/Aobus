// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>

#include <string>
#include <vector>

namespace ao::rt
{
  class LibraryWriter;
}

namespace ao::uimodel::tag
{
  struct TagEditRequest final
  {
    std::vector<TrackId> selectedIds = {};
    std::vector<std::string> tagsToAdd = {};
    std::vector<std::string> tagsToRemove = {};
  };

  struct TagEditResult final
  {
    bool applied = false;
    std::string notificationText;
  };

  class TagEditWorkflow final
  {
  public:
    explicit TagEditWorkflow(rt::LibraryWriter& writer);

    TagEditResult apply(TagEditRequest const& request);

  private:
    rt::LibraryWriter& _writer;
  };
} // namespace ao::uimodel::tag
