// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListTreeModelBuilder.h"
#include "list/ListRowObject.h"
#include "list/ListTreeItem.h"
#include <ao/Type.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <runtime/AppSession.h>

#include <giomm/listmodel.h>
#include <giomm/liststore.h>
#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/treelistmodel.h>

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    ListId allTracksListId()
    {
      return ListId{std::numeric_limits<std::uint32_t>::max()};
    }

    ListId rootParentId()
    {
      return ListId{0};
    }

    struct StoredListNode final
    {
      ListId id = ListId{0};
      ListId parentId = rootParentId();
      std::string name;
      bool isSmart = false;
      std::string localExpression;
    };
  }

  ListTreeModelBuilder::Result ListTreeModelBuilder::build(rt::AppSession& session, lmdb::ReadTransaction const& txn)
  {
    auto result = Result{};
    result.store = Gio::ListStore<ListTreeItem>::create();

    auto const reader = session.musicLibrary().lists().reader(txn);
    auto nodes = std::map<ListId, StoredListNode>{};

    for (auto const& [id, listView] : reader)
    {
      nodes.emplace(id,
                    StoredListNode{.id = id,
                                   .parentId = listView.parentId(),
                                   .name = std::string(listView.name()),
                                   .isSmart = listView.isSmart(),
                                   .localExpression = std::string(listView.filter())});
    }

    auto children = std::map<ListId, std::vector<ListId>>{};

    for (auto const& [id, node] : nodes)
    {
      if (node.parentId != rootParentId() && node.parentId != id && nodes.contains(node.parentId))
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

    auto allRow = ListRowObject::create(allTracksListId(), rootParentId(), 0, false, "All Tracks");
    auto allTracksNode = ListTreeItem::create(allRow);
    result.nodesById[allTracksListId()] = allTracksNode;

    for (auto const& [id, node] : nodes)
    {
      auto childNode = result.nodesById[id];
      auto parentId = node.parentId;

      if (auto parentIt = result.nodesById.find(parentId); parentIt != result.nodesById.end())
      {
        parentIt->second->getChildren()->append(childNode);
        childNode->setParent(parentIt->second.get());
      }
      else
      {
        allTracksNode->getChildren()->append(childNode);
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

        return node->getChildren();
      },
      false,
      true);

    result.selectionModel = Gtk::SingleSelection::create(result.treeModel);
    return result;
  }
} // namespace ao::gtk
