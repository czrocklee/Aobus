// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ListSidebarController.h"
#include "LayoutConstants.h"
#include "ListRow.h"
#include "ListTreeNode.h"
#include "SmartListDialog.h"
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/utility/Log.h>
#include <format>
#include <limits>

namespace ao::gtk
{
  namespace
  {
    ao::ListId allTracksListId()
    {
      return ao::ListId{std::numeric_limits<std::uint32_t>::max()};
    }

    ao::ListId rootParentId()
    {
      return ao::ListId{0};
    }

    struct StoredListNode final
    {
      ao::ListId id = ao::ListId{0};
      ao::ListId parentId = rootParentId();
      std::string name;
      bool isSmart = false;
      std::string localExpression;
    };
  }

  ListSidebarController::ListSidebarController(Gtk::Window& parent, Callbacks callbacks)
    : _parent{parent}, _callbacks{std::move(callbacks)}
  {
    setupLayout();
  }

  ListSidebarController::~ListSidebarController() = default;

  void ListSidebarController::setupLayout()
  {
    // Scrolled window for sidebar list
    _listScrolledWindow.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _listScrolledWindow.set_child(_listView);
    _listScrolledWindow.set_size_request(kSidebarWidth, -1);

    // List view setup
    auto factory = Gtk::SignalListItemFactory::create();
    factory->signal_setup().connect([this](Glib::RefPtr<Gtk::ListItem> const& listItem)
                                    { setupSidebarListItem(listItem); });
    factory->signal_bind().connect([this](Glib::RefPtr<Gtk::ListItem> const& listItem)
                                   { bindSidebarListItem(listItem); });

    _listView.set_factory(factory);

    // Setup context menu
    auto menuModel = Gio::Menu::create();
    menuModel->append("New Smart List...", "win.new");
    menuModel->append("Edit List...", "win.edit");
    menuModel->append("Delete List", "win.delete");
    _listContextMenu.set_menu_model(menuModel);
    _listContextMenu.set_parent(_listView);

    // Actions
    _newListAction = Gio::SimpleAction::create("new");
    _newListAction->signal_activate().connect([this](Glib::VariantBase const& /*variant*/)
                                              { openNewSmartListDialog(); });
    _newListAction->set_enabled(false);

    _deleteListAction = Gio::SimpleAction::create("delete");
    _deleteListAction->signal_activate().connect([this](Glib::VariantBase const& /*variant*/) { onDeleteList(); });
    _deleteListAction->set_enabled(false);

    _editListAction = Gio::SimpleAction::create("edit");
    _editListAction->signal_activate().connect([this](Glib::VariantBase const& /*variant*/) { onEditList(); });
    _editListAction->set_enabled(false);
  }

  void ListSidebarController::addActionsTo(Gio::ActionMap& actionMap)
  {
    actionMap.add_action(_newListAction);
    actionMap.add_action(_deleteListAction);
    actionMap.add_action(_editListAction);
  }

  void ListSidebarController::rebuildTree(LibrarySession& session, ao::lmdb::ReadTransaction& txn)
  {
    _currentSession = &session;

    buildListTree(txn);
  }

  void ListSidebarController::select(ao::ListId listId)
  {
    selectSidebarList(listId);
  }

  void ListSidebarController::onListSelectionChanged(std::uint32_t /*position*/, std::uint32_t /*nItems*/)
  {
    if (_listSelectionModel == nullptr)
    {
      return;
    }

    auto const selectedPosition = _listSelectionModel->get_selected();

    if (selectedPosition == GTK_INVALID_LIST_POSITION)
    {
      return;
    }

    auto const treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(_listSelectionModel->get_selected_item());

    if (treeListRow == nullptr)
    {
      return;
    }

    auto const node = std::dynamic_pointer_cast<ListTreeNode>(treeListRow->get_item());

    if (node == nullptr)
    {
      return;
    }

    auto const listId = node->getListId();

    _newListAction->set_enabled(true);
    _deleteListAction->set_enabled(listId != allTracksListId());
    _editListAction->set_enabled(listId != allTracksListId());

    if (_callbacks.onListSelected)
    {
      _callbacks.onListSelected(listId);
    }
  }

