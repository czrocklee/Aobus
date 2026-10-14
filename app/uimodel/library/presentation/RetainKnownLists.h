// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/VirtualListIds.h>

#include <algorithm>
#include <map>
#include <span>

namespace ao::uimodel
{
  /**
   * Drops every entry keyed by a list the library no longer has.
   *
   * A virtual id references no user-created list, so it is kept without
   * appearing in @p knownListIds. Both presentation stores share this rule:
   * their restore paths install state that outlived the process, and neither
   * receives a LibraryChanges deletion for a list removed while it was down.
   */
  template<typename Value>
  void retainKnownLists(std::map<ListId, Value>& entries, std::span<ListId const> const knownListIds)
  {
    std::erase_if(entries,
                  [knownListIds](auto const& entry)
                  { return !rt::isVirtualListId(entry.first) && !std::ranges::contains(knownListIds, entry.first); });
  }

  /**
   * Drops every entry keyed by a user-created list.
   *
   * Both presentation schemas apply this to a document saved for another
   * library, whose list ids name unrelated lists here; a virtual id names the
   * same view in every library.
   */
  template<typename Value>
  void retainVirtualLists(std::map<ListId, Value>& entries)
  {
    std::erase_if(entries, [](auto const& entry) { return !rt::isVirtualListId(entry.first); });
  }
} // namespace ao::uimodel
