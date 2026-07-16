// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/ListMutation.h>
#include <ao/uimodel/library/list/SmartListDraft.h>
#include <ao/uimodel/library/list/SmartListPreview.h>

#include <string>
#include <string_view>

namespace ao::uimodel
{
  bool canSubmitSmartListDraft(std::string_view name, SmartListPreviewStatus status)
  {
    return !name.empty() && (status != SmartListPreviewStatus::InvalidExpression);
  }

  rt::LibraryListDraft makeSmartListDraft(ListId parentListId,
                                          ListId editListId,
                                          std::string const& name,
                                          std::string const& description,
                                          std::string const& expression)
  {
    auto draft = rt::LibraryListDraft{};
    draft.kind = rt::LibraryListKind::Smart;
    draft.parentId = parentListId;
    draft.listId = editListId;
    draft.name = name;
    draft.description = description;
    draft.expression = expression;
    return draft;
  }
} // namespace ao::uimodel