  void ListSidebarController::openNewListDialog(ao::ListId parentListId, std::string initialExpression)
  {
    if (_currentSession == nullptr)
    {
      return;
    }

    // Determine the parent membership list
    ao::model::TrackIdList* parentMembershipList = nullptr;

    if (parentListId == allTracksListId())
    {
      // Use All Tracks as source
      parentMembershipList = _currentSession->allTrackIds.get();
    }
    else
    {
      // Find the parent's membership list from track pages
      if (auto* parentList = _callbacks.getListMembership ? _callbacks.getListMembership(parentListId) : nullptr;
          parentList)
      {
        parentMembershipList = parentList;
      }
      else
      {
        // Fallback to All Tracks if parent not found
        parentMembershipList = _currentSession->allTrackIds.get();
      }
    }

    auto* dialog = Gtk::make_managed<SmartListDialog>(_parent,
                                                      *_currentSession->musicLibrary,
                                                      *_currentSession->allTrackIds,
                                                      *parentMembershipList,
                                                      parentListId,
                                                      *_currentSession->rowDataProvider);

    if (!initialExpression.empty())
    {
      dialog->setLocalExpression(std::move(initialExpression));
    }

    dialog->signal_response().connect(
      [this, dialog](int responseId)
      {
        if (responseId == Gtk::ResponseType::OK)
        {
          if (auto const draft = dialog->draft(); draft.listId != ao::ListId{0})
          {
            updateList(draft);
          }
          else
          {
            createList(draft);
          }
        }

        dialog->close();
      });

    dialog->present();
  }

  void ListSidebarController::createSmartListFromExpression(ao::ListId parentListId, std::string expression)
  {
    openNewListDialog(parentListId, std::move(expression));
  }

  void ListSidebarController::openNewSmartListDialog()
  {
    // Smart selection: if a non-All-Tracks list is selected, use it as parent; otherwise use root
    auto parentListId = rootParentId();

    if (auto const selected = _listSelectionModel ? _listSelectionModel->get_selected() : GTK_INVALID_LIST_POSITION;
        _treeListModel && selected != GTK_INVALID_LIST_POSITION && selected != 0)
    {
      if (auto model = _listSelectionModel->get_model())
      {
        if (auto item = model->get_object(selected))
        {
          if (auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(item); treeListRow != nullptr)
          {
            if (auto node = std::dynamic_pointer_cast<ListTreeNode>(treeListRow->get_item()); node != nullptr)
            {
              parentListId = node->getListId();
            }
          }
        }
      }
    }

    openNewListDialog(parentListId);
  }

  bool ListSidebarController::listHasChildren(ao::ListId listId) const
  {
    auto it = _nodesById.find(listId);

    if (it == _nodesById.end())
    {
      return false;
    }

    return it->second->hasChildren();
  }

  void ListSidebarController::setupSidebarListItem(Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto* rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    rowBox->set_halign(Gtk::Align::FILL);
    rowBox->set_hexpand(true);
    rowBox->set_margin_start(Layout::kMarginMedium);
    rowBox->set_margin_end(Layout::kMarginMedium);
    rowBox->set_margin_top(Layout::kMarginSmall);
    rowBox->set_margin_bottom(Layout::kMarginSmall);

    auto* expander = Gtk::make_managed<Gtk::TreeExpander>();
    rowBox->append(*expander);

    auto* label = Gtk::make_managed<Gtk::Label>("");
    label->set_halign(Gtk::Align::START);
    rowBox->append(*label);

    auto* filterLabel = Gtk::make_managed<Gtk::Label>("");
    filterLabel->set_halign(Gtk::Align::START);
    filterLabel->add_css_class("dim-label");
    filterLabel->set_margin_start(Layout::kMarginMedium);
    filterLabel->set_ellipsize(Pango::EllipsizeMode::END);
    filterLabel->set_hexpand(true);
    rowBox->append(*filterLabel);

    auto clickController = Gtk::GestureClick::create();
    clickController->set_button(GDK_BUTTON_SECONDARY);
    clickController->signal_pressed().connect(
      [this, listItem, rowBox](int /*nPress*/, double xPos, double yPos)
      {
        if (auto const position = listItem->get_position(); position != GTK_INVALID_LIST_POSITION)
        {
          _listSelectionModel->set_selected(position);
        }

        auto point =
          rowBox->compute_point(_listView, Gdk::Graphene::Point(static_cast<float>(xPos), static_cast<float>(yPos)));

        if (!point)
        {
          return;
        }

        auto rect = Gdk::Rectangle(static_cast<int>(point->get_x()), static_cast<int>(point->get_y()), 1, 1);
        showListContextMenu(_listView, rect);
      });

    rowBox->add_controller(clickController);
    listItem->set_child(*rowBox);
  }

