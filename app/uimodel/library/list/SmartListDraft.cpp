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
    auto draftData = rt::LibraryWriter::ListDraft{};
    draftData.kind = rt::LibraryWriter::ListKind::Smart;
    draftData.parentId = parentListId;
    draftData.listId = editListId;
    draftData.name = name;
    draftData.description = description;
    draftData.expression = expression;
    return draftData;
  }
} // namespace ao::uimodel
