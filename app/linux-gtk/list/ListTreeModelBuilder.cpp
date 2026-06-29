// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListTreeModelBuilder.h"

#include "list/ListRowObject.h"
#include "list/ListTreeItem.h"
#include <ao/CoreIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

#include <giomm/listmodel.h>
#include <giomm/liststore.h>
#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/treelistmodel.h>

#include <memory>

namespace ao::gtk
{
  ListTreeModelBuilder::Result ListTreeModelBuilder::build(rt::Library const& reads)
  {
    auto result = Result{};
    result.storePtr = Gio::ListStore<ListTreeItem>::create();

    auto scope = reads.reader();
    auto const snapshot = scope.lists();
    auto const projection = uimodel::buildListTreeProjection(snapshot);

    for (auto const& [id, row] : projection.rowsById)
    {
      auto listRowPtr = ListRowObject::create(id, row.parentId, 0, row.isSmart, row.name, row.localExpression);
      auto treeNodePtr = ListTreeItem::create(listRowPtr);
      result.nodesById[id] = treeNodePtr;
    }

    for (auto const& [parentId, row] : projection.rowsById)
    {
      auto const parentIt = result.nodesById.find(parentId);

      if (parentIt == result.nodesById.end())
      {
        continue;
      }

      for (auto const childId : row.childIds)
      {
        if (auto const childIt = result.nodesById.find(childId); childIt != result.nodesById.end())
        {
          parentIt->second->children()->append(childIt->second);
          childIt->second->setParent(parentIt->second.get());
        }
      }
    }

    for (auto const rootId : projection.rootIds)
    {
      if (auto const rootIt = result.nodesById.find(rootId); rootIt != result.nodesById.end())
      {
        result.storePtr->append(rootIt->second);
      }
    }

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
