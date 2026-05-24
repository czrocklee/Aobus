// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/document/LayoutDocument.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"

#include <giomm/simpleactiongroup.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/paned.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/treemodel.h>
#include <gtkmm/treemodelcolumn.h>
#include <gtkmm/treestore.h>
#include <gtkmm/treeview.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::gtk::layout::editor
{
  class LayoutEditorDialog final : public Gtk::Dialog
  {
  public:
    LayoutEditorDialog(Gtk::Window& parent, ComponentRegistry const& registry, LayoutDocument initialDoc);
    ~LayoutEditorDialog() override;

    LayoutEditorDialog(LayoutEditorDialog const&) = delete;
    LayoutEditorDialog& operator=(LayoutEditorDialog const&) = delete;
    LayoutEditorDialog(LayoutEditorDialog&&) = delete;
    LayoutEditorDialog& operator=(LayoutEditorDialog&&) = delete;

    LayoutDocument const& document() const { return _document; }

    sigc::signal<void(LayoutDocument const&)>& signalApplyPreview() { return _signalApplyPreview; }

    void updateNodePosition(std::string_view nodeId, std::int32_t posX, std::int32_t posY);

  private:
    // Tree Model columns
    struct ModelColumns : Gtk::TreeModel::ColumnRecord
    {
      ModelColumns()
      {
        add(displayName);
        add(type);
        add(nodePtr);
      }

      Gtk::TreeModelColumn<Glib::ustring> displayName;
      Gtk::TreeModelColumn<Glib::ustring> type;
      Gtk::TreeModelColumn<LayoutNode*> nodePtr;
    };

    void setupUi();
    void populateTree();
    void appendNodeToTree(Gtk::TreeModel::Row parentRow, LayoutNode* node);
    void onSelectionChanged();
    void updatePropertiesPanel(LayoutNode* node);
    void applyPropertyChange(LayoutNode* node, std::string_view propName, LayoutValue const& value, bool isLayoutProp);
    void notifyPreview();

    void renderIdSection(LayoutNode* node);
    void addSectionTitle(std::string_view text);
    void renderBoolEditor(LayoutNode* node,
                          PropertyDescriptor const& prop,
                          LayoutValue const& currentVal,
                          bool isLayoutProp);
    void renderIntEditor(LayoutNode* node,
                         PropertyDescriptor const& prop,
                         LayoutValue const& currentVal,
                         bool isLayoutProp);
    void renderEnumEditor(LayoutNode* node,
                          PropertyDescriptor const& prop,
                          LayoutValue const& currentVal,
                          bool isLayoutProp);
    void renderStringEditor(LayoutNode* node,
                            PropertyDescriptor const& prop,
                            LayoutValue const& currentVal,
                            bool isLayoutProp);
    void dispatchEditor(LayoutNode* node, PropertyDescriptor const& prop, bool isLayoutProp);

    void onAddChild();
    void addComponent(std::string type);
    void wrapNode(std::string containerType);

    void onRemoveNode();
    void onMoveUp();
    void onMoveDown();
    void onRaiseZ();
    void onLowerZ();
    void onResetDefault();

    LayoutNode* findParentOf(LayoutNode* root, LayoutNode* target);

    ComponentRegistry const& _registry;
    LayoutDocument _document;

    ModelColumns _columns;
    Glib::RefPtr<Gtk::TreeStore> _treeStore;
    Gtk::TreeView _treeView;
    Gtk::ScrolledWindow _treeScroll;

    Gtk::Box _treeBox{Gtk::Orientation::VERTICAL};
    Gtk::Box _toolbar{Gtk::Orientation::HORIZONTAL};
    Gtk::MenuButton _btnAdd;
    Gtk::MenuButton _btnWrap;
    Gtk::Button _btnRemove{"Remove"};
    Gtk::Button _btnUp{"Up"};
    Gtk::Button _btnDown{"Down"};
    Gtk::Button _btnRaiseZ{"Raise Z"};
    Gtk::Button _btnLowerZ{"Lower Z"};
    Gtk::Button _btnReset{"Reset Default"};

    Gtk::PopoverMenu _addPopover;
    Gtk::PopoverMenu _wrapPopover;

    Glib::RefPtr<Gio::SimpleActionGroup> _actionGroup;

    Gtk::Box _propertiesBox{Gtk::Orientation::VERTICAL};
    Gtk::ScrolledWindow _propertiesScroll;

    Gtk::Paned _paned{Gtk::Orientation::HORIZONTAL};

    sigc::signal<void(LayoutDocument const&)> _signalApplyPreview;
  };
} // namespace ao::gtk::layout::editor
