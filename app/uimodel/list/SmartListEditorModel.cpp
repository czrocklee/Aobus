// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/Type.h"
#include "ao/rt/LibraryMutationService.h"
#include <ao/uimodel/list/SmartListEditorModel.h>

#include <format>
#include <string>
#include <string_view>

namespace ao::uimodel::list
{
  std::string SmartListEditorModel::composeEffectiveExpression(std::string_view parent, std::string_view local)
  {
    if (parent.empty())
    {
      return std::string{local};
    }

    if (local.empty())
    {
      return std::string{parent};
    }

    return std::format("({}) and ({})", parent, local);
  }

  bool SmartListEditorModel::canSubmit(std::string_view name, SmartListStatus status)
  {
    return !name.empty() && (status != SmartListStatus::InvalidExpression);
  }

  rt::LibraryMutationService::ListDraft SmartListEditorModel::createDraft(ListId parentListId,
                                                                          ListId editListId,
                                                                          std::string const& name,
                                                                          std::string const& description,
                                                                          std::string const& expression)
  {
    auto draftData = rt::LibraryMutationService::ListDraft{};
    draftData.kind = rt::LibraryMutationService::ListKind::Smart;
    draftData.parentId = parentListId;
    draftData.listId = editListId;
    draftData.name = name;
    draftData.description = description;
    draftData.expression = expression;
    return draftData;
  }
} // namespace ao::uimodel::list