  void ListSidebarController::bindSidebarListItem(Glib::RefPtr<Gtk::ListItem> const& listItem)
  {
    auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(listItem->get_item());
    if (!treeListRow)
    {
      return;
    }

    auto node = std::dynamic_pointer_cast<ListTreeNode>(treeListRow->get_item());
    if (!node)
    {
      return;
    }

    auto* box = dynamic_cast<Gtk::Box*>(listItem->get_child());
    auto* expander = box != nullptr ? dynamic_cast<Gtk::TreeExpander*>(box->get_first_child()) : nullptr;
    auto* label = expander != nullptr ? dynamic_cast<Gtk::Label*>(expander->get_next_sibling()) : nullptr;
    auto* filterLabel = label != nullptr ? dynamic_cast<Gtk::Label*>(label->get_next_sibling()) : nullptr;

    if (expander != nullptr)
    {
      expander->set_list_row(treeListRow);
    }

    if (label == nullptr)
    {
      return;
    }

    auto row = node->getRow();
    if (!row)
    {
      return;
    }

    label->set_text(row->getName());

    if (filterLabel == nullptr)
    {
      return;
    }

    auto const filter = row->getFilter();
    if (!filter.empty())
    {
      filterLabel->set_text("[" + filter + "]");
      filterLabel->set_visible(true);
    }
    else
    {
      filterLabel->set_text("");
      filterLabel->set_visible(false);
    }
  }

  void ListSidebarController::showListContextMenu(Gtk::ListView& /*listView*/, Gdk::Rectangle const& rect)
  {
    auto const hasLibrary = static_cast<bool>(_currentSession);

    auto canDelete = false;
    auto canEdit = false;

    if (auto const selected = _listSelectionModel ? _listSelectionModel->get_selected() : GTK_INVALID_LIST_POSITION;
        hasLibrary && _treeListModel && selected != GTK_INVALID_LIST_POSITION && selected != 0)
    {
      if (auto item = _listSelectionModel->get_selected_item())
      {
        if (auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(item))
        {
          auto const treeListItem = treeListRow->get_item();

          if (auto node = std::dynamic_pointer_cast<ListTreeNode>(treeListItem))
          {
            canDelete = !listHasChildren(node->getListId());
            canEdit = true;
          }
        }
      }
    }

    if (_newListAction)
    {
      _newListAction->set_enabled(hasLibrary);
    }

    if (_deleteListAction)
    {
      _deleteListAction->set_enabled(canDelete);
    }

    if (_editListAction)
    {
      _editListAction->set_enabled(canEdit);
    }

    _listContextMenu.set_pointing_to(rect);
    _listContextMenu.popup();
  }

  void ListSidebarController::createList(ao::model::ListDraft const& draft)
  {
    if (_currentSession == nullptr)
    {
      APP_LOG_ERROR("No music library open");
      return;
    }

    auto txn = _currentSession->musicLibrary->writeTransaction();

    // Build the list payload
    auto builder =
      ao::library::ListBuilder::createNew().name(draft.name).description(draft.description).parentId(draft.parentId);

    if (draft.kind == ao::model::ListKind::Smart)
    {
      builder.filter(draft.expression);
    }
    else
    {
      for (auto id : draft.trackIds)
      {
        builder.tracks().add(id);
      }
    }

    auto payload = builder.serialize();

    // Create the list in the store
    auto [listId, view] = _currentSession->musicLibrary->lists().writer(txn).create(payload);

    txn.commit();

    // Refresh the lists
    if (_callbacks.onListCreatedAndSelected)
    {
      _callbacks.onListCreatedAndSelected(listId);
    }
  }

