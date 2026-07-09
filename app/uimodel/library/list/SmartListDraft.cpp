// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/library/LibraryWriter.h>
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

  rt::LibraryWriter::ListDraft makeSmartListDraft(ListId parentListId,
                                                  ListId editListId,
                                                  std::string const& name,
                                                  std::string const& description,
                                                  std::string const& expression)
  {
    auto draft = rt::LibraryWriter::ListDraft{};
    draft.kind = rt::LibraryWriter::ListKind::Smart;
    draft.parentId = parentListId;
    draft.listId = editListId;
    draft.name = name;
    draft.description = description;
    draft.expression = expression;
    return draft;
  }
} // namespace ao::uimodel
