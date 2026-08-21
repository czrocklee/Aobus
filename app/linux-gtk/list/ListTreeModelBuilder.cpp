// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListTreeModelBuilder.h"

#include "list/ListNavigationSectionModel.h"
#include "list/ListRowObject.h"
#include "list/ListTreeItem.h"
#include <ao/CoreIds.h>
#include <ao/rt/VirtualListIds.h>
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
  ListTreeModelBuilder::BuildResult ListTreeModelBuilder::build(rt::Library const& reads,
                                                                uimodel::PresentationTextCatalog const& textCatalog)
  {
    auto result = BuildResult{};
    result.storePtr = Gio::ListStore<ListTreeItem>::create();

    auto scope = reads.reader();
    auto const snapshot = scope.lists();
    auto const projection = uimodel::buildListTreeProjection(textCatalog, snapshot);

    for (auto const& [id, row] : projection.rowsById)
    {
      auto listRowPtr = ListRowObject::create(id, row.isSystem, row.name, row.localExpression);
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
        }
      }
    }

    if (auto const allTracksIt = result.nodesById.find(rt::kAllTracksListId); allTracksIt != result.nodesById.end())
    {
      result.storePtr->append(allTracksIt->second);
    }

    for (auto const rootId : projection.rootIds)
    {
      if (rootId != rt::kAllTracksListId)
      {
        if (auto const rootIt = result.nodesById.find(rootId); rootIt != result.nodesById.end())
        {
          result.storePtr->append(rootIt->second);
        }
      }
    }

    result.treeModelPtr = Gtk::TreeListModel::create(
      result.storePtr,
      [](Glib::RefPtr<Glib::ObjectBase> const& itemPtr) -> Glib::RefPtr<Gio::ListModel>
      {
        auto nodePtr = std::dynamic_pointer_cast<ListTreeItem>(itemPtr);

        if (!nodePtr || !nodePtr->hasChildren())
        {
          return nullptr;
        }

        return nodePtr->children();
      },
      false,
      true);

    result.sectionModelPtr = ListNavigationSectionModel::create(result.treeModelPtr);
    result.selectionModelPtr = Gtk::SingleSelection::create(result.sectionModelPtr);
    return result;
  }
} // namespace ao::gtk
