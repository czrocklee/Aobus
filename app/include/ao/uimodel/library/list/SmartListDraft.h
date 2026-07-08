// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/library/list/SmartListPreview.h>

#include <string>
#include <string_view>

namespace ao::uimodel
{
  bool canSubmitSmartListDraft(std::string_view name, SmartListPreviewStatus status);

  rt::LibraryWriter::ListDraft makeSmartListDraft(ListId parentListId,
                                                  ListId editListId,
                                                  std::string const& name,
                                                  std::string const& description,
                                                  std::string const& expression);
} // namespace ao::uimodel
