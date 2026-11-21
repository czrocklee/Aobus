// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListTreeModelBuilder.h"

#include "list/ListNavigationSectionModel.h"
#include "list/ListRowObject.h"
#include "list/ListTreeItem.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

#include <giomm/listmodel.h>
#include <giomm/liststore.h>
#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/treelistmodel.h>

#include <map>
#include <memory>

namespace ao::gtk
{
  ListTreeModelBuilder::BuildResult ListTreeModelBuilder::build(rt::Library const& reads,
                                                                i18n::MessageCatalog const& textCatalog)
  {
    auto result = BuildResult{};
    result.storePtr = Gio::ListStore<ListTreeItem>::create();

    auto scope = reads.snapshot();
    auto const snapshot = scope.lists();
    result.projection = uimodel::buildListTreeProjection(textCatalog, snapshot);
    auto nodesById = std::map<ListId, Glib::RefPtr<ListTreeItem>>{};

    for (auto const& [id, row] : result.projection.rowsById)
    {
      auto listRowPtr = ListRowObject::create(id, row.isSystem, row.name, row.localExpression);
      auto treeNodePtr = ListTreeItem::create(listRowPtr);
      nodesById.emplace(id, treeNodePtr);
    }

    auto const nodeForId = [&nodesById](ListId const id) -> Glib::RefPtr<ListTreeItem> const&
    {
      auto const it = nodesById.find(id);
      AO_INVARIANT(it != nodesById.end(), "Missing list tree node for ID {}", id.raw());
      return it->second;
    };

    // buildListTreeProjection repairs parent links: every child ID names a row whose parentId matches this parent.
    for (auto const& [parentId, row] : result.projection.rowsById)
    {
      auto const& parentPtr = nodeForId(parentId);

      for (auto const childId : row.childIds)
      {
        parentPtr->children()->append(nodeForId(childId));
      }
    }

    for (auto const rootId : result.projection.rootIds)
    {
      result.storePtr->append(nodeForId(rootId));
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
