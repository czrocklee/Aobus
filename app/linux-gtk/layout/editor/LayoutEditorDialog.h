// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/document/LayoutDocument.h"
#include "layout/runtime/ComponentRegistry.h"

#include <gtkmm.h>
#include <map>
#include <memory>
#include <vector>

namespace ao::gtk::layout::editor
{
  class LayoutEditorDialog final : public Gtk::Dialog
  {
  public:
    LayoutEditorDialog(Gtk::Window& parent, ComponentRegistry const& registry, LayoutDocument const& initialDoc);
    ~LayoutEditorDialog() override;

    LayoutDocument const& document() const { return _document; }

    sigc::signal<void(LayoutDocument const&)>& signalApplyPreview() { return _signalApplyPreview; }

    void updateNodePosition(std::string const& nodeId, int x, int y);

  private:
    // Tree Model columns
    class ModelColumns : public Gtk::TreeModel::ColumnRecord
    {
    public:
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
    void addComponent(std::string const& type);
    void wrapNode(std::string const& containerType);

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
