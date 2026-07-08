// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "app/AppDialog.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

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
    std::map<std::string, uimodel::LayoutDocument, std::less<>> modified;
    std::vector<std::string> resets;
    std::string activePresetId;
    uimodel::LayoutDocument activeDocument;
  };

  using LayoutLoaderFn = std::function<uimodel::LayoutDocument(std::string_view presetId)>;

  class LayoutEditorDialog final : public AppDialog
  {
  public:
    LayoutEditorDialog(Gtk::Window& parent,
                       ComponentRegistry const& registry,
                       ActionRegistry const& actionRegistry,
                       uimodel::LayoutDocument initialLayout,
                       std::string initialPresetId,
                       std::string initialThemeId,
                       LayoutLoaderFn layoutLoader);
    ~LayoutEditorDialog() override;

    LayoutEditorDialog(LayoutEditorDialog const&) = delete;
    LayoutEditorDialog& operator=(LayoutEditorDialog const&) = delete;
    LayoutEditorDialog(LayoutEditorDialog&&) = delete;
    LayoutEditorDialog& operator=(LayoutEditorDialog&&) = delete;

    uimodel::LayoutDocument const& document() const { return _document; }
    std::string selectedPresetId() const { return _comboPresets.get_active_id(); }
    std::string selectedThemeId() const { return _comboThemePresets.get_active_id(); }
    void setSelectedThemeId(std::string_view themeId) { _comboThemePresets.set_active_id(std::string{themeId}); }

    sigc::signal<void(uimodel::LayoutDocument const&)>& signalApplyPreview() { return _signalApplyPreview; }
    sigc::signal<void(std::string_view)>& signalThemePreview() { return _signalThemePreview; }
    sigc::signal<void(LayoutSaveResult const&)>& signalSaveRequest() { return _signalSaveRequest; }

    void updateNodePosition(std::string_view nodeId, std::int32_t xPosition, std::int32_t yPosition);

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
      Gtk::TreeModelColumn<uimodel::LayoutNode*> nodePtr;
    };

    void buildUi();
    void populateTree();
    void appendNodeToTree(Gtk::TreeModel::Row parentRow, uimodel::LayoutNode* node);
    void onSelectionChanged();
    void updatePropertiesPanel(uimodel::LayoutNode* node);
    void applyPropertyChange(uimodel::LayoutNode* node,
                             std::string_view propName,
                             uimodel::LayoutValue const& value,
                             bool isLayoutProp);
    void notifyPreview();
    void scheduleDebouncedPreview();

    Gtk::Widget* renderIdSection(uimodel::LayoutNode* node);
    void addSectionTitle(std::string_view text);
    Gtk::Widget* renderBoolEditor(uimodel::LayoutNode* node,
                                  uimodel::LayoutPropertyDescriptor const& prop,
                                  uimodel::LayoutValue const& currentVal,
                                  bool isLayoutProp);
    Gtk::Widget* renderIntEditor(uimodel::LayoutNode* node,
                                 uimodel::LayoutPropertyDescriptor const& prop,
                                 uimodel::LayoutValue const& currentVal,
                                 bool isLayoutProp);
    Gtk::Widget* renderEnumEditor(uimodel::LayoutNode* node,
                                  uimodel::LayoutPropertyDescriptor const& prop,
                                  uimodel::LayoutValue const& currentVal,
                                  bool isLayoutProp);
    void populateActionComboBox(Gtk::ComboBoxText* combo,
                                uimodel::LayoutNode* node,
                                uimodel::LayoutPropertyDescriptor const& prop);
    Gtk::Widget* renderStringEditor(uimodel::LayoutNode* node,
                                    uimodel::LayoutPropertyDescriptor const& prop,
                                    uimodel::LayoutValue const& currentVal,
                                    bool isLayoutProp);
    Gtk::Widget* renderPropertyEditor(uimodel::LayoutNode* node,
                                      uimodel::LayoutPropertyDescriptor const& prop,
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

    uimodel::LayoutNode* findParentOf(uimodel::LayoutNode* root, uimodel::LayoutNode* target);
    uimodel::LayoutNode* selectedNonRootNode() const;

    void markEdited();
    void stashCurrentDocument();

    struct SessionEntry final
    {
      uimodel::LayoutDocument doc;
      bool dirty = false;
      bool resetPending = false;
    };

    ComponentRegistry const& _registry;
    ActionRegistry const& _actionRegistry;

    // Forward declaration of resolver (defined in LayoutActionValidator.h already included)
    uimodel::LayoutDocument _document;

    ModelColumns _columns;
    Glib::RefPtr<Gtk::TreeStore> _treeStorePtr;
    Gtk::TreeView _treeView;
    Gtk::ScrolledWindow _treeScroll;

    Gtk::Box _treeBox{Gtk::Orientation::VERTICAL};
    Gtk::Box _toolbar{Gtk::Orientation::HORIZONTAL};
    Gtk::MenuButton _btnAdd;
    Gtk::MenuButton _btnWrap;
    Gtk::Button _btnRemove;
    Gtk::Button _btnUp;
    Gtk::Button _btnDown;
    Gtk::Button _btnReset;
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

    sigc::signal<void(uimodel::LayoutDocument const&)> _signalApplyPreview;
    sigc::signal<void(std::string_view)> _signalThemePreview;
    sigc::signal<void(LayoutSaveResult const&)> _signalSaveRequest;
  };
} // namespace ao::gtk::layout::editor