  void ListSidebarController::selectSidebarList(ao::ListId listId)
  {
    if (!_treeListModel)
    {
      return;
    }

    auto const itemCount = _treeListModel->get_n_items();

    for (guint index = 0; index < itemCount; ++index)
    {
      auto item = _treeListModel->get_object(index);
      if (!item)
      {
        continue;
      }

      auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(item);
      if (!treeListRow)
      {
        continue;
      }

      auto node = std::dynamic_pointer_cast<ListTreeNode>(treeListRow->get_item());
      if (!node)
      {
        continue;
      }

      if (node->getListId() == listId)
      {
        _listSelectionModel->set_selected(index);
        break;
      }
    }
  }

  void ListSidebarController::updateList(ao::model::ListDraft const& draft)
  {
    if (_currentSession == nullptr)
    {
      APP_LOG_ERROR("No music library open");
      return;
    }

    auto txn = _currentSession->musicLibrary->writeTransaction();

    auto builder =
      ao::library::ListBuilder::createNew().name(draft.name).description(draft.description).parentId(draft.parentId);

    if (draft.kind == ao::model::ListKind::Smart)
    {
      builder.filter(draft.expression);
    }
    else
    {
      for (auto id : draft.trackIds)
      {
        builder.tracks().add(id);
      }
    }

    auto payload = builder.serialize();

    _currentSession->musicLibrary->lists().writer(txn).update(draft.listId, payload);

    txn.commit();

    // Refresh the list page
    if (_callbacks.onListsChanged)
    {
      _callbacks.onListsChanged();
      selectSidebarList(draft.listId);
    }
  }

  void ListSidebarController::onEditList()
  {
    if (_currentSession == nullptr)
    {
      return;
    }

    auto const position = _listSelectionModel->get_selected();

    if (position == GTK_INVALID_LIST_POSITION)
    {
      return;
    }

    // Don't allow editing "All Tracks" (position 0)

    if (position == 0)
    {
      return;
    }

    auto item = _listSelectionModel->get_selected_item();

    if (!item)
    {
      return;
    }

    auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(item);

    if (!treeListRow)
    {
      return;
    }

    auto node = std::dynamic_pointer_cast<ListTreeNode>(treeListRow->get_item());

    if (!node)
    {
      return;
    }

    openEditListDialog(node->getListId());
  }

  void ListSidebarController::openEditListDialog(ao::ListId listId)
  {
    if (_currentSession == nullptr)
    {
      return;
    }

    auto readTxn = _currentSession->musicLibrary->readTransaction();
    auto reader = _currentSession->musicLibrary->lists().reader(readTxn);
    auto view = reader.get(listId);

    if (!view)
    {
      return;
    }

    // Determine the parent membership list for the preview
    ao::model::TrackIdList* parentMembershipList = nullptr;
    auto const parentId = view->parentId();

    if (parentId == allTracksListId())
    {
      parentMembershipList = _currentSession->allTrackIds.get();
    }
    else
    {
      if (auto* parentList = _callbacks.getListMembership ? _callbacks.getListMembership(parentId) : nullptr;
          parentList)
      {
        parentMembershipList = parentList;
      }
      else
      {
        parentMembershipList = _currentSession->allTrackIds.get();
      }
    }

    auto* dialog = Gtk::make_managed<SmartListDialog>(_parent,
                                                      *_currentSession->musicLibrary,
                                                      *_currentSession->allTrackIds,
                                                      *parentMembershipList,
                                                      view->parentId(),
                                                      *_currentSession->rowDataProvider);

    dialog->populate(listId, *view);

    dialog->signal_response().connect(
      [this, dialog](int responseId)
      {
        if (responseId == Gtk::ResponseType::OK)
        {
          auto const draft = dialog->draft();

          if (draft.listId != ao::ListId{0})
          {
            updateList(draft);
          }
        }

        dialog->close();
      });

    dialog->present();
  }

