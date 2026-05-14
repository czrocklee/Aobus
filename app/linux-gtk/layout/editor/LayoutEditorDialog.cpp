// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "LayoutEditorDialog.h"

#include "../LayoutConstants.h"
#include <gtkmm/box.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <memory>
#include <string>
#include <string_view>

#include <format>

namespace ao::gtk::layout::editor
{
  LayoutEditorDialog::LayoutEditorDialog(Gtk::Window& parent,
                                         ComponentRegistry const& registry,
                                         LayoutDocument const& initialDoc)
    : Gtk::Dialog("Layout Editor", parent, true), _registry(registry), _document(initialDoc)
  {
    int const defaultWidth = 800;
    int const defaultHeight = 600;
    set_default_size(defaultWidth, defaultHeight);

    add_button("Cancel", Gtk::ResponseType::CANCEL);
    add_button("Apply", Gtk::ResponseType::APPLY);
    add_button("Save", Gtk::ResponseType::OK);

    setupUi();
    populateTree();
  }

  LayoutEditorDialog::~LayoutEditorDialog() = default;

  void LayoutEditorDialog::setupUi()
  {
    _treeStore = Gtk::TreeStore::create(_columns);
    _treeView.set_model(_treeStore);

    _treeView.append_column("Node", _columns.displayName);
    _treeView.append_column("Type", _columns.type);

    _treeView.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &LayoutEditorDialog::onSelectionChanged));

    _treeScroll.set_child(_treeView);
    _treeScroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    _treeScroll.set_vexpand(true);

    _toolbar.set_spacing(2);
    _toolbar.append(_btnAdd);
    _toolbar.append(_btnWrap);
    _toolbar.append(_btnRemove);
    _toolbar.append(_btnUp);
    _toolbar.append(_btnDown);
    _toolbar.append(_btnRaiseZ);
    _toolbar.append(_btnLowerZ);
    _toolbar.append(_btnReset);

    _actionGroup = Gio::SimpleActionGroup::create();
    insert_action_group("editor", _actionGroup);

    auto const addMenu = Gio::Menu::create();
    auto const wrapMenu = Gio::Menu::create();
    auto categoryMenus = std::map<std::string, Glib::RefPtr<Gio::Menu>>{};

    for (auto const& descriptor : _registry.getDescriptors())
    {
      if (!categoryMenus.contains(descriptor.category))
      {
        categoryMenus[descriptor.category] = Gio::Menu::create();
        addMenu->append_submenu(descriptor.category, categoryMenus[descriptor.category]);
      }

      auto actionName = "add_" + descriptor.type;
      std::ranges::replace(actionName, '.', '_');

      categoryMenus[descriptor.category]->append(descriptor.displayName, "editor." + actionName);

      _actionGroup->add_action(actionName, [this, type = descriptor.type]() { addComponent(type); });

      if (descriptor.container)
      {
        auto wrapActionName = "wrap_" + descriptor.type;
        std::ranges::replace(wrapActionName, '.', '_');
        wrapMenu->append(descriptor.displayName, "editor." + wrapActionName);
        _actionGroup->add_action(wrapActionName, [this, type = descriptor.type]() { wrapNode(type); });
      }
    }

    _btnAdd.set_label("Add Child");
    _btnAdd.set_menu_model(addMenu);

    _btnWrap.set_label("Wrap In");
    _btnWrap.set_menu_model(wrapMenu);

    _btnRemove.signal_clicked().connect(sigc::mem_fun(*this, &LayoutEditorDialog::onRemoveNode));
    _btnUp.signal_clicked().connect(sigc::mem_fun(*this, &LayoutEditorDialog::onMoveUp));
    _btnDown.signal_clicked().connect(sigc::mem_fun(*this, &LayoutEditorDialog::onMoveDown));
    _btnRaiseZ.signal_clicked().connect(sigc::mem_fun(*this, &LayoutEditorDialog::onRaiseZ));
    _btnLowerZ.signal_clicked().connect(sigc::mem_fun(*this, &LayoutEditorDialog::onLowerZ));
    _btnReset.signal_clicked().connect(sigc::mem_fun(*this, &LayoutEditorDialog::onResetDefault));

    _treeBox.append(_toolbar);
    _treeBox.append(_treeScroll);

    int const padding = kMarginXLarge;
    _propertiesBox.set_margin_start(padding);
    _propertiesBox.set_margin_end(padding);
    _propertiesBox.set_margin_top(padding);
    _propertiesBox.set_margin_bottom(padding);

    int const spacing = 6;
    _propertiesBox.set_spacing(spacing);

    _propertiesScroll.set_child(_propertiesBox);
    _propertiesScroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);

    _paned.set_start_child(_treeBox);
    _paned.set_end_child(_propertiesScroll);

    int const panedPosition = 300;
    _paned.set_position(panedPosition);

    get_content_area()->append(_paned);
    _paned.set_vexpand(true);
  }

  void LayoutEditorDialog::populateTree()
  {
    _treeStore->clear();

    auto row = *(_treeStore->append());
    auto const descriptor = _registry.getDescriptor(_document.root.type);

    row[_columns.displayName] = descriptor ? descriptor->displayName : _document.root.id;
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
    auto row = *(_treeStore->append(parentRow.children()));
    auto const descriptor = _registry.getDescriptor(node->type);
    auto displayName = node->id;

    if (displayName.empty())
    {
      displayName = descriptor ? descriptor->displayName : node->type;
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

  LayoutNode* findNodeById(LayoutNode* root, std::string const& id)
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

  void LayoutEditorDialog::updateNodePosition(std::string const& nodeId, int posX, int posY)
  {
    auto* const node = findNodeById(&_document.root, nodeId);

    if (node != nullptr)
    {
      node->layout["x"] = LayoutValue{static_cast<std::int64_t>(posX)};
      node->layout["y"] = LayoutValue{static_cast<std::int64_t>(posY)};

      if (auto const row = _treeView.get_selection()->get_selected())
      {
        if (row->get_value(_columns.nodePtr) == node)
        {
          updatePropertiesPanel(node);
        }
      }

      notifyPreview();
    }
  }

  void LayoutEditorDialog::onRaiseZ()
  {
    auto const row = _treeView.get_selection()->get_selected();

    if (!row)
    {
      return;
    }

    auto* const node = row->get_value(_columns.nodePtr);

    if (node == nullptr || node == &_document.root)
    {
      return;
    }

    auto const currentZ = node->getLayout<std::int64_t>("zIndex", 0);
    node->layout["zIndex"] = LayoutValue{static_cast<std::int64_t>(currentZ + 1)};

    if (auto const selection = _treeView.get_selection()->get_selected())
    {
      if (selection->get_value(_columns.nodePtr) == node)
      {
        updatePropertiesPanel(node);
      }
    }

    notifyPreview();
  }

  void LayoutEditorDialog::onLowerZ()
  {
    auto row = _treeView.get_selection()->get_selected();

    if (!row)
    {
      return;
    }

    auto* const node = row->get_value(_columns.nodePtr);

    if (node == nullptr || node == &_document.root)
    {
      return;
    }

    auto const currentZ = node->getLayout<std::int64_t>("zIndex", 0);
    node->layout["zIndex"] =
      LayoutValue{static_cast<std::int64_t>(std::max(static_cast<std::int64_t>(0), currentZ - 1))};

    if (auto const selection = _treeView.get_selection()->get_selected())
    {
      if (selection->get_value(_columns.nodePtr) == node)
      {
        updatePropertiesPanel(node);
      }
    }

    notifyPreview();
  }

  void LayoutEditorDialog::addComponent(std::string const& type)
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

    auto const descriptor = _registry.getDescriptor(parentNode->type);

    if (descriptor && descriptor->maxChildren.has_value() && parentNode->children.size() >= *descriptor->maxChildren)
    {
      // Cannot add more children
      return;
    }

    auto newNode = LayoutNode{};
    newNode.type = type;
    newNode.id = type + "_new";
    std::ranges::replace(newNode.id, '.', '_');

    parentNode->children.push_back(std::move(newNode));

    populateTree();
    notifyPreview();
  }

  void LayoutEditorDialog::wrapNode(std::string const& containerType)
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
        containerNode.type = containerType;
        containerNode.id = containerType + "_wrap";

        // Move the target node into the new container
        containerNode.children.push_back(std::move(*it));

        // Replace the target node in the parent with the container
        *it = std::move(containerNode);

        populateTree();
        notifyPreview();
      }
    }
  }

  void LayoutEditorDialog::onAddChild()
  {
  } // no-op, removed usage

  void LayoutEditorDialog::onRemoveNode()
  {
    auto row = _treeView.get_selection()->get_selected();

    if (!row)
    {
      return;
    }

    auto* const targetNode = row->get_value(_columns.nodePtr);

    if (targetNode == nullptr || targetNode == &_document.root)
    {
      return; // Cannot remove root
    }

    auto* const parentNode = findParentOf(&_document.root, targetNode);

    if (parentNode != nullptr)
    {
      auto const it =
        std::ranges::find_if(parentNode->children, [targetNode](auto const& child) { return &child == targetNode; });

      if (it != parentNode->children.end())
      {
        parentNode->children.erase(it);
        populateTree();
        notifyPreview();
      }
    }
  }

  void LayoutEditorDialog::onMoveUp()
  {
    auto row = _treeView.get_selection()->get_selected();

    if (!row)
    {
      return;
    }

    auto* const targetNode = row->get_value(_columns.nodePtr);

    if (targetNode == nullptr || targetNode == &_document.root)
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
        std::iter_swap(it, it - 1);
        populateTree();
        notifyPreview();
      }
    }
  }

  void LayoutEditorDialog::onMoveDown()
  {
    auto row = _treeView.get_selection()->get_selected();

    if (!row)
    {
      return;
    }

    auto* const targetNode = row->get_value(_columns.nodePtr);

    if (targetNode == nullptr || targetNode == &_document.root)
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
        std::iter_swap(it, it + 1);
        populateTree();
        notifyPreview();
      }
    }
  }

  void LayoutEditorDialog::onResetDefault()
  {
    _document = createDefaultLayout();
    populateTree();
    notifyPreview();
  }

  void LayoutEditorDialog::onSelectionChanged()
  {
    if (auto const row = _treeView.get_selection()->get_selected())
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

    notifyPreview();
  }

  namespace
  {
    Gtk::Box* createPropertyRow(std::string const& label, Gtk::Widget& editor)
    {
      auto* const hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
      auto* const labelWidget = Gtk::make_managed<Gtk::Label>(label);
      labelWidget->set_halign(Gtk::Align::START);
      labelWidget->set_size_request(100, -1);
      hbox->append(*labelWidget);
      hbox->append(editor);
      return hbox;
    }
  }

  void LayoutEditorDialog::addSectionTitle(std::string_view text)
  {
    auto* const label = Gtk::make_managed<Gtk::Label>(std::format("<b>{}</b>", text));
    label->set_use_markup(true);
    label->set_halign(Gtk::Align::START);
    label->set_margin_top(kMarginXLarge);
    _propertiesBox.append(*label);
  }

  void LayoutEditorDialog::renderIdSection(LayoutNode* node)
  {
    auto* const hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kSpacingLarge);

    auto* const label = Gtk::make_managed<Gtk::Label>("ID");
    label->set_halign(Gtk::Align::START);
    label->set_size_request(100, -1);

    auto* const entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(node->id);
    entry->set_hexpand(true);

    auto const debounceConn = std::make_shared<sigc::connection>();
    constexpr int kDebounceMs = 500;

    entry->signal_changed().connect(
      [this, node, entry, debounceConn]
      {
        if (*debounceConn)
        {
          debounceConn->disconnect();
        }

        *debounceConn = Glib::signal_timeout().connect(
          [this, node, entry]() -> bool
          {
            auto const newId = std::string{entry->get_text().raw()};

            if (node->id == newId)
            {
              return false;
            }

            node->id = newId;

            if (auto const row = _treeView.get_selection()->get_selected())
            {
              auto const descriptor = _registry.getDescriptor(node->type);
              auto displayName = Glib::ustring{node->id};

              if (displayName.empty())
              {
                displayName = descriptor ? descriptor->displayName : node->type;
              }

              row->set_value(_columns.displayName, displayName);
            }

            notifyPreview();
            return false;
          },
          kDebounceMs);
      });

    hbox->append(*label);
    hbox->append(*entry);
    _propertiesBox.append(*hbox);
  }

  void LayoutEditorDialog::renderBoolEditor(LayoutNode* node,
                                            PropertyDescriptor const& prop,
                                            LayoutValue const& currentVal,
                                            bool isLayoutProp)
  {
    auto* const check = Gtk::make_managed<Gtk::CheckButton>();
    check->set_active(currentVal.asBool());
    check->signal_toggled().connect(
      [this, node, prop, check, isLayoutProp]
      { applyPropertyChange(node, prop.name, LayoutValue{check->get_active()}, isLayoutProp); });
    _propertiesBox.append(*createPropertyRow(prop.label, *check));
  }

  void LayoutEditorDialog::renderIntEditor(LayoutNode* node,
                                           PropertyDescriptor const& prop,
                                           LayoutValue const& currentVal,
                                           bool isLayoutProp)
  {
    auto* const spin = Gtk::make_managed<Gtk::SpinButton>(
      Gtk::Adjustment::create(static_cast<double>(currentVal.asInt()), -9999, 9999, 1));
    spin->signal_value_changed().connect(
      [this, node, prop, spin, isLayoutProp]
      {
        applyPropertyChange(
          node, prop.name, LayoutValue{static_cast<std::int64_t>(spin->get_value_as_int())}, isLayoutProp);
      });
    _propertiesBox.append(*createPropertyRow(prop.label, *spin));
  }

  void LayoutEditorDialog::renderEnumEditor(LayoutNode* node,
                                            PropertyDescriptor const& prop,
                                            LayoutValue const& currentVal,
                                            bool isLayoutProp)
  {
    auto* const combo = Gtk::make_managed<Gtk::ComboBoxText>();

    for (auto const& val : prop.enumValues)
    {
      combo->append(val, val);
    }

    combo->set_active_id(currentVal.asString());
    combo->signal_changed().connect(
      [this, node, prop, combo, isLayoutProp]
      { applyPropertyChange(node, prop.name, LayoutValue{combo->get_active_id().raw()}, isLayoutProp); });
    _propertiesBox.append(*createPropertyRow(prop.label, *combo));
  }

  void LayoutEditorDialog::renderStringEditor(LayoutNode* node,
                                              PropertyDescriptor const& prop,
                                              LayoutValue const& currentVal,
                                              bool isLayoutProp)
  {
    auto* const entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(currentVal.asString());
    entry->set_hexpand(true);

    auto const debounceConn = std::make_shared<sigc::connection>();
    constexpr int kDebounceMs = 500;

    entry->signal_changed().connect(
      [this, node, prop, entry, isLayoutProp, debounceConn]
      {
        if (*debounceConn)
        {
          debounceConn->disconnect();
        }

        *debounceConn = Glib::signal_timeout().connect(
          [this, node, prop, entry, isLayoutProp]() -> bool
          {
            applyPropertyChange(node, prop.name, LayoutValue{std::string{entry->get_text().raw()}}, isLayoutProp);
            return false;
          },
          kDebounceMs);
      });

    _propertiesBox.append(*createPropertyRow(prop.label, *entry));
  }

  void LayoutEditorDialog::dispatchEditor(LayoutNode* node, PropertyDescriptor const& prop, bool isLayoutProp)
  {
    auto currentVal = prop.defaultValue;
    auto const& propertyMap = isLayoutProp ? node->layout : node->props;

    if (auto const it = propertyMap.find(prop.name); it != propertyMap.end())
    {
      currentVal = it->second;
    }

    switch (prop.kind)
    {
      case PropertyKind::Bool: renderBoolEditor(node, prop, currentVal, isLayoutProp); break;
      case PropertyKind::Int: renderIntEditor(node, prop, currentVal, isLayoutProp); break;
      case PropertyKind::Enum: renderEnumEditor(node, prop, currentVal, isLayoutProp); break;
      case PropertyKind::String: renderStringEditor(node, prop, currentVal, isLayoutProp); break;
      default:
      {
        auto* const placeholder = Gtk::make_managed<Gtk::Label>("(Unsupported editor)");
        _propertiesBox.append(*createPropertyRow(prop.label, *placeholder));
      }
    }
  }

  void LayoutEditorDialog::updatePropertiesPanel(LayoutNode* node)
  {
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

    auto const descriptorOption = _registry.getDescriptor(node->type);

    // 1. ID editor
    renderIdSection(node);

    // 2. Component properties
    if (descriptorOption && !descriptorOption->props.empty())
    {
      addSectionTitle("Properties");

      for (auto const& prop : descriptorOption->props)
      {
        dispatchEditor(node, prop, false);
      }
    }

    // 3. Layout properties (component-specific + common)
    {
      auto layoutProps = descriptorOption ? descriptorOption->layoutProps : std::vector<PropertyDescriptor>{};
      auto const addCommon = [&](PropertyDescriptor prop)
      {
        if (std::ranges::find_if(layoutProps, [&](auto const& lp) { return lp.name == prop.name; }) ==
            layoutProps.end())
        {
          layoutProps.push_back(prop);
        }
      };

      addCommon({.name = "margin",
                 .kind = PropertyKind::Int,
                 .label = "Margin",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(0)}});
      addCommon({.name = "hexpand",
                 .kind = PropertyKind::Bool,
                 .label = "Expand Horizontal",
                 .defaultValue = LayoutValue{false}});
      addCommon({.name = "vexpand",
                 .kind = PropertyKind::Bool,
                 .label = "Expand Vertical",
                 .defaultValue = LayoutValue{false}});
      addCommon({.name = "halign",
                 .kind = PropertyKind::Enum,
                 .label = "Horizontal Align",
                 .defaultValue = LayoutValue{std::string{"fill"}},
                 .enumValues = {"fill", "start", "end", "center"}});
      addCommon({.name = "valign",
                 .kind = PropertyKind::Enum,
                 .label = "Vertical Align",
                 .defaultValue = LayoutValue{std::string{"fill"}},
                 .enumValues = {"fill", "start", "end", "center"}});
      addCommon({.name = "minWidth",
                 .kind = PropertyKind::Int,
                 .label = "Min Width",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}});
      addCommon({.name = "minHeight",
                 .kind = PropertyKind::Int,
                 .label = "Min Height",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}});
      addCommon({.name = "x",
                 .kind = PropertyKind::Int,
                 .label = "X",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(0)}});
      addCommon({.name = "y",
                 .kind = PropertyKind::Int,
                 .label = "Y",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(0)}});
      addCommon({.name = "width",
                 .kind = PropertyKind::Int,
                 .label = "Width",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}});
      addCommon({.name = "height",
                 .kind = PropertyKind::Int,
                 .label = "Height",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}});
      addCommon({.name = "zIndex",
                 .kind = PropertyKind::Int,
                 .label = "Z-Index",
                 .defaultValue = LayoutValue{static_cast<std::int64_t>(0)}});

      if (!layoutProps.empty())
      {
        addSectionTitle("Layout Properties");

        for (auto const& prop : layoutProps)
        {
          dispatchEditor(node, prop, true);
        }
      }
    }

    if (!descriptorOption)
    {
      auto* const label = Gtk::make_managed<Gtk::Label>("<i>No descriptor found</i>");
      label->set_use_markup(true);
      label->set_halign(Gtk::Align::START);
      label->set_margin_top(kMarginXLarge);
      _propertiesBox.append(*label);
    }
  }
} // namespace ao::gtk::layout::editor
