// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/Type.h>

#include <optional>
#include <string>
#include <vector>

namespace ao::rt
{
  class LibraryMutationService;
}

namespace ao::gtk
{
  struct TagEditRequest final
  {
    std::vector<TrackId> selectedIds;
    std::vector<std::string> tagsToAdd;
    std::vector<std::string> tagsToRemove;
  };

  struct TagEditResult final
  {
    bool applied = false;
    std::string notificationText;
    std::optional<Error> optError;
  };

  class TagEditWorkflow final
  {
  public:
    explicit TagEditWorkflow(rt::LibraryMutationService& mutation);

    TagEditResult apply(TagEditRequest const& request);

  private:
    rt::LibraryMutationService& _mutation;
  };
} // namespace ao::gtk
