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
  async::Task<Result<ListId>> saveListAsync(rt::Library* const library, rt::ListDraft draft)
  {
    auto& commands = library->commands();

    if (draft.listId == kInvalidListId)
    {
      co_return co_await commands.createListAsync(std::move(draft));
    }

    auto const listId = draft.listId;

    if (auto res = co_await commands.updateListAsync(std::move(draft)); !res)
    {
      co_return std::unexpected{std::move(res).error()};
    }

    co_return listId;
  }

  async::Task<Result<rt::DeleteListSubtreeReply>> previewListDeletionAsync(rt::Library* const library,
                                                                           ListId const listId,
                                                                           bool const includeDescendants)
  {
    auto& commands = library->commands();

    if (includeDescendants)
    {
      co_return co_await commands.previewDeleteListAndDescendantsAsync(listId);
    }

    auto res = co_await commands.previewDeleteListAsync(listId);

    if (!res)
    {
      co_return std::unexpected{std::move(res).error()};
    }

    co_return rt::DeleteListSubtreeReply{.rootListId = listId, .deletedLists = {std::move(*res)}};
  }

  async::Task<Result<rt::DeleteListSubtreeReply>> deleteListAsync(rt::Library* const library,
                                                                  ListId const listId,
                                                                  bool const includeDescendants,
                                                                  rt::DeleteListOptions const options)
  {
    auto& commands = library->commands();

    if (includeDescendants)
    {
      co_return co_await commands.deleteListAndDescendantsAsync(listId, options);
    }

    auto res = co_await commands.deleteListAsync(listId, options);

    if (!res)
    {
      co_return std::unexpected{std::move(res).error()};
    }

    co_return rt::DeleteListSubtreeReply{.rootListId = listId, .deletedLists = {std::move(*res)}};
  }
} // namespace ao::uimodel
