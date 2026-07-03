// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/library/property/TagEditWorkflow.h>

#include <cstddef>
#include <format>
#include <string>
#include <vector>

namespace ao::uimodel
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

  TagEditWorkflow::TagEditWorkflow(rt::LibraryWriter& writer)
    : _writer{writer}
  {
  }

  TagEditResult TagEditWorkflow::apply(TagEditRequest const& request)
  {
    auto result = TagEditResult{};

    if (request.selectedIds.empty() || (request.tagsToAdd.empty() && request.tagsToRemove.empty()))
    {
      return result;
    }

    auto const replyResult = _writer.editTags(request.selectedIds, request.tagsToAdd, request.tagsToRemove);

    if (!replyResult)
    {
      result.rejected = true;
      result.notificationText = replyResult.error().message;
      return result;
    }

    result.applied = !replyResult->mutatedIds.empty();
    result.notificationText =
      tagChangeStatusMessage(replyResult->mutatedIds.size(), request.tagsToAdd.size(), request.tagsToRemove.size());
    return result;
  }
} // namespace ao::uimodel
