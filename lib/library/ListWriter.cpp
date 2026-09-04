// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/ListWriter.h>

#include "WriteTransactionAccess.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/WriteTransaction.h>

#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::library
{
  ListWriter::ListWriter(WriteTransaction& transaction)
    : _transaction{&transaction}
  {
  }

  std::optional<ListView> ListWriter::get(ListId const id) const
  {
    requireActiveOperation();
    return detail::WriteTransactionAccess::listStoreWriter(*_transaction).get(id);
  }

  Result<> ListWriter::validateParent(ListId const targetId, ListId const parentId) const
  {
    if (parentId == kInvalidListId)
    {
      return {};
    }

    if (parentId == targetId)
    {
      return makeError(Error::Code::InvalidInput, "list parent cannot be the list itself");
    }

    auto& writer = detail::WriteTransactionAccess::listStoreWriter(*_transaction);
    auto visited = std::unordered_set<ListId>{};
    auto current = parentId;
    bool isDirectCandidateParent = true;

    while (current != kInvalidListId)
    {
      auto const inserted = visited.insert(current).second;
      AO_INVARIANT(inserted, "List parent graph contains a cycle after library validation");

      auto const optParent = writer.get(current);

      if (!optParent && isDirectCandidateParent)
      {
        return makeError(Error::Code::InvalidInput, std::format("list parent not found: {}", current.raw()));
      }

      AO_INVARIANT(optParent, "List parent chain refers to missing List {} after library validation", current.raw());
      isDirectCandidateParent = false;
      current = optParent->parentId();

      if (targetId != kInvalidListId && current == targetId)
      {
        return makeError(Error::Code::InvalidInput, "list parent cannot be a descendant of the list");
      }
    }

    return {};
  }

  Result<ListId> ListWriter::create(ListBuilder const& list)
  {
    requireActiveOperation();

    auto preparedRes = list.prepare();

    if (!preparedRes)
    {
      return std::unexpected{preparedRes.error()};
    }

    auto const view = ListView{preparedRes->bytes()};
    AO_INVARIANT(view.isValid(), "Successful List preparation produced an invalid record");

    if (auto validationRes = validateParent(kInvalidListId, view.parentId()); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    return detail::WriteTransactionAccess::listStoreWriter(*_transaction).create(*preparedRes);
  }

  Result<> ListWriter::update(ListId const id, ListBuilder const& list)
  {
    requireActiveOperation();

    auto preparedRes = list.prepare();

    if (!preparedRes)
    {
      return std::unexpected{preparedRes.error()};
    }

    auto const view = ListView{preparedRes->bytes()};
    AO_INVARIANT(view.isValid(), "Successful List preparation produced an invalid record");
    auto& writer = detail::WriteTransactionAccess::listStoreWriter(*_transaction);

    if (!writer.get(id))
    {
      return makeError(Error::Code::NotFound, std::format("List {} does not exist", id.raw()));
    }

    if (auto validationRes = validateParent(id, view.parentId()); !validationRes)
    {
      return validationRes;
    }

    return writer.update(id, *preparedRes);
  }

  Result<> ListWriter::remove(ListId const id)
  {
    requireActiveOperation();

    auto& writer = detail::WriteTransactionAccess::listStoreWriter(*_transaction);

    if (!writer.get(id))
    {
      return makeError(Error::Code::NotFound, std::format("List {} does not exist", id.raw()));
    }

    for (auto const& [candidateId, view] : _transaction->listStore().reader(*_transaction))
    {
      if (candidateId != id && view.parentId() == id)
      {
        return makeError(
          Error::Code::Conflict, std::format("List {} still has child List {}", id.raw(), candidateId.raw()));
      }
    }

    auto const removed = writer.remove(id);
    AO_INVARIANT(removed, "Validated List disappeared during deletion");
    return {};
  }

  Result<std::vector<ListId>> ListWriter::removeSubtree(ListId const rootId)
  {
    requireActiveOperation();

    auto records = std::unordered_set<ListId>{};
    auto children = std::unordered_map<ListId, std::vector<ListId>>{};

    for (auto const& [id, view] : _transaction->listStore().reader(*_transaction))
    {
      records.insert(id);
      children[view.parentId()].push_back(id);
    }

    if (!records.contains(rootId))
    {
      return makeError(Error::Code::NotFound, std::format("List {} does not exist", rootId.raw()));
    }

    struct Visit final
    {
      ListId id;
      bool exiting;
    };

    auto rootFirst = std::vector<ListId>{};
    auto visiting = std::unordered_set<ListId>{};
    auto visited = std::unordered_set<ListId>{};
    auto pending = std::vector<Visit>{{.id = rootId, .exiting = false}};

    while (!pending.empty())
    {
      auto const [id, exiting] = pending.back();
      pending.pop_back();

      if (exiting)
      {
        visiting.erase(id);
        visited.insert(id);
        continue;
      }

      if (visited.contains(id))
      {
        continue;
      }

      auto const inserted = visiting.insert(id).second;
      AO_INVARIANT(inserted, "List parent graph contains a cycle after library validation");

      rootFirst.push_back(id);
      pending.push_back(Visit{.id = id, .exiting = true});

      if (auto const childIt = children.find(id); childIt != children.end())
      {
        for (auto const childId : childIt->second | std::views::reverse)
        {
          pending.push_back(Visit{.id = childId, .exiting = false});
        }
      }
    }

    auto& writer = detail::WriteTransactionAccess::listStoreWriter(*_transaction);

    for (auto const id : rootFirst | std::views::reverse)
    {
      auto const removed = writer.remove(id);
      AO_INVARIANT(removed, "Validated List subtree changed during deletion");
    }

    return rootFirst;
  }

  Result<> ListWriter::clear()
  {
    requireActiveOperation();
    return detail::WriteTransactionAccess::listStoreWriter(*_transaction).clear();
  }

  void ListWriter::requireActiveOperation() const
  {
    _transaction->requireOperationActive();
  }
} // namespace ao::library