  void ListSidebarController::onDeleteList()
  {
    if (_currentSession == nullptr)
    {
      return;
    }

    auto const position = _listSelectionModel->get_selected();

    if (position == GTK_INVALID_LIST_POSITION)
    {
      return;
    }

    if (position == 0)
    {
      // Don't allow deleting "All Tracks" (position 0)
      return;
    }

    auto item = _listSelectionModel->get_selected_item();

    if (!item)
    {
      return;
    }

    auto treeListRow = std::dynamic_pointer_cast<Gtk::TreeListRow>(item);

    if (!treeListRow)
    {
      return;
    }

    auto node = std::dynamic_pointer_cast<ListTreeNode>(treeListRow->get_item());

    if (!node)
    {
      return;
    }

    auto listId = node->getListId();

    if (listHasChildren(listId))
    {
      APP_LOG_ERROR("Cannot delete a list that still has child lists");
      return;
    }

    // Delete the list from the library
    auto txn = _currentSession->musicLibrary->writeTransaction();
    _currentSession->musicLibrary->lists().writer(txn).del(listId);
    txn.commit();

    // Refresh lists
    if (_callbacks.onListsChanged)
    {
      _callbacks.onListsChanged();
      selectSidebarList(allTracksListId());
    }
  }

  void ListSidebarController::buildListTree(ao::lmdb::ReadTransaction& txn)
  {
    // Clear existing tree store and lookup map
    _nodesById.clear();

    // Create new tree store
    _listTreeStore = Gio::ListStore<ListTreeNode>::create();

    auto reader = _currentSession->musicLibrary->lists().reader(txn);
    auto nodes = std::map<ao::ListId, StoredListNode>{};

    for (auto const& [id, listView] : reader)
    {
      nodes.emplace(id,
                    StoredListNode{
                      .id = id,
                      .parentId = listView.parentId(),
                      .name = std::string(listView.name()),
                      .isSmart = listView.isSmart(),
                      .localExpression = std::string(listView.filter()),
                    });
    }

    // Build children map
    auto children = std::map<ao::ListId, std::vector<ao::ListId>>{};

    for (auto const& [id, node] : nodes)
    {
      if (node.parentId != rootParentId() && node.parentId != id && nodes.contains(node.parentId))
      {
        children[node.parentId].push_back(id);
      }
    }

    // Create tree nodes for all stored lists
    for (auto const& [id, node] : nodes)
    {
      auto listRow = ListRow::create(id, node.parentId, 0, node.isSmart, node.name, node.localExpression);
      auto treeNode = ListTreeNode::create(listRow);
      _nodesById[id] = treeNode;
    }

    // Create "All Tracks" root node
    auto allRow = ListRow::create(allTracksListId(), rootParentId(), 0, false, "All Tracks");
    auto allTracksNode = ListTreeNode::create(allRow);
    _nodesById[allTracksListId()] = allTracksNode;

    // Attach children to parents
    for (auto const& [id, node] : nodes)
    {
      auto childNode = _nodesById[id];
      auto parentId = node.parentId;

      if (auto parentNodeIt = _nodesById.find(parentId); parentNodeIt != _nodesById.end())
      {
        parentNodeIt->second->getChildren()->append(childNode);
        childNode->setParent(parentNodeIt->second.get());
      }
      else
      {
        // Parent not found, attach to All Tracks root
        allTracksNode->getChildren()->append(childNode);
        childNode->setParent(allTracksNode.get());
      }
    }

    // Add allTracksNode to the tree store
    _listTreeStore->append(allTracksNode);

    // Create Gtk::TreeListModel wrapping the store
    // Return nullptr for leaf nodes so GTK doesn't show an expander
    _treeListModel = Gtk::TreeListModel::create(
      _listTreeStore,
      [](Glib::RefPtr<Glib::ObjectBase> const& item) -> Glib::RefPtr<Gio::ListModel>
      {
        auto node = std::dynamic_pointer_cast<ListTreeNode>(item);

        if (!node || !node->hasChildren())
        {
          return nullptr;
        }

        return node->getChildren();
      },
      false,
      true);

    _listSelectionModel = Gtk::SingleSelection::create(_treeListModel);
    _listSelectionModel->signal_selection_changed().connect(
      sigc::mem_fun(*this, &ListSidebarController::onListSelectionChanged));
    _listView.set_model(_listSelectionModel);
  }
} // namespace ao::gtk
