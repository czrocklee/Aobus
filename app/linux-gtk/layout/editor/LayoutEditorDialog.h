// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "app/AppDialog.h"
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <giomm/simpleactiongroup.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/enums.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/paned.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/treemodel.h>
#include <gtkmm/treemodelcolumn.h>
#include <gtkmm/treestore.h>
#include <gtkmm/treeview.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <sigc++/connection.h>
#include <sigc++/signal.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  class ActionRegistry;
  class ComponentRegistry;
}

namespace ao::gtk::layout::editor
{
  struct LayoutSaveResult final
  {
    std::map<std::string, uimodel::layout::LayoutDocument, std::less<>> modified;
    std::vector<std::string> resets;
    std::string activePresetId;
    uimodel::layout::LayoutDocument activeDocument;
  };

  using LayoutLoaderFn = std::function<uimodel::layout::LayoutDocument(std::string_view presetId)>;

  class LayoutEditorDialog final : public AppDialog
  {
  public:
    LayoutEditorDialog(Gtk::Window& parent,
                       ComponentRegistry const& registry,
                       ActionRegistry const& actionRegistry,
                       uimodel::layout::LayoutDocument initialLayout,
                       std::string initialPresetId,
                       std::string initialThemeId,
                       LayoutLoaderFn layoutLoader);
    ~LayoutEditorDialog() override;

    LayoutEditorDialog(LayoutEditorDialog const&) = delete;
    LayoutEditorDialog& operator=(LayoutEditorDialog const&) = delete;
    LayoutEditorDialog(LayoutEditorDialog&&) = delete;
    LayoutEditorDialog& operator=(LayoutEditorDialog&&) = delete;

    uimodel::layout::LayoutDocument const& document() const { return _document; }
    std::string selectedPresetId() const { return _comboPresets.get_active_id(); }
    std::string selectedThemeId() const { return _comboThemePresets.get_active_id(); }

    sigc::signal<void(uimodel::layout::LayoutDocument const&)>& signalApplyPreview() { return _signalApplyPreview; }
    sigc::signal<void(std::string_view)>& signalThemePreview() { return _signalThemePreview; }
    sigc::signal<void(LayoutSaveResult const&)>& signalSaveRequest() { return _signalSaveRequest; }

    void updateNodePosition(std::string_view nodeId, std::int32_t posX, std::int32_t posY);

    // Test helper methods
    void testAddComponent(std::string type) { addComponent(std::move(type)); }
    void testWrapNode(std::string containerType) { wrapNode(std::move(containerType)); }
    void testOnResetDefault() { onResetDefault(); }
    void testMarkEdited() { markEdited(); }
    void testSuppressErrorDialogs() { _suppressErrorDialogsForTests = true; }

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
      Gtk::TreeModelColumn<uimodel::layout::LayoutNode*> nodePtr;
    };

    void setupUi();
    void populateTree();
    void appendNodeToTree(Gtk::TreeModel::Row parentRow, uimodel::layout::LayoutNode* node);
    void onSelectionChanged();
    void updatePropertiesPanel(uimodel::layout::LayoutNode* node);
    void applyPropertyChange(uimodel::layout::LayoutNode* node,
                             std::string_view propName,
                             uimodel::layout::LayoutValue const& value,
                             bool isLayoutProp);
    void notifyPreview();
    void scheduleDebouncedPreview();

    Gtk::Widget* renderIdSection(uimodel::layout::LayoutNode* node);
    void addSectionTitle(std::string_view text);
    Gtk::Widget* renderBoolEditor(uimodel::layout::LayoutNode* node,
                                  uimodel::layout::PropertyDescriptor const& prop,
                                  uimodel::layout::LayoutValue const& currentVal,
                                  bool isLayoutProp);
    Gtk::Widget* renderIntEditor(uimodel::layout::LayoutNode* node,
                                 uimodel::layout::PropertyDescriptor const& prop,
                                 uimodel::layout::LayoutValue const& currentVal,
                                 bool isLayoutProp);
    Gtk::Widget* renderEnumEditor(uimodel::layout::LayoutNode* node,
                                  uimodel::layout::PropertyDescriptor const& prop,
                                  uimodel::layout::LayoutValue const& currentVal,
                                  bool isLayoutProp);
    void populateActionComboBox(Gtk::ComboBoxText* combo,
                                uimodel::layout::LayoutNode* node,
                                uimodel::layout::PropertyDescriptor const& prop);
    Gtk::Widget* renderStringEditor(uimodel::layout::LayoutNode* node,
                                    uimodel::layout::PropertyDescriptor const& prop,
                                    uimodel::layout::LayoutValue const& currentVal,
                                    bool isLayoutProp);
    Gtk::Widget* dispatchEditor(uimodel::layout::LayoutNode* node,
                                uimodel::layout::PropertyDescriptor const& prop,
                                bool isLayoutProp);

    void addComponent(std::string type);
    void wrapNode(std::string containerType);

    void onRemoveNode();
    void onMoveUp();
    void onMoveDown();
    void onResetDefault();
    void onPresetChanged();

    bool validateAllDirtyDocuments();
    void presentErrorDialog(std::string const& title, std::string const& message);

    uimodel::layout::LayoutNode* findParentOf(uimodel::layout::LayoutNode* root, uimodel::layout::LayoutNode* target);
    uimodel::layout::LayoutNode* selectedNonRootNode() const;

    void markEdited();
    void stashCurrentDocument();

    struct SessionEntry final
    {
      uimodel::layout::LayoutDocument doc;
      bool dirty = false;
      bool resetPending = false;
    };

    ComponentRegistry const& _registry;
    ActionRegistry const& _actionRegistry;

    // Forward declaration of resolver (defined in ActionValidator.h already included)
    uimodel::layout::LayoutDocument _document;

    ModelColumns _columns;
    Glib::RefPtr<Gtk::TreeStore> _treeStorePtr;
    Gtk::TreeView _treeView;
    Gtk::ScrolledWindow _treeScroll;

    Gtk::Box _treeBox{Gtk::Orientation::VERTICAL};
    Gtk::Box _toolbar{Gtk::Orientation::HORIZONTAL};
    Gtk::MenuButton _btnAdd;
    Gtk::MenuButton _btnWrap;
    Gtk::Button _btnRemove{"Remove"};
    Gtk::Button _btnUp;
    Gtk::Button _btnDown;
    Gtk::Button _btnReset{"Reset Default"};
    Gtk::ComboBoxText _comboPresets;
    Gtk::ComboBoxText _comboThemePresets;

    Gtk::PopoverMenu _addPopover;
    Gtk::PopoverMenu _wrapPopover;

    Glib::RefPtr<Gio::SimpleActionGroup> _actionGroupPtr;

    Gtk::Box _propertiesBox{Gtk::Orientation::VERTICAL};
    Gtk::ScrolledWindow _propertiesScroll;

    Gtk::Paned _paned{Gtk::Orientation::HORIZONTAL};

    LayoutLoaderFn _layoutLoader;
    std::map<std::string, SessionEntry, std::less<>> _session;
    std::string _currentPresetId;
    sigc::connection _previewDebounceConn;
    bool _suppressErrorDialogsForTests = false;

    sigc::signal<void(uimodel::layout::LayoutDocument const&)> _signalApplyPreview;
    sigc::signal<void(std::string_view)> _signalThemePreview;
    sigc::signal<void(LayoutSaveResult const&)> _signalSaveRequest;
  };
} // namespace ao::gtk::layout::editor
