// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/ListMutation.h>

namespace ao::rt
{
  class Library;
}

namespace ao::uimodel
{
  /** Platform-neutral list mutation workflow used by frontend controllers. */
  class ListEditWorkflow final
  {
  public:
    explicit ListEditWorkflow(rt::Library& library) noexcept;

    Result<ListId> create(rt::LibraryListDraft const& draft) const;
    Result<rt::UpdateListReply> update(rt::LibraryListDraft const& draft) const;
    Result<rt::DeleteListReply> remove(ListId listId) const;

  private:
    rt::Library& _library;
  };
} // namespace ao::uimodel
