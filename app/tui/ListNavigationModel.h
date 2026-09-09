// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "ListSearch.h"
#include <ao/CoreIds.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

#include <ftxui/component/event.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ao::tui
{
  struct ListNavigationRow final
  {
    ListId id = kInvalidListId;
    ListId parentId = kInvalidListId;
    std::string name{};
    std::string detail{};
    std::string path{};
    std::size_t depth = 0;
    bool hasChildren = false;
    bool expanded = false;
    bool matching = false;
  };

  class ListNavigationModel final
  {
  public:
    void setTree(uimodel::ListTreeProjection tree, ListId activeId);
    void reveal(ListId id);
    void expandPath(ListId id);
    bool trySelect(ListId id);
    void selectVisibleRow(std::int32_t rowIndex);
    void move(std::int32_t delta);
    void expand();
    void collapse();
    void toggleExpanded(ListId id);
    bool trySearchEvent(ftxui::Event const& event);
    void clearSearch();

    ListSearch& search() noexcept { return _search; }
    ListSearch const& search() const noexcept { return _search; }
    std::vector<ListNavigationRow> const& rows() const noexcept { return _rows; }
    ListId cursor() const noexcept { return _cursor; }
    std::int32_t selectedIndex() const;
    std::uint64_t revision() const noexcept { return _revision; }
    std::optional<ListId> activationTarget() const;

  private:
    struct ProjectedNavigationRow final
    {
      ListNavigationRow row{};
      ListId traversalParentId = kInvalidListId;
      std::string nameKey{};
      std::string pathKey{};
    };

    bool isNavigable(ListId id) const;
    void expandAncestors(ListId id);
    void rebuildProjection();
    void rebuildRows();
    void reconcileSearchCursor();

    uimodel::ListTreeProjection _tree{};
    std::vector<ProjectedNavigationRow> _projection{};
    ListId _cursor = kInvalidListId;
    std::set<ListId> _expandedIds{};
    std::vector<ListNavigationRow> _rows{};
    ListSearch _search{};
    std::uint64_t _revision = 0;
  };
} // namespace ao::tui
