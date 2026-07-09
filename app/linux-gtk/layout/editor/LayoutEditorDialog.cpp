// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "LayoutEditorDialog.h"

#include "app/AppDialog.h"
#include "layout/document/GtkLayoutPresets.h"
#include "layout/document/LayoutDocument.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ActionValidator.h"
#include "layout/runtime/ComponentRegistry.h"
#include <ao/uimodel/layout/action/LayoutActionBinding.h>
#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionValidator.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutNodeId.h>

#include <giomm/menu.h>
#include <giomm/simpleactiongroup.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/box.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/object.h>
#include <gtkmm/paned.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/switch.h>
#include <gtkmm/treestore.h>
#include <gtkmm/treeview.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <sigc++/connection.h>
#include <sigc++/functors/mem_fun.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout::editor
{
  using namespace uimodel;
  namespace
  {
    constexpr int kTreeMinContentWidth = 220;
    constexpr int kTreeMinContentHeight = 460;
    constexpr int kPropertiesMinContentWidth = 420;
    constexpr int kPropertiesMaxContentWidth = 560;
    constexpr int kPropertiesMaxContentHeight = 560;
  }

  LayoutEditorDialog::LayoutEditorDialog(Gtk::Window& parent,
                                         ComponentRegistry const& registry,
                                         ActionRegistry const& actionRegistry,
                                         LayoutDocument initialLayout,
                                         std::string initialPresetId,
                                         std::string initialThemeId,
                                         LayoutLoaderFn layoutLoader)
    : AppDialog{}
    , _registry{registry}
    , _actionRegistry{actionRegistry}
    , _document{std::move(initialLayout)}
    , _columns{}
    , _treeStorePtr{Gtk::TreeStore::create(_columns)}
    , _actionGroupPtr{Gio::SimpleActionGroup::create()}
    , _layoutLoader{std::move(layoutLoader)}
    , _currentPresetId{initialPresetId}
  {
    set_title("Layout Editor");
    configureForParent(parent);
    set_default_size(-1, -1);

    addCancelAction("Cancel", Gtk::ResponseType::CANCEL);
    addPrimaryAction("Apply", Gtk::ResponseType::APPLY);
    addPrimaryAction("Save", Gtk::ResponseType::OK);

    buildUi();

    _session[initialPresetId] = SessionEntry{.doc = _document, .dirty = false, .resetPending = false};

    signal_response().connect(
      [this](std::int32_t responseId)
      {
        if (responseId == Gtk::ResponseType::APPLY)
        {
          notifyPreview();
        }
        else if (responseId == Gtk::ResponseType::OK)
        {
          stashCurrentDocument();

          if (validateAllDirtyDocuments())
          {
            auto result = LayoutSaveResult{};

            for (auto const& [id, entry] : _session)
            {
              if (entry.dirty)
              {
                result.modified[id] = entry.doc;
              }
              else if (entry.resetPending)
              {
                result.resets.push_back(id);
              }
            }

            result.activePresetId = _comboPresets.get_active_id().raw();
            result.activeDocument = _document;

            _signalSaveRequest.emit(result);
            close();
          }
        }
        else
        {
          close();
        }
      });

    _comboPresets.set_active_id(initialPresetId);
    _comboThemePresets.set_active_id(initialThemeId);

    _comboPresets.signal_changed().connect(sigc::mem_fun(*this, &LayoutEditorDialog::handlePresetChanged));

    _comboThemePresets.signal_changed().connect(
      [this]
      {
        if (auto const id = _comboThemePresets.get_active_id(); !id.empty())
        {
          _signalThemePreview.emit(id.raw());
        }
      });

    populateTree();
  }

  LayoutEditorDialog::~LayoutEditorDialog()
  {
    headerBar().remove(_comboPresets);
    headerBar().remove(_comboThemePresets);
    headerBar().remove(_btnReset);
  }

  void LayoutEditorDialog::buildUi()
  {
    _treeStorePtr = Gtk::TreeStore::create(_columns);
    _treeView.set_model(_treeStorePtr);

    _treeView.append_column("Node", _columns.displayName);
    _treeView.append_column("Type", _columns.type);

    _treeView.get_selection()->signal_changed().connect(
      sigc::mem_fun(*this, &LayoutEditorDialog::handleSelectionChanged));

    _treeScroll.set_child(_treeView);
    _treeScroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    _treeScroll.set_propagate_natural_width(true);
    _treeScroll.set_propagate_natural_height(true);
    _treeScroll.set_min_content_width(kTreeMinContentWidth);
    _treeScroll.set_min_content_height(kTreeMinContentHeight);
    _treeScroll.set_vexpand(true);

    _toolbar.set_spacing(2);
    _toolbar.add_css_class("ao-editor-toolbar");
    _toolbar.append(_btnAdd);
    _toolbar.append(_btnWrap);
    _toolbar.append(_btnRemove);
    _toolbar.append(_btnUp);
    _toolbar.append(_btnDown);

    headerBar().pack_end(_comboPresets);
    headerBar().pack_end(_comboThemePresets);
    headerBar().pack_end(_btnReset);

    _comboPresets.append("classic", "Classic Layout");
    _comboPresets.append("modern", "Modern Layout");

    _comboThemePresets.append("classic", "Classic Theme");
    _comboThemePresets.append("modern", "Modern Theme");

    _btnReset.set_icon_name("view-refresh-symbolic");
    _btnReset.set_tooltip_text("Reset to selected preset's default layout");
    _btnReset.add_css_class("flat");

    _actionGroupPtr = Gio::SimpleActionGroup::create();
    insert_action_group("editor", _actionGroupPtr);

    auto const addMenuPtr = Gio::Menu::create();
    auto const wrapMenuPtr = Gio::Menu::create();
    auto categoryMenus = std::map<std::string, Glib::RefPtr<Gio::Menu>>{};

    for (auto const& descriptor : _registry.descriptors())
    {
      auto const categoryLabel = std::string{uimodel::toString(descriptor.category)};

      if (!categoryMenus.contains(categoryLabel))
      {
        categoryMenus[categoryLabel] = Gio::Menu::create();
        addMenuPtr->append_submenu(categoryLabel, categoryMenus[categoryLabel]);
      }

      auto actionName = "add_" + descriptor.type;
      std::ranges::replace(actionName, '.', '_');

      categoryMenus[categoryLabel]->append(descriptor.displayName, "editor." + actionName);

      _actionGroupPtr->add_action(actionName, [this, type = descriptor.type] { addComponent(type); });

      if (uimodel::isContainer(descriptor))
      {
        auto wrapActionName = "wrap_" + descriptor.type;
        std::ranges::replace(wrapActionName, '.', '_');
        wrapMenuPtr->append(descriptor.displayName, "editor." + wrapActionName);
        _actionGroupPtr->add_action(wrapActionName, [this, type = descriptor.type] { wrapNode(type); });
      }
    }

    _btnAdd.set_icon_name("list-add-symbolic");
    _btnAdd.set_tooltip_text("Add Child");
    _btnAdd.set_menu_model(addMenuPtr);

    _btnWrap.set_icon_name("object-group-symbolic");
    _btnWrap.set_tooltip_text("Wrap Node");
    _btnWrap.set_menu_model(wrapMenuPtr);

    _btnRemove.set_icon_name("user-trash-symbolic");
    _btnRemove.set_tooltip_text("Remove Node");
    _btnRemove.signal_clicked().connect(sigc::mem_fun(*this, &LayoutEditorDialog::handleRemoveNodeClicked));

    _btnUp.set_icon_name("go-up-symbolic");
    _btnUp.set_tooltip_text("Move Up");
    _btnUp.signal_clicked().connect(sigc::mem_fun(*this, &LayoutEditorDialog::handleMoveUpClicked));

    _btnDown.set_icon_name("go-down-symbolic");
    _btnDown.set_tooltip_text("Move Down");
    _btnDown.signal_clicked().connect(sigc::mem_fun(*this, &LayoutEditorDialog::handleMoveDownClicked));

    _btnReset.signal_clicked().connect(sigc::mem_fun(*this, &LayoutEditorDialog::handleResetDefaultClicked));

    _treeBox.append(_toolbar);
    _treeBox.append(_treeScroll);

    _propertiesBox.add_css_class("ao-layout-editor-properties");

    int const spacing = 6;
    _propertiesBox.set_spacing(spacing);

    _propertiesScroll.set_child(_propertiesBox);
    _propertiesScroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _propertiesScroll.set_propagate_natural_width(true);
    _propertiesScroll.set_propagate_natural_height(true);
    _propertiesScroll.set_min_content_width(kPropertiesMinContentWidth);
    _propertiesScroll.set_max_content_width(kPropertiesMaxContentWidth);
    _propertiesScroll.set_max_content_height(kPropertiesMaxContentHeight);

    _paned.set_start_child(_treeBox);
    _paned.set_end_child(_propertiesScroll);

    int const panedPosition = 220;
    _paned.set_position(panedPosition);

    _paned.set_vexpand(true);
    setContentWidget(_paned);
  }

  void LayoutEditorDialog::populateTree()
  {
    _treeStorePtr->clear();

    auto row = *(_treeStorePtr->append());
    auto const optDescriptor = _registry.descriptor(_document.root.type);

    auto displayName = _document.root.id;

    if (displayName.empty())
    {
      displayName = optDescriptor ? optDescriptor->displayName : _document.root.type;
    }

    if (_document.root.type == "template")
    {
      if (auto const templateId = _document.root.propertyOr<std::string>("templateId", ""); !templateId.empty())
      {
        displayName += " [" + templateId + "]";
      }
    }

    row[_columns.displayName] = displayName;
    row[_columns.type] = _document.root.type;
    row[_columns.nodePtr] = &_document.root;

    for (auto& child : _document.root.children)
    {
      appendNodeToTree(row, &child);
    }

    _treeView.expand_all();
  }

  void LayoutEditorDialog::appendNodeToTree(Gtk::TreeModel::Row parentRow, LayoutNode* node)
  {
    auto row = *(_treeStorePtr->append(parentRow.children()));
    auto const optDescriptor = _registry.descriptor(node->type);
    auto displayName = node->id;

    if (displayName.empty())
    {
      displayName = optDescriptor ? optDescriptor->displayName : node->type;
    }

    if (node->type == "template")
    {
      if (auto const templateId = node->propertyOr<std::string>("templateId", ""); !templateId.empty())
      {
        displayName += " [" + templateId + "]";
      }
    }

    row[_columns.displayName] = displayName;
    row[_columns.type] = node->type;
    row[_columns.nodePtr] = node;

    for (auto& child : node->children)
    {
      appendNodeToTree(row, &child);
    }
  }

  LayoutNode* LayoutEditorDialog::findParentOf(LayoutNode* root, LayoutNode* target)
  {
    for (auto& child : root->children)
    {
      if (&child == target)
      {
        return root;
      }

      if (auto* parent = findParentOf(&child, target); parent != nullptr)
      {
        return parent;
      }
    }

    return nullptr;
  }

  namespace
  {
    LayoutNode* findNodeById(LayoutNode* root, std::string_view id)
    {
      if (root->id == id)
      {
        return root;
      }

      for (auto& child : root->children)
      {
        if (auto* match = findNodeById(&child, id); match != nullptr)
        {
          return match;
        }
      }

      return nullptr;
    }
  } // namespace

  void LayoutEditorDialog::updateNodePosition(std::string_view nodeId, std::int32_t xPosition, std::int32_t yPosition)
  {
    if (auto* const node = findNodeById(&_document.root, nodeId); node != nullptr)
    {
      node->layout["x"] = LayoutValue{static_cast<std::int64_t>(xPosition)};
      node->layout["y"] = LayoutValue{static_cast<std::int64_t>(yPosition)};
      markEdited();

      if (auto const row = _treeView.get_selection()->get_selected(); row)
      {
        if (row->get_value(_columns.nodePtr) == node)
        {
          updatePropertiesPanel(node);
        }
      }

      notifyPreview();
    }
  }

  void LayoutEditorDialog::addComponent(std::string type)
  {
    auto row = _treeView.get_selection()->get_selected();

    if (!row)
    {
      return;
    }

    auto* const parentNode = row->get_value(_columns.nodePtr);

    if (parentNode == nullptr)
    {
      return;
    }

    auto const optDescriptor = _registry.descriptor(parentNode->type);

    if (optDescriptor && optDescriptor->optMaxChildren && parentNode->children.size() >= *optDescriptor->optMaxChildren)
    {
      // Cannot add more children
      return;
    }

    auto newNode = LayoutNode{};
    newNode.type = std::move(type);
    newNode.id = uimodel::makeUniqueLayoutNodeId(_document, newNode.type, "new");

    markEdited();
    parentNode->children.push_back(std::move(newNode));

    populateTree();
    notifyPreview();
  }

  void LayoutEditorDialog::wrapNode(std::string containerType)
  {
    auto row = _treeView.get_selection()->get_selected();

    if (!row)
    {
      return;
    }

    auto* const targetNode = row->get_value(_columns.nodePtr);

    if (targetNode == nullptr || targetNode == &_document.root)
    {
      return; // Cannot wrap root
    }

    auto* const parentNode = findParentOf(&_document.root, targetNode);

    if (parentNode != nullptr)
    {
      auto const it =
        std::ranges::find_if(parentNode->children, [targetNode](auto const& child) { return &child == targetNode; });

      if (it != parentNode->children.end())
      {
        auto containerNode = LayoutNode{};
        containerNode.type = std::move(containerType);
        containerNode.id = uimodel::makeUniqueLayoutNodeId(_document, containerNode.type, "wrap");

        markEdited();
        // Move the target node into the new container
        containerNode.children.push_back(std::move(*it));

        // Replace the target node in the parent with the container
        *it = std::move(containerNode);

        populateTree();
        notifyPreview();
      }
    }
  }

  LayoutNode* LayoutEditorDialog::selectedNonRootNode() const
  {
    auto const row = _treeView.get_selection()->get_selected();

    if (!row)
    {
      return nullptr;
    }

    auto* const node = row->get_value(_columns.nodePtr);

    if (node == nullptr || node == &_document.root)
    {
      return nullptr;
    }

    return node;
  }

  void LayoutEditorDialog::handleRemoveNodeClicked()
  {
    auto* const targetNode = selectedNonRootNode();

    if (targetNode == nullptr)
    {
      return;
    }

    auto* const parentNode = findParentOf(&_document.root, targetNode);

    if (parentNode != nullptr)
    {
      auto const it =
        std::ranges::find_if(parentNode->children, [targetNode](auto const& child) { return &child == targetNode; });

      if (it != parentNode->children.end())
      {
        markEdited();
        parentNode->children.erase(it);
        populateTree();
        notifyPreview();
      }
    }
  }

  void LayoutEditorDialog::handleMoveUpClicked()
  {
    auto* const targetNode = selectedNonRootNode();

    if (targetNode == nullptr)
    {
      return;
    }

    auto* const parentNode = findParentOf(&_document.root, targetNode);

    if (parentNode != nullptr)
    {
      auto const it =
        std::ranges::find_if(parentNode->children, [targetNode](auto const& child) { return &child == targetNode; });

      if (it != parentNode->children.end() && it != parentNode->children.begin())
      {
        markEdited();
        std::iter_swap(it, it - 1);
        populateTree();
        notifyPreview();
      }
    }
  }

  void LayoutEditorDialog::handleMoveDownClicked()
  {
    auto* const targetNode = selectedNonRootNode();

    if (targetNode == nullptr)
    {
      return;
    }

    auto* const parentNode = findParentOf(&_document.root, targetNode);

    if (parentNode != nullptr)
    {
      auto const it =
        std::ranges::find_if(parentNode->children, [targetNode](auto const& child) { return &child == targetNode; });

      if (it != parentNode->children.end() && (it + 1) != parentNode->children.end())
      {
        markEdited();
        std::iter_swap(it, it + 1);
        populateTree();
        notifyPreview();
      }
    }
  }

  void LayoutEditorDialog::handleResetDefaultClicked()
  {
    auto const presetId = _comboPresets.get_active_id();

    if (presetId.empty())
    {
      return;
    }

    auto const presetEnum = presetIdFromString(presetId.raw());
    _document = makeBuiltInLayout(presetEnum);

    if (auto const it = _session.find(_currentPresetId); it != _session.end())
    {
      it->second.doc = _document;
      it->second.dirty = false;
      it->second.resetPending = true;
    }

    populateTree();
    notifyPreview();
  }

  void LayoutEditorDialog::handlePresetChanged()
  {
    auto const id = _comboPresets.get_active_id();

    if (id.empty() || id.raw() == _currentPresetId)
    {
      return;
    }

    if (_previewDebounceConn)
    {
      _previewDebounceConn.disconnect();
    }

    stashCurrentDocument();

    if (auto const it = _session.find(id.raw()); it != _session.end())
    {
      _document = it->second.doc;
    }
    else
    {
      _document = _layoutLoader(id.raw());
      _session.emplace(id.raw(), SessionEntry{.doc = _document, .dirty = false, .resetPending = false});
    }

    _currentPresetId = id.raw();
    populateTree();
    notifyPreview();
  }

  void LayoutEditorDialog::markEdited()
  {
    if (auto const it = _session.find(_currentPresetId); it != _session.end())
    {
      it->second.dirty = true;
      it->second.resetPending = false;
    }
  }

  void LayoutEditorDialog::stashCurrentDocument()
  {
    if (auto const it = _session.find(_currentPresetId); it != _session.end())
    {
      it->second.doc = _document;
    }
  }

  bool LayoutEditorDialog::validateAllDirtyDocuments()
  {
    for (auto& [presetId, entry] : _session)
    {
      if (entry.dirty || presetId == _currentPresetId)
      {
        auto const idDiagnostics = uimodel::validateStatefulLayoutNodeIds(entry.doc);

        if (uimodel::hasLayoutNodeIdErrors(idDiagnostics))
        {
          auto const firstErrorIt =
            std::ranges::find_if(idDiagnostics,
                                 [](uimodel::LayoutNodeIdDiagnostic const& diagnostic)
                                 { return diagnostic.severity == uimodel::LayoutNodeIdDiagnosticSeverity::Error; });
          auto const& firstError = *firstErrorIt;
          presentErrorDialog("Invalid Layout Component IDs",
                             std::format("Validation failed on preset '{}' component '{}' type '{}':\n\n{}",
                                         presetId,
                                         firstError.componentId,
                                         firstError.componentType,
                                         firstError.message));
          return false;
        }

        auto const diagnostics = uimodel::validateLayoutActions(
          entry.doc, _registry.catalog(), _actionRegistry.catalog(), resolveGtkLayoutActionBindingContext);

        if (!diagnostics.empty())
        {
          auto const& firstError = diagnostics.front();
          presentErrorDialog("Invalid Layout Actions",
                             std::format("Validation failed on preset '{}' component '{}' property '{}':\n\n{}",
                                         presetId,
                                         firstError.componentId,
                                         firstError.propertyName,
                                         firstError.message));
          return false;
        }
      }
    }

    return true;
  }

  void LayoutEditorDialog::presentErrorDialog(std::string const& title, std::string const& message)
  {
    AppDialog::presentMessage(
      *this,
      title,
      message,
      {AppDialogAction{.label = "OK", .responseId = Gtk::ResponseType::OK, .role = AppDialogActionRole::Primary}},
      Gtk::ResponseType::OK);
  }

  void LayoutEditorDialog::handleSelectionChanged()
  {
    if (auto const row = _treeView.get_selection()->get_selected(); row)
    {
      updatePropertiesPanel(row->get_value(_columns.nodePtr));
    }
    else
    {
      updatePropertiesPanel(nullptr);
    }
  }

  void LayoutEditorDialog::notifyPreview()
  {
    _signalApplyPreview.emit(_document);
  }

  void LayoutEditorDialog::applyPropertyChange(LayoutNode* node,
                                               std::string_view propName,
                                               LayoutValue const& value,
                                               bool isLayoutProp)
  {
    if (isLayoutProp)
    {
      node->layout[std::string{propName}] = value;
    }
    else
    {
      node->props[std::string{propName}] = value;
    }

    markEdited();
    scheduleDebouncedPreview();
  }

  void LayoutEditorDialog::scheduleDebouncedPreview()
  {
    if (_previewDebounceConn)
    {
      _previewDebounceConn.disconnect();
    }

    constexpr auto kDebounceInterval = std::chrono::milliseconds{500};

    _previewDebounceConn = Glib::signal_timeout().connect(
      [this] -> bool
      {
        notifyPreview();
        return false;
      },
      kDebounceInterval.count());
  }

  namespace
  {
    Gtk::Box* createPropertyRow(std::string const& label, Gtk::Widget& editor)
    {
      auto* const hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
      auto* const labelWidget = Gtk::make_managed<Gtk::Label>(label);
      labelWidget->set_halign(Gtk::Align::START);
      labelWidget->set_size_request(100, -1);

      // If it's a switch or something we want right-aligned, let it expand?
      // Actually we can just let editor set its own halign.
      editor.set_valign(Gtk::Align::CENTER);

      hbox->append(*labelWidget);

      // Let the editor expand if it's an entry
      if (dynamic_cast<Gtk::Entry*>(&editor) != nullptr)
      {
        editor.set_hexpand(true);
      }

      hbox->append(editor);
      return hbox;
    }
  } // namespace

  void LayoutEditorDialog::addSectionTitle(std::string_view text)
  {
    auto* const label = Gtk::make_managed<Gtk::Label>(std::format("<b>{}</b>", text));
    label->set_use_markup(true);
    label->set_halign(Gtk::Align::START);
    label->add_css_class("ao-layout-editor-section-title");
    _propertiesBox.append(*label);
  }

  Gtk::Widget* LayoutEditorDialog::renderIdSection(LayoutNode* node)
  {
    auto* const hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);

    auto* const label = Gtk::make_managed<Gtk::Label>("ID");
    label->set_halign(Gtk::Align::START);
    label->set_size_request(100, -1);

    auto* const entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(node->id);
    entry->set_hexpand(true);
    entry->add_css_class("flat-entry");

    entry->signal_changed().connect(
      [this, node, entry]
      {
        auto const newId = std::string{entry->get_text().raw()};

        if (node->id == newId)
        {
          return;
        }

        node->id = newId;
        markEdited();

        if (auto const row = _treeView.get_selection()->get_selected(); row)
        {
          auto const optDescriptor = _registry.descriptor(node->type);
          auto displayName = Glib::ustring{node->id};

          if (displayName.empty())
          {
            displayName = optDescriptor ? optDescriptor->displayName : node->type;
          }

          row->set_value(_columns.displayName, displayName);
        }

        scheduleDebouncedPreview();
      });

    hbox->append(*label);
    hbox->append(*entry);
    return hbox;
  }

  Gtk::Widget* LayoutEditorDialog::renderBoolEditor(LayoutNode* node,
                                                    LayoutPropertyDescriptor const& prop,
                                                    LayoutValue const& currentVal,
                                                    bool isLayoutProp)
  {
    auto* const check = Gtk::make_managed<Gtk::Switch>();
    check->set_halign(Gtk::Align::END);
    check->set_hexpand(true);
    check->set_active(currentVal.asBool());
    check->property_active().signal_changed().connect(
      [this, node, prop, check, isLayoutProp]
      { applyPropertyChange(node, prop.name, LayoutValue{check->get_active()}, isLayoutProp); });
    return createPropertyRow(prop.label, *check);
  }

  Gtk::Widget* LayoutEditorDialog::renderIntEditor(LayoutNode* node,
                                                   LayoutPropertyDescriptor const& prop,
                                                   LayoutValue const& currentVal,
                                                   bool isLayoutProp)
  {
    auto* const spin = Gtk::make_managed<Gtk::SpinButton>(
      Gtk::Adjustment::create(static_cast<double>(currentVal.asInt()), -9999, 9999, 1));
    spin->set_halign(Gtk::Align::END);
    spin->set_hexpand(true);
    spin->signal_value_changed().connect(
      [this, node, prop, spin, isLayoutProp]
      {
        applyPropertyChange(
          node, prop.name, LayoutValue{static_cast<std::int64_t>(spin->get_value_as_int())}, isLayoutProp);
      });
    return createPropertyRow(prop.label, *spin);
  }

  void LayoutEditorDialog::populateActionComboBox(Gtk::ComboBoxText* combo,
                                                  LayoutNode* node,
                                                  LayoutPropertyDescriptor const& prop)
  {
    if (!prop.optActionBinding)
    {
      return;
    }

    auto const bindCtx = LayoutActionBindingContext{.slot = prop.optActionBinding->slot,
                                                    .hasAnchor = true, // We assume standard UI buttons have anchors
                                                    .hasFocusedView = true, // And we assume focus is valid during edit
                                                    .componentType = node->type};

    combo->append("none", "none");

    for (auto const& desc : _actionRegistry.descriptors())
    {
      if (!_actionRegistry.canBind(desc.id, bindCtx))
      {
        continue;
      }

      auto label = std::string{desc.id};
      auto caps = std::vector<std::string>{};

      if (desc.capabilities.has(LayoutActionCapability::RequiresAnchor))
      {
        caps.emplace_back("Anchor");
      }

      if (desc.capabilities.has(LayoutActionCapability::PresentsMenu))
      {
        caps.emplace_back("Menu");
      }

      if (desc.capabilities.has(LayoutActionCapability::RequiresActiveTrack))
      {
        caps.emplace_back("Track");
      }

      if (desc.capabilities.has(LayoutActionCapability::RequiresFocusedView))
      {
        caps.emplace_back("Focus");
      }

      if (!caps.empty())
      {
        label += " [";

        for (std::size_t i = 0; i < caps.size(); ++i)
        {
          label += caps[i];

          if (i + 1 < caps.size())
          {
            label += ", ";
          }
        }

        label += "]";
      }

      combo->append(desc.id, label);
    }
  }

  Gtk::Widget* LayoutEditorDialog::renderEnumEditor(LayoutNode* node,
                                                    LayoutPropertyDescriptor const& prop,
                                                    LayoutValue const& currentVal,
                                                    bool isLayoutProp)
  {
    auto* const combo = Gtk::make_managed<Gtk::ComboBoxText>();
    combo->set_halign(Gtk::Align::END);
    combo->set_hexpand(true);

    if (prop.optActionBinding)
    {
      populateActionComboBox(combo, node, prop);
    }
    else
    {
      for (auto const& val : prop.enumValues)
      {
        combo->append(val, val);
      }
    }

    auto const currentStr = currentVal.asString();
    combo->set_active_id(currentStr);

    bool const isUnknown = (combo->get_active_row_number() == -1);

    combo->signal_changed().connect(
      [this, node, prop, combo, isLayoutProp]
      { applyPropertyChange(node, prop.name, LayoutValue{combo->get_active_id().raw()}, isLayoutProp); });

    auto* const rowBox = createPropertyRow(prop.label, *combo);

    if (isUnknown && prop.optActionBinding && currentStr != "none" && !currentStr.empty())
    {
      auto* const warning = Gtk::make_managed<Gtk::Label>("⚠ Unknown");
      warning->add_css_class("error");

      warning->set_markup(
        std::format("<span color='red' weight='bold'>⚠ Unknown ID: {}</span>", Glib::Markup::escape_text(currentStr)));
      rowBox->append(*warning);
    }

    return rowBox;
  }

  Gtk::Widget* LayoutEditorDialog::renderStringEditor(LayoutNode* node,
                                                      LayoutPropertyDescriptor const& prop,
                                                      LayoutValue const& currentVal,
                                                      bool isLayoutProp)
  {
    auto* const entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(currentVal.asString());
    entry->set_hexpand(true);
    entry->add_css_class("flat-entry");

    entry->signal_changed().connect(
      [this, node, prop, entry, isLayoutProp]
      { applyPropertyChange(node, prop.name, LayoutValue{entry->get_text().raw()}, isLayoutProp); });

    return createPropertyRow(prop.label, *entry);
  }

  Gtk::Widget* LayoutEditorDialog::renderPropertyEditor(LayoutNode* node,
                                                        LayoutPropertyDescriptor const& prop,
                                                        bool isLayoutProp)
  {
    auto currentVal = LayoutValue{prop.defaultValue};
    auto const& propertyMap = isLayoutProp ? node->layout : node->props;

    if (auto const it = propertyMap.find(prop.name); it != propertyMap.end())
    {
      currentVal = it->second;
    }

    switch (prop.kind)
    {
      case LayoutPropertyKind::Bool: return renderBoolEditor(node, prop, currentVal, isLayoutProp);
      case LayoutPropertyKind::Int: return renderIntEditor(node, prop, currentVal, isLayoutProp);
      case LayoutPropertyKind::Enum: return renderEnumEditor(node, prop, currentVal, isLayoutProp);
      case LayoutPropertyKind::String: return renderStringEditor(node, prop, currentVal, isLayoutProp);
      default:
      {
        auto* const placeholder = Gtk::make_managed<Gtk::Label>("(Unsupported editor)");
        return createPropertyRow(prop.label, *placeholder);
      }
    }
  }

  void LayoutEditorDialog::updatePropertiesPanel(LayoutNode* node)
  {
    if (_previewDebounceConn)
    {
      _previewDebounceConn.disconnect();
    }

    while (auto* child = _propertiesBox.get_first_child())
    {
      _propertiesBox.remove(*child);
    }

    if (node == nullptr)
    {
      auto* const label = Gtk::make_managed<Gtk::Label>("No selection");
      _propertiesBox.append(*label);
      return;
    }

    auto* const titleLabel = Gtk::make_managed<Gtk::Label>(std::format("<b>{}</b>", node->type));
    titleLabel->set_use_markup(true);
    titleLabel->set_halign(Gtk::Align::START);
    _propertiesBox.append(*titleLabel);

    _propertiesBox.append(*Gtk::make_managed<Gtk::Separator>());

    auto const optDescriptorOption = _registry.descriptor(node->type);

    auto appendToListBox = [&](Gtk::ListBox* list, Gtk::Widget* rowContent)
    {
      if (!rowContent)
      {
        return;
      }

      auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
      rowContent->set_margin(4);
      row->set_child(*rowContent);
      row->set_activatable(false);
      row->set_selectable(false);
      list->append(*row);
    };

    // 1. General Section (ID + Component properties)
    addSectionTitle("General");
    auto* const generalList = Gtk::make_managed<Gtk::ListBox>();
    generalList->add_css_class("ao-boxed-list");

    appendToListBox(generalList, renderIdSection(node));

    if (optDescriptorOption && !optDescriptorOption->props.empty())
    {
      for (auto const& prop : optDescriptorOption->props)
      {
        appendToListBox(generalList, renderPropertyEditor(node, prop, false));
      }
    }

    _propertiesBox.append(*generalList);

    // 2. Layout properties (component-specific + common)
    {
      auto layoutProps =
        optDescriptorOption ? optDescriptorOption->layoutProps : std::vector<LayoutPropertyDescriptor>{};
      auto const addCommon = [&](LayoutPropertyDescriptor prop)
      {
        if (!std::ranges::contains(layoutProps, prop.name, &LayoutPropertyDescriptor::name))
        {
          layoutProps.push_back(prop);
        }
      };

      addCommon({.name = "hexpand",
                 .kind = LayoutPropertyKind::Bool,
                 .label = "Expand Horizontal",
                 .defaultValue = LayoutValue{false}});
      addCommon({.name = "vexpand",
                 .kind = LayoutPropertyKind::Bool,
                 .label = "Expand Vertical",
                 .defaultValue = LayoutValue{false}});
      addCommon({.name = "halign",
                 .kind = LayoutPropertyKind::Enum,
                 .label = "Horizontal Align",
                 .defaultValue = LayoutValue{std::string{"fill"}},
                 .enumValues = {"fill", "start", "end", "center"}});
      addCommon({.name = "valign",
                 .kind = LayoutPropertyKind::Enum,
                 .label = "Vertical Align",
                 .defaultValue = LayoutValue{std::string{"fill"}},
                 .enumValues = {"fill", "start", "end", "center"}});
      addCommon({.name = "widthRequest",
                 .kind = LayoutPropertyKind::Int,
                 .label = "Width Request",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}});
      addCommon({.name = "heightRequest",
                 .kind = LayoutPropertyKind::Int,
                 .label = "Height Request",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}});
      addCommon({.name = "x",
                 .kind = LayoutPropertyKind::Int,
                 .label = "X",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(0)}});
      addCommon({.name = "y",
                 .kind = LayoutPropertyKind::Int,
                 .label = "Y",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(0)}});
      addCommon({.name = "width",
                 .kind = LayoutPropertyKind::Int,
                 .label = "Width",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}});
      addCommon({.name = "height",
                 .kind = LayoutPropertyKind::Int,
                 .label = "Height",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}});
      addCommon({.name = "zIndex",
                 .kind = LayoutPropertyKind::Int,
                 .label = "Z-Index",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(0)}});

      if (!layoutProps.empty())
      {
        addSectionTitle("Layout Properties");
        auto* const layoutList = Gtk::make_managed<Gtk::ListBox>();
        layoutList->add_css_class("ao-boxed-list");

        for (auto const& prop : layoutProps)
        {
          appendToListBox(layoutList, renderPropertyEditor(node, prop, true));
        }

        _propertiesBox.append(*layoutList);
      }
    }

    if (!optDescriptorOption)
    {
      auto* const label = Gtk::make_managed<Gtk::Label>("<i>No descriptor found</i>");
      label->set_use_markup(true);
      label->set_halign(Gtk::Align::START);
      label->add_css_class("ao-layout-editor-section-title");
      _propertiesBox.append(*label);
    }
  }
} // namespace ao::gtk::layout::editor
