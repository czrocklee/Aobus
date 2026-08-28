// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/ListMutation.h>

namespace ao::rt
{
  class Library;
}

namespace ao::uimodel
{
  /** Narrow List CRUD functions shared by native authoring workflows. */
  async::Task<Result<ListId>> saveList(rt::Library* library, rt::ListDraft draft);
  async::Task<Result<rt::DeleteListSubtreeReply>> previewListDeletion(rt::Library* library,
                                                                      ListId listId,
                                                                      bool includeDescendants);
  async::Task<Result<rt::DeleteListSubtreeReply>> deleteList(rt::Library* library,
                                                             ListId listId,
                                                             bool includeDescendants,
                                                             rt::DeleteListOptions options = {});
} // namespace ao::uimodel
