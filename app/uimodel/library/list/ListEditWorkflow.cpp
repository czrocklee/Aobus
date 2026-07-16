// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/library/list/ListEditWorkflow.h>

namespace ao::uimodel
{
  ListEditWorkflow::ListEditWorkflow(rt::Library& library) noexcept
    : _library{library}
  {
  }

  Result<ListId> ListEditWorkflow::create(rt::LibraryListDraft const& draft) const
  {
    return _library.writer().createList(draft);
  }

  Result<rt::UpdateListReply> ListEditWorkflow::update(rt::LibraryListDraft const& draft) const
  {
    return _library.writer().updateList(draft);
  }

  Result<rt::DeleteListReply> ListEditWorkflow::remove(ListId listId) const
  {
    return _library.writer().deleteList(listId);
  }
} // namespace ao::uimodel
