// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/list/ListAuthoring.h>

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>

#include <expected>
#include <utility>

namespace ao::uimodel
{
  async::Task<Result<ListId>> saveList(rt::Library* const library, rt::ListDraft draft)
  {
    auto& commands = library->commands();

    if (draft.listId == kInvalidListId)
    {
      co_return co_await commands.createList(std::move(draft));
    }

    auto const listId = draft.listId;

    if (auto result = co_await commands.updateList(std::move(draft)); !result)
    {
      co_return std::unexpected{std::move(result).error()};
    }

    co_return listId;
  }

  async::Task<Result<rt::DeleteListSubtreeReply>> previewListDeletion(rt::Library* const library,
                                                                      ListId const listId,
                                                                      bool const includeDescendants)
  {
    auto& commands = library->commands();

    if (includeDescendants)
    {
      co_return co_await commands.previewDeleteListAndDescendants(listId);
    }

    auto result = co_await commands.previewDeleteList(listId);

    if (!result)
    {
      co_return std::unexpected{std::move(result).error()};
    }

    co_return rt::DeleteListSubtreeReply{.rootListId = listId, .deletedLists = {std::move(*result)}};
  }

  async::Task<Result<rt::DeleteListSubtreeReply>> deleteList(rt::Library* const library,
                                                             ListId const listId,
                                                             bool const includeDescendants,
                                                             rt::DeleteListOptions const options)
  {
    auto& commands = library->commands();

    if (includeDescendants)
    {
      co_return co_await commands.deleteListAndDescendants(listId, options);
    }

    auto result = co_await commands.deleteList(listId, options);

    if (!result)
    {
      co_return std::unexpected{std::move(result).error()};
    }

    co_return rt::DeleteListSubtreeReply{.rootListId = listId, .deletedLists = {std::move(*result)}};
  }
} // namespace ao::uimodel
