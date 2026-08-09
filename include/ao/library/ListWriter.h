// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListView.h>

#include <optional>
#include <vector>

namespace ao::library
{
  class ListStore;
  class WriteTransaction;

  /**
   * Transaction-scoped logical mutation port for saved List topology.
   * Every operation belongs inside the owning WriteTransaction::apply() root.
   * The writer borrows that transaction and must not outlive it.
   */
  class ListWriter final
  {
  public:
    ~ListWriter() = default;

    ListWriter(ListWriter const&) = delete;
    ListWriter& operator=(ListWriter const&) = delete;
    ListWriter(ListWriter&&) noexcept = default;
    ListWriter& operator=(ListWriter&&) noexcept = default;

    std::optional<ListView> get(ListId id) const;

    Result<ListId> create(ListBuilder const& list);
    Result<> update(ListId id, ListBuilder const& list);
    Result<> remove(ListId id);
    Result<std::vector<ListId>> removeSubtree(ListId rootId);
    Result<> clear();

  private:
    explicit ListWriter(WriteTransaction& transaction);
    void requireActiveOperation() const;
    Result<> validateParent(ListId targetId, ListId parentId) const;

    WriteTransaction* _transaction;

    friend class WriteTransaction;
  };
} // namespace ao::library
