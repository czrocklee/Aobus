// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListTreeModelBuilder.h"

#include "list/ListRowObject.h"
#include "list/ListTreeItem.h"
#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>

#include <giomm/listmodel.h>
#include <giomm/liststore.h>
#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/treelistmodel.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    struct StoredListNode final
    {
      ListId id = kInvalidListId;
      ListId parentId = kInvalidListId;
      std::string name;
      bool isSmart = false;
      std::string localExpression;
    };
  }

  ListTreeModelBuilder::Result ListTreeModelBuilder::build(rt::Library const& reads)
  {
    auto result = Result{};
    result.storePtr = Gio::ListStore<ListTreeItem>::create();

    auto scope = reads.reader();
    auto const snapshot = scope.lists();
    auto nodes = std::map<ListId, StoredListNode>{};

    for (auto const& node : snapshot)
    {
      nodes.emplace(node.id,
                    StoredListNode{.id = node.id,
                                   .parentId = node.parentId,
                                   .name = node.name,
                                   .isSmart = node.kind == rt::ListNodeKind::Smart,
                                   .localExpression = node.smartExpression});
    }

    auto children = std::map<ListId, std::vector<ListId>>{};

    for (auto const& [id, node] : nodes)
    {
      if (node.parentId != kInvalidListId && node.parentId != id && nodes.contains(node.parentId))
      {
        children[node.parentId].push_back(id);
      }
    }

    for (auto const& [id, node] : nodes)
    {
      auto listRowPtr = ListRowObject::create(id, node.parentId, 0, node.isSmart, node.name, node.localExpression);
      auto treeNodePtr = ListTreeItem::create(listRowPtr);
      result.nodesById[id] = treeNodePtr;
    }

    auto allRowPtr = ListRowObject::create(rt::kAllTracksListId, kInvalidListId, 0, false, "All Tracks");
    auto allTracksNodePtr = ListTreeItem::create(allRowPtr);
    result.nodesById[rt::kAllTracksListId] = allTracksNodePtr;

    for (auto const& [id, node] : nodes)
    {
      auto childNodePtr = result.nodesById[id];
      auto parentId = node.parentId;

      if (auto parentIt = result.nodesById.find(parentId); parentIt != result.nodesById.end())
      {
        parentIt->second->children()->append(childNodePtr);
        childNodePtr->setParent(parentIt->second.get());
      }
      else
      {
        allTracksNodePtr->children()->append(childNodePtr);
        childNodePtr->setParent(allTracksNodePtr.get());
      }
    }

    result.storePtr->append(allTracksNodePtr);

    result.treeModelPtr = Gtk::TreeListModel::create(
      result.storePtr,
      [](Glib::RefPtr<Glib::ObjectBase> const& item) -> Glib::RefPtr<Gio::ListModel>
      {
        auto nodePtr = std::dynamic_pointer_cast<ListTreeItem>(item);

        if (!nodePtr || !nodePtr->hasChildren())
        {
          return nullptr;
        }

        return nodePtr->children();
      },
      false,
      true);

    result.selectionModelPtr = Gtk::SingleSelection::create(result.treeModelPtr);
    return result;
  }
} // namespace ao::gtk
