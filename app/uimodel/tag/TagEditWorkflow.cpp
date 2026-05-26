// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/LibraryMutationService.h>
#include <ao/uimodel/tag/TagEditWorkflow.h>

#include <cstddef>
#include <format>
#include <string>
#include <vector>

namespace ao::uimodel::tag
{
  namespace
  {
    std::string tagChangeStatusMessage(std::size_t trackCount, std::size_t addedCount, std::size_t removedCount)
    {
      auto parts = std::vector<std::string>{};

      if (addedCount > 0)
      {
        parts.push_back(std::format("added {}", addedCount));
      }

      if (removedCount > 0)
      {
        parts.push_back(std::format("removed {}", removedCount));
      }

      if (parts.empty())
      {
        return std::format("Tags unchanged for {} track{}", trackCount, trackCount == 1 ? "" : "s");
      }

      auto message = std::format("Tags {}", parts[0]);

      if (parts.size() > 1)
      {
        message += " and " + parts[1];
      }

      return std::format("{} for {} track{}", message, trackCount, trackCount == 1 ? "" : "s");
    }
  } // namespace

  TagEditWorkflow::TagEditWorkflow(rt::LibraryMutationService& mutation)
    : _mutation{mutation}
  {
  }

  TagEditResult TagEditWorkflow::apply(TagEditRequest const& request)
  {
    auto result = TagEditResult{};

    if (request.selectedIds.empty() || (request.tagsToAdd.empty() && request.tagsToRemove.empty()))
    {
      return result;
    }

    auto const editResult = _mutation.editTags(request.selectedIds, request.tagsToAdd, request.tagsToRemove);

    if (!editResult)
    {
      result.optError = editResult.error();
      result.notificationText = std::format("Failed to edit tags: {}", result.optError->message);
      return result;
    }

    result.applied = true;
    result.notificationText =
      tagChangeStatusMessage(request.selectedIds.size(), request.tagsToAdd.size(), request.tagsToRemove.size());
    return result;
  }
} // namespace ao::uimodel::tag
