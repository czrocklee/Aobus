// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListTreeModelBuilder.h"

#include "ao/Type.h"
#include "ao/library/ListStore.h"
#include "ao/library/MusicLibrary.h"
#include "list/ListRowObject.h"
#include "list/ListTreeItem.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>

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

  ListTreeModelBuilder::Result ListTreeModelBuilder::build(rt::AppRuntime& runtime, lmdb::ReadTransaction const& txn)
  {
    auto result = Result{};
    result.store = Gio::ListStore<ListTreeItem>::create();

    auto const reader = runtime.musicLibrary().lists().reader(txn);
    auto nodes = std::map<ListId, StoredListNode>{};

    for (auto const& [id, listView] : reader)
    {
      nodes.emplace(id,
                    StoredListNode{.id = id,
                                   .parentId = listView.parentId(),
                                   .name = std::string{listView.name()},
                                   .isSmart = listView.isSmart(),
                                   .localExpression = std::string{listView.filter()}});
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
      auto listRow = ListRowObject::create(id, node.parentId, 0, node.isSmart, node.name, node.localExpression);
      auto treeNode = ListTreeItem::create(listRow);
      result.nodesById[id] = treeNode;
    }

    auto allRow = ListRowObject::create(rt::kAllTracksListId, kInvalidListId, 0, false, "All Tracks");
    auto allTracksNode = ListTreeItem::create(allRow);
    result.nodesById[rt::kAllTracksListId] = allTracksNode;

    for (auto const& [id, node] : nodes)
    {
      auto childNode = result.nodesById[id];
      auto parentId = node.parentId;

      if (auto parentIt = result.nodesById.find(parentId); parentIt != result.nodesById.end())
      {
        parentIt->second->children()->append(childNode);
        childNode->setParent(parentIt->second.get());
      }
      else
      {
        allTracksNode->children()->append(childNode);
        childNode->setParent(allTracksNode.get());
      }
    }

    result.store->append(allTracksNode);

    result.treeModel = Gtk::TreeListModel::create(
      result.store,
      [](Glib::RefPtr<Glib::ObjectBase> const& item) -> Glib::RefPtr<Gio::ListModel>
      {
        auto node = std::dynamic_pointer_cast<ListTreeItem>(item);

        if (!node || !node->hasChildren())
        {
          return nullptr;
        }

        return node->children();
      },
      false,
      true);

    result.selectionModel = Gtk::SingleSelection::create(result.treeModel);
    return result;
  }
} // namespace ao::gtk
