// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/component/track/TrackFieldGridCustomControls.h"
#include "layout/component/track/TrackFieldGridRows.h"
#include "layout/component/track/TrackFieldGridTextUtils.h"
#include "layout/component/track/TrackFieldGridWidgets.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "track/TrackFieldUi.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/MetadataValueCompleter.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>
#include <ao/utility/Log.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/sizegroup.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel::layout;
  namespace
  {
    constexpr float kLabelOpacity = 0.6F;

    class ColumnWidthAnchor final : public Gtk::Widget
    {
    public:
      explicit ColumnWidthAnchor(std::string_view cssClass)
      {
        set_halign(Gtk::Align::FILL);
        set_hexpand(false);
        set_can_target(false);
        set_focusable(false);
        set_overflow(Gtk::Overflow::HIDDEN);
        set_size_request(0, -1);
        set_visible(true);
        add_css_class(std::string{cssClass});
      }

      void setWidgets(std::vector<Gtk::Widget*> widgets)
      {
        _widgets = std::move(widgets);
        queue_resize();
      }

    protected:
      Gtk::SizeRequestMode get_request_mode_vfunc() const override { return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH; }

      void measure_vfunc(Gtk::Orientation orientation,
                         int /*forSize*/,
                         int& minimum,
                         int& natural,
                         int& minimumBaseline,
                         int& naturalBaseline) const override
      {
        minimumBaseline = -1;
        naturalBaseline = -1;
        minimum = 0;
        natural = 0;

        if (orientation != Gtk::Orientation::HORIZONTAL)
        {
          return;
        }

        for (auto* const widget : _widgets)
        {
          if (widget == nullptr)
          {
            continue;
          }

          std::int32_t labelMinimum = 0;
          std::int32_t labelNatural = 0;
          std::int32_t labelMinimumBaseline = -1;
          std::int32_t labelNaturalBaseline = -1;
          widget->measure(
            Gtk::Orientation::HORIZONTAL, -1, labelMinimum, labelNatural, labelMinimumBaseline, labelNaturalBaseline);
          natural = std::max(natural, labelNatural);
        }
      }

      void size_allocate_vfunc(int /*width*/, int /*height*/, int /*baseline*/) override {}

    private:
      std::vector<Gtk::Widget*> _widgets;
    };

    using namespace track_field_grid;

    class TrackFieldGridComponent final : public ILayoutComponent
    {
    public:
      TrackFieldGridComponent(LayoutContext& ctx, LayoutNode const& node)
        : _editCoordinator{ctx.parentWindow}
        , _writer{ctx.runtime.library().writer()}
        , _completion{ctx.runtime.completion()}
        , _scope{ctx.track.detailScope}
        , _mainBox{Gtk::Orientation::VERTICAL, 0}
        , _compositePrimarySizeGroupPtr{Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL)}
        , _compositeSecondarySizeGroupPtr{Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL)}
      {
        _mainBox.set_vexpand(true);
        _fieldScroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        _fieldScroll.set_vexpand(true);
        _fieldScroll.set_propagate_natural_width(false);
        _fieldScroll.set_propagate_natural_height(false);
        _fieldScroll.set_child(_wrapper);
        _fieldViewport.set_vexpand(true);
        _mainBox.append(_fieldViewport);
        _undoBar.signalUndoRequested().connect([this] { onUndo(); });
        _mainBox.append(_undoBar.widget());

        _metadataHeader.button.signal_clicked().connect([this] { onToggleMetadata(); });
        _customHeader.button.signal_clicked().connect([this] { onToggleCustom(); });
        _technicalHeader.button.signal_clicked().connect([this] { onToggleTechnical(); });

        _metadataHeader.addCssClass("ao-track-detail-section-meta");
        _customHeader.addCssClass("ao-track-detail-section-custom");
        _technicalHeader.addCssClass("ao-track-detail-section-tech");

        updateHeaderStyles();

        _wrapper.setGrid(_grid);
        _wrapper.set_vexpand(true);
        _grid.set_hexpand(true);
        _grid.set_column_spacing(kGridColumnSpacing);
        _grid.set_row_spacing(4);
        _grid.set_valign(Gtk::Align::START);
        _grid.set_vexpand(true);
        _valueColumnWidthAnchor.set_hexpand(true);

        auto const requestedCategories =
          node.getProp<std::vector<std::string>>("categories", {"metadata", "technical"});

        for (auto const& def : rt::trackFieldDefinitions())
        {
          if (def.synthetic || def.category == rt::TrackFieldCategory::Tag || !def.presentable)
          {
            continue;
          }

          auto const isMeta = (def.category == rt::TrackFieldCategory::Metadata);

          if (auto const* const catStr = isMeta ? "metadata" : "technical";
              !std::ranges::contains(requestedCategories, catStr))
          {
            continue;
          }

          if (def.field == rt::TrackField::DiscTotal || def.field == rt::TrackField::TrackTotal ||
              def.field == rt::TrackField::MovementTotal)
          {
            continue; // Handled by composite rows
          }

          if (isMeta)
          {
            if (def.field == rt::TrackField::DiscNumber)
            {
              _compositeRows.emplace_back(rt::TrackField::DiscNumber, rt::TrackField::DiscTotal);
              setupCompositeRow(_compositeRows.back());
            }
            else if (def.field == rt::TrackField::TrackNumber)
            {
              _compositeRows.emplace_back(rt::TrackField::TrackNumber, rt::TrackField::TrackTotal);
              setupCompositeRow(_compositeRows.back());
            }
            else if (def.field == rt::TrackField::MovementNumber)
            {
              _compositeRows.emplace_back(rt::TrackField::MovementNumber, rt::TrackField::MovementTotal);
              setupCompositeRow(_compositeRows.back());
            }
            else
            {
              _metadataRows.emplace_back(def.field);
              setupBuiltInRow(_metadataRows.back());
            }
          }
          else
          {
            _technicalRows.emplace_back(def.field);
            setupBuiltInRow(_technicalRows.back(), true);
          }
        }

        setupAddPropertyUi();
        buildGrid();

        if (_scope != nullptr)
        {
          _scopeConn = _scope->signalSnapshotChanged().connect([this](auto const& snap) { onSnapshot(snap); });
          onSnapshot(_scope->snapshot());
        }
      }

      ~TrackFieldGridComponent() override = default;

      TrackFieldGridComponent(TrackFieldGridComponent const&) = delete;
      TrackFieldGridComponent& operator=(TrackFieldGridComponent const&) = delete;
      TrackFieldGridComponent(TrackFieldGridComponent&&) = delete;
      TrackFieldGridComponent& operator=(TrackFieldGridComponent&&) = delete;

      Gtk::Widget& widget() override { return _mainBox; }

    private:
      static constexpr std::int32_t kGridColumnSpacing = 12;
      static constexpr std::int32_t kValueColWidth = 3;
      static constexpr std::int32_t kFieldRowHeight = 28;
      static constexpr std::int32_t kVisibleFieldRows = 8;
      static constexpr std::int32_t kGridRowSpacing = 8;
      static constexpr std::int32_t kGridViewportHeight =
        (kVisibleFieldRows * kFieldRowHeight) + ((kVisibleFieldRows - 1) * kGridRowSpacing);

      void onToggleMetadata()
      {
        _metadataExpanded = !_metadataExpanded;
        _metadataHeader.setExpanded(_metadataExpanded);
        updateHeaderStyles();

        for (auto& row : _metadataRows)
        {
          row.labelSlot.set_visible(_metadataExpanded);
          row.valueSlot.set_visible(_metadataExpanded);
        }

        for (auto& row : _compositeRows)
        {
          row.labelSlot.set_visible(_metadataExpanded);
          row.valueSlot.set_visible(_metadataExpanded);
        }
      }

      void onToggleCustom()
      {
        _customExpanded = !_customExpanded;
        _customHeader.setExpanded(_customExpanded);
        updateHeaderStyles();

        for (auto& row : _customRows)
        {
          row.labelSlot.set_visible(_customExpanded);
          row.valueSlot.set_visible(_customExpanded);
        }

        _addPropertyRow.keySlot().set_visible(_customExpanded);
        _addPropertyRow.valueSlot().set_visible(_customExpanded);
      }

      void onToggleTechnical()
      {
        _technicalExpanded = !_technicalExpanded;
        _technicalHeader.setExpanded(_technicalExpanded);
        updateHeaderStyles();

        for (auto& row : _technicalRows)
        {
          row.labelSlot.set_visible(_technicalExpanded);
          row.valueSlot.set_visible(_technicalExpanded);
        }
      }

      void updateHeaderStyles()
      {
        auto toggleClass = [](auto& header, bool const expanded)
        {
          if (expanded)
          {
            header.button.remove_css_class("is-collapsed");
          }
          else
          {
            header.button.add_css_class("is-collapsed");
          }
        };

        toggleClass(_metadataHeader, _metadataExpanded);
        toggleClass(_customHeader, _customExpanded);
        toggleClass(_technicalHeader, _technicalExpanded);
      }

      void onUndo()
      {
        auto optPendingUndo = _undoBar.takePendingUndo();

        if (!optPendingUndo)
        {
          return;
        }

        auto patch = rt::MetadataPatch{};
        patch.customUpdates[optPendingUndo->key] = optPendingUndo->value;

        _writer.updateMetadata(optPendingUndo->trackIds, patch);
      }

      void onSnapshot(rt::TrackDetailSnapshot const& snap)
      {
        for (auto& row : _metadataRows)
        {
          updateBuiltInRow(row, snap);
        }

        for (auto& row : _technicalRows)
        {
          updateBuiltInRow(row, snap);
        }

        for (auto& row : _compositeRows)
        {
          updateCompositeRow(row, snap);
        }

        updateCustomRows(snap);
      }

      void setupBuiltInRow(BuiltInRow& row, bool isTechnical = false)
      {
        auto const* def = rt::trackFieldDefinition(row.field);
        row.label.set_text(std::string{def != nullptr ? def->label : ""});
        configureKeyLabel(row.label);

        row.label.set_opacity(kLabelOpacity);
        row.label.add_css_class("ao-property-label");

        configureValueBox(row.valueBox);
        configureValueEditor(row.valueEditor);
        row.valueEditor.add_css_class("ao-property-value");
        _editCoordinator.registerEditor(row.valueEditor);

        if (isTechnical)
        {
          row.label.add_css_class("ao-detail-field-technical");
          row.valueEditor.add_css_class("ao-detail-field-technical-value");
        }

        if (row.editable)
        {
          if (rt::trackFieldSupportsValueCompletion(row.field))
          {
            row.valueEditor.setCompletionProvider(rt::MetadataValueCompleter{_completion, row.field}.asProvider());
          }

          row.valueEditor.add_css_class("ao-property-editable");
          row.valueEditor.signalCommitted().connect([this, field = row.field] { onBuiltInEdited(field); });
          row.valueEditor.signalCanceled().connect(
            [this, field = row.field]
            {
              if (auto* row = findBuiltInRow(field); row != nullptr)
              {
                if (_scope != nullptr)
                {
                  updateBuiltInRow(*row, _scope->snapshot());
                }
              }
            });
        }
      }

      void setupCompositeRow(CompositeBuiltInRow& row)
      {
        auto const* def = rt::trackFieldDefinition(row.primaryField);
        row.label.set_text(std::string{def != nullptr ? def->label : ""});
        configureKeyLabel(row.label);

        row.label.set_opacity(kLabelOpacity);
        row.label.add_css_class("ao-property-label");

        configureValueBox(row.valueBox);
        row.separatorLabel.set_opacity(kLabelOpacity);

        configureValueEditor(row.primaryEditor);
        row.primaryEditor.add_css_class("ao-property-value");
        row.primaryEditor.removeMaxWidthConstraint();
        row.primaryEditor.set_hexpand(false);
        row.primaryEditor.set_halign(Gtk::Align::START);
        _compositePrimarySizeGroupPtr->add_widget(row.primaryEditor);

        configureValueEditor(row.secondaryEditor);
        row.secondaryEditor.add_css_class("ao-property-value");
        row.secondaryEditor.removeMaxWidthConstraint();
        row.secondaryEditor.set_hexpand(false);
        row.secondaryEditor.set_halign(Gtk::Align::START);
        _compositeSecondarySizeGroupPtr->add_widget(row.secondaryEditor);

        auto bindEditor = [this](DetailFieldEditor& editor, bool isEditable, rt::TrackField field)
        {
          _editCoordinator.registerEditor(editor);

          if (isEditable)
          {
            if (rt::trackFieldSupportsValueCompletion(field))
            {
              editor.setCompletionProvider(rt::MetadataValueCompleter{_completion, field}.asProvider());
            }

            editor.add_css_class("ao-property-editable");
            editor.signalCommitted().connect([this, field] { onCompositeEdited(field); });
            editor.signalCanceled().connect(
              [this, field]
              {
                if (auto* row = findCompositeBuiltInRow(field); row != nullptr)
                {
                  if (_scope != nullptr)
                  {
                    updateCompositeRow(*row, _scope->snapshot());
                  }
                }
              });
          }
        };

        bindEditor(row.primaryEditor, row.primaryEditableFlag, row.primaryField);
        bindEditor(row.secondaryEditor, row.secondaryEditableFlag, row.secondaryField);
      }

      void updateBuiltInRow(BuiltInRow& row, rt::TrackDetailSnapshot const& snap)
      {
        if (row.valueEditor.getEditing())
        {
          return;
        }

        auto const displayText = validUtf8Text(displayTextForField(row.field, snap, kMultipleValuesText, true));
        row.valueEditor.setText(displayText);
        row.valueEditor.set_tooltip_text(displayText);
      }

      void updateCompositeRow(CompositeBuiltInRow& row, rt::TrackDetailSnapshot const& snap)
      {
        auto updateField = [](DetailFieldEditor& editor, rt::TrackField field, rt::TrackDetailSnapshot const& snap)
        {
          if (editor.getEditing())
          {
            return;
          }

          auto const displayText = validUtf8Text(displayTextForField(field, snap, kCompositeMixedText, false));
          editor.setText(displayText);
          editor.set_tooltip_text(displayText);
        };

        updateField(row.primaryEditor, row.primaryField, snap);
        updateField(row.secondaryEditor, row.secondaryField, snap);
      }

      bool applyFieldEdit(rt::TrackField field, std::string_view newValue, rt::TrackDetailSnapshot const& snap)
      {
        auto const* uiDef = trackFieldUiDefinition(field);

        if (uiDef == nullptr || uiDef->parseInlineEdit == nullptr || uiDef->writePatch == nullptr)
        {
          return false;
        }

        auto const editValResult = uiDef->parseInlineEdit(newValue);

        if (!editValResult)
        {
          APP_LOG_ERROR(
            "Failed to parse edit value for {}: {}", rt::trackFieldId(field), editValResult.error().message);
          return false;
        }

        auto patch = rt::MetadataPatch{};
        uiDef->writePatch({.patch = patch, .value = *editValResult});

        auto const reply = _writer.updateMetadata(snap.trackIds, patch);

        // Empty mutatedIds means the edit did not land (e.g. the row was removed
        // concurrently); report it so the caller reverts the optimistic UI value.
        return !reply.mutatedIds.empty();
      }

      void onBuiltInEdited(rt::TrackField field)
      {
        auto* row = findBuiltInRow(field);

        if (row == nullptr || !row->editable || row->valueEditor.getEditing() || _scope == nullptr)
        {
          return;
        }

        auto const snap = _scope->snapshot();

        if (snap.trackIds.empty())
        {
          return;
        }

        auto const newValue = row->valueEditor.getText().raw();

        if (isProtectedFieldEditValue(field, snap, newValue, false))
        {
          return;
        }

        if (!applyFieldEdit(field, newValue, snap))
        {
          updateBuiltInRow(*row, snap);
        }
      }

      void onCompositeEdited(rt::TrackField field)
      {
        auto* row = findCompositeBuiltInRow(field);

        if (row == nullptr || _scope == nullptr)
        {
          return;
        }

        bool const isPrimary = (row->primaryField == field);
        bool const editable = isPrimary ? row->primaryEditableFlag : row->secondaryEditableFlag;
        auto& editor = isPrimary ? row->primaryEditor : row->secondaryEditor;

        if (!editable || editor.getEditing())
        {
          return;
        }

        auto const& snap = _scope->snapshot();
        auto const newValue = editor.getText().raw();

        if (isProtectedFieldEditValue(field, snap, newValue, true))
        {
          return;
        }

        if (!applyFieldEdit(field, newValue, snap))
        {
          updateCompositeRow(*row, snap);
        }
      }

      void updateCustomRows(rt::TrackDetailSnapshot const& snap)
      {
        bool keysChanged = (snap.customMetadata.size() != _customRows.size());

        if (!keysChanged)
        {
          for (std::uint32_t i = 0U; i < snap.customMetadata.size(); ++i)
          {
            if (snap.customMetadata[i].key != _customRows[i].key)
            {
              keysChanged = true;
              break;
            }
          }
        }

        if (keysChanged)
        {
          for (auto& row : _customRows)
          {
            _editCoordinator.forgetEditor(row.editor);
          }

          _customRows.clear();

          for (auto const& item : snap.customMetadata)
          {
            _customRows.emplace_back(item.key, kGridColumnSpacing);
            setupCustomRow(_customRows.back());
          }

          buildGrid();
        }

        for (std::uint32_t i = 0U; i < snap.customMetadata.size(); ++i)
        {
          updateCustomRow(_customRows[i], snap.customMetadata[i]);
        }
      }

      void setupCustomRow(CustomRow& row)
      {
        row.label.set_text(validUtf8Text(row.key));
        configureKeyLabel(row.label);
        row.label.set_ellipsize(Pango::EllipsizeMode::END);
        row.label.set_width_chars(0);
        row.label.set_max_width_chars(24);
        row.label.set_opacity(kLabelOpacity);
        row.label.add_css_class("ao-property-label");

        configureValueBox(row.valueBox);
        configureValueEditor(row.editor);
        row.editor.add_css_class("ao-property-value");
        row.editor.add_css_class("ao-property-editable");
        _editCoordinator.registerEditor(row.editor);
        row.editor.signalCommitted().connect([this, key = row.key] { onCustomEdited(key); });
        row.editor.signalCanceled().connect(
          [this, key = row.key]
          {
            if (auto* row = findCustomRow(key); row != nullptr)
            {
              if (_scope != nullptr)
              {
                updateCustomRows(_scope->snapshot());
              }
            }
          });

        row.deleteButton.set_icon_name("user-trash-symbolic");
        row.deleteButton.set_halign(Gtk::Align::END);
        row.deleteButton.set_hexpand(false);
        row.deleteButton.set_has_frame(false);
        row.deleteButton.add_css_class("ao-icon-button");
        row.deleteButton.add_css_class("ao-detail-field-delete");
        row.deleteButton.set_tooltip_text("Delete Property");
        row.deleteButton.signal_clicked().connect([this, key = row.key] { onCustomDeleted(key); });

        row.partialIcon.set_from_icon_name("dialog-warning-symbolic");
        row.partialIcon.set_opacity(kLabelOpacity);
        row.partialIcon.set_tooltip_text("Missing on some tracks");
      }

      void updateCustomRow(CustomRow& row, rt::CustomMetadataItem const& item)
      {
        if (row.editor.getEditing())
        {
          return;
        }

        auto text = std::string{};

        if (item.value.mixed)
        {
          text = kMultipleValuesText;
        }
        else
        {
          text = item.value.optValue.value_or("");
        }

        auto const displayText = validUtf8Text(text);
        row.editor.setText(displayText);
        row.editor.set_tooltip_text(displayText);
        row.partialIcon.set_visible(!item.presentOnAll);
      }

      void onCustomEdited(std::string const& key)
      {
        auto* row = findCustomRow(key);

        if (row == nullptr || row->editor.getEditing() || _scope == nullptr)
        {
          return;
        }

        auto const snap = _scope->snapshot();
        auto const newValue = row->editor.getText().raw();

        if (newValue == kMultipleValuesText)
        {
          return;
        }

        auto patch = rt::MetadataPatch{};
        patch.customUpdates[key] = newValue;

        _writer.updateMetadata(snap.trackIds, patch);
      }

      void onCustomDeleted(std::string const& key)
      {
        if (_scope == nullptr)
        {
          return;
        }

        auto const snap = _scope->snapshot();

        auto optPrevValue = std::optional<std::string>{};

        for (auto const& item : snap.customMetadata)
        {
          if (item.key == key && item.presentOnAll && !item.value.mixed)
          {
            optPrevValue = item.value.optValue.value_or("");
            break;
          }
        }

        auto patch = rt::MetadataPatch{};
        patch.customUpdates[key] = std::nullopt;

        auto const reply = _writer.updateMetadata(snap.trackIds, patch);

        if (!reply.mutatedIds.empty() && optPrevValue)
        {
          _undoBar.show(key, snap.trackIds, *optPrevValue);
        }
      }

      void onCustomAdded(std::string key, std::string value)
      {
        if (_scope == nullptr)
        {
          return;
        }

        auto const& snap = _scope->snapshot();

        for (auto const& item : snap.customMetadata)
        {
          if (item.key == key)
          {
            _addPropertyRow.markKeyError();
            return;
          }
        }

        if (auto const optField = rt::trackFieldFromId(key); optField)
        {
          _addPropertyRow.markKeyError();
          return;
        }

        auto patch = rt::MetadataPatch{};
        patch.customUpdates[key] = value;
        _writer.updateMetadata(snap.trackIds, patch);

        _addPropertyRow.clearInputs();
      }

      void buildGrid()
      {
        clearGrid();
        std::int32_t rowIdx = 0;

        refreshColumnWidthAnchors();
        _grid.attach(_keyColumnWidthAnchor, 0, 0, 1, 1);
        _grid.attach(_valueColumnWidthAnchor, 1, 0, kValueColWidth, 1);

        bool const hasMetadata = !_metadataRows.empty() || !_compositeRows.empty();

        if (hasMetadata)
        {
          _metadataHeader.setExpanded(_metadataExpanded);
          _grid.attach(_metadataHeader.button, 0, rowIdx++, 1 + kValueColWidth, 1);

          attachBuiltInGroup(_metadataRows, rowIdx, _metadataExpanded);

          for (auto& row : _compositeRows)
          {
            attachCompositeRow(row, rowIdx++);
            row.labelSlot.set_visible(_metadataExpanded);
            row.valueSlot.set_visible(_metadataExpanded);
          }
        }

        bool const tracksSelected = (_scope != nullptr && !_scope->snapshot().trackIds.empty());

        if (tracksSelected)
        {
          _customHeader.setExpanded(_customExpanded);
          _grid.attach(_customHeader.button, 0, rowIdx++, 1 + kValueColWidth, 1);

          for (auto& row : _customRows)
          {
            _grid.attach(row.labelSlot, 0, rowIdx, 1, 1);
            _grid.attach(row.valueSlot, 1, rowIdx, kValueColWidth, 1);
            row.labelSlot.set_visible(_customExpanded);
            row.valueSlot.set_visible(_customExpanded);
            rowIdx++;
          }

          attachAddPropertyRow(rowIdx++);
          _addPropertyRow.keySlot().set_visible(_customExpanded);
          _addPropertyRow.valueSlot().set_visible(_customExpanded);
        }

        if (!_technicalRows.empty())
        {
          _technicalHeader.setExpanded(_technicalExpanded);
          _grid.attach(_technicalHeader.button, 0, rowIdx++, 1 + kValueColWidth, 1);
          attachBuiltInGroup(_technicalRows, rowIdx, _technicalExpanded);
        }
      }

      void clearGrid()
      {
        while (auto* child = _grid.get_first_child())
        {
          _grid.remove(*child);
        }
      }

      void attachBuiltInRow(BuiltInRow& row, std::int32_t const rowNum)
      {
        _grid.attach(row.labelSlot, 0, rowNum, 1, 1);
        _grid.attach(row.valueSlot, 1, rowNum, kValueColWidth, 1);
      }

      void attachCompositeRow(CompositeBuiltInRow& row, std::int32_t const rowNum)
      {
        _grid.attach(row.labelSlot, 0, rowNum, 1, 1);
        _grid.attach(row.valueSlot, 1, rowNum, kValueColWidth, 1);
      }

      void attachBuiltInGroup(std::deque<BuiltInRow>& rows, std::int32_t& rowIdx, bool const expanded)
      {
        for (auto& row : rows)
        {
          attachBuiltInRow(row, rowIdx++);
          row.labelSlot.set_visible(expanded);
          row.valueSlot.set_visible(expanded);
        }
      }

      void attachAddPropertyRow(std::int32_t const rowIdx)
      {
        configureValueBox(_addPropertyRow.valueSlot());
        _grid.attach(_addPropertyRow.keySlot(), 0, rowIdx, 1, 1);
        _grid.attach(_addPropertyRow.valueSlot(), 1, rowIdx, kValueColWidth, 1);
      }

      void refreshColumnWidthAnchors()
      {
        auto keyWidgets = std::vector<Gtk::Widget*>{};
        auto valueWidgets = std::vector<Gtk::Widget*>{};
        auto const rowCount = _metadataRows.size() + _compositeRows.size() + _technicalRows.size() + _customRows.size();
        keyWidgets.reserve(rowCount);
        valueWidgets.reserve(rowCount + 1);

        for (auto& row : _metadataRows)
        {
          keyWidgets.push_back(&row.label);
          valueWidgets.push_back(&row.valueBox);
        }

        for (auto& row : _compositeRows)
        {
          keyWidgets.push_back(&row.label);
          valueWidgets.push_back(&row.valueBox);
        }

        for (auto& row : _technicalRows)
        {
          keyWidgets.push_back(&row.label);
          valueWidgets.push_back(&row.valueBox);
        }

        for (auto& row : _customRows)
        {
          keyWidgets.push_back(&row.label);
          valueWidgets.push_back(&row.valueBox);
        }

        valueWidgets.push_back(_addPropertyRow.valueSlot().get_first_child());
        _keyColumnWidthAnchor.setWidgets(std::move(keyWidgets));
        _valueColumnWidthAnchor.setWidgets(std::move(valueWidgets));
      }

      void configureValueBox(Gtk::Widget& box)
      {
        box.set_halign(Gtk::Align::FILL);
        box.set_hexpand(true);
        box.set_overflow(Gtk::Overflow::HIDDEN);
        box.set_size_request(0, -1);
      }

      void configureKeyLabel(Gtk::Label& label)
      {
        label.set_halign(Gtk::Align::END);
        label.set_xalign(1.0F);
        label.set_hexpand(false);
        label.set_overflow(Gtk::Overflow::HIDDEN);
        label.set_size_request(0, -1);
        label.set_ellipsize(Pango::EllipsizeMode::NONE);
        label.set_wrap(false);
        label.set_lines(1);
      }

      void configureValueEditor(DetailFieldEditor& editor)
      {
        editor.set_halign(Gtk::Align::FILL);
        editor.set_hexpand(true);
        editor.set_overflow(Gtk::Overflow::HIDDEN);
        editor.set_size_request(0, -1);
      }

      void setupAddPropertyUi()
      {
        _addPropertyRow.signalAddRequested().connect([this](std::string key, std::string value)
                                                     { onCustomAdded(std::move(key), std::move(value)); });
      }

      BuiltInRow* findBuiltInRow(rt::TrackField field)
      {
        for (auto& row : _metadataRows)
        {
          if (row.field == field)
          {
            return &row;
          }
        }

        for (auto& row : _technicalRows)
        {
          if (row.field == field)
          {
            return &row;
          }
        }

        return nullptr;
      }

      CompositeBuiltInRow* findCompositeBuiltInRow(rt::TrackField field)
      {
        for (auto& row : _compositeRows)
        {
          if (row.primaryField == field || row.secondaryField == field)
          {
            return &row;
          }
        }

        return nullptr;
      }

      CustomRow* findCustomRow(std::string const& key)
      {
        for (auto& row : _customRows)
        {
          if (row.key == key)
          {
            return &row;
          }
        }

        return nullptr;
      }

      Gtk::Grid _grid;
      DetailEditCoordinator _editCoordinator;
      rt::LibraryWriter& _writer;
      rt::CompletionService& _completion;
      ITrackDetailScope* _scope;
      std::deque<BuiltInRow> _metadataRows;
      std::deque<CompositeBuiltInRow> _compositeRows;
      std::deque<BuiltInRow> _technicalRows;
      std::deque<CustomRow> _customRows;

      bool _metadataExpanded = true;
      bool _customExpanded = true;
      bool _technicalExpanded = false;

      SectionHeaderRow _metadataHeader{"Metadata"};
      SectionHeaderRow _customHeader{"Custom Properties"};
      SectionHeaderRow _technicalHeader{"Audio Properties"};

      Gtk::Box _mainBox;
      CustomPropertyUndoBar _undoBar;

      Glib::RefPtr<Gtk::SizeGroup> _compositePrimarySizeGroupPtr;
      Glib::RefPtr<Gtk::SizeGroup> _compositeSecondarySizeGroupPtr;

      AddCustomPropertyRow _addPropertyRow{kGridColumnSpacing};

      ColumnWidthAnchor _keyColumnWidthAnchor{"ao-key-column-width-anchor"};
      ColumnWidthAnchor _valueColumnWidthAnchor{"ao-value-column-width-anchor"};

      sigc::connection _scopeConn;

      ConstrainedGridBox _wrapper;
      Gtk::ScrolledWindow _fieldScroll;
      FixedHeightWidgetSlot _fieldViewport{_fieldScroll, true, false, kGridViewportHeight, FixedHeightMinimum::Zero};
    };

    std::unique_ptr<ILayoutComponent> createTrackFieldGrid(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackFieldGridComponent>(ctx, node);
    }
  } // namespace

  void registerTrackFieldGridComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "track.fieldGrid",
       .displayName = "Field Grid",
       .category = ComponentCategory::Track,
       .props = {{.name = "categories", .kind = PropertyKind::StringList, .label = "Categories"}},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0},
      createTrackFieldGrid);
  }
} // namespace ao::gtk::layout
