// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/component/track/TrackFieldGridCustomControls.h"
#include "layout/component/track/TrackFieldGridRows.h"
#include "layout/component/track/TrackFieldGridWidgets.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "track/TrackFieldUi.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/utility/Log.h>

#include <glib.h>
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
  namespace
  {
    constexpr float kLabelOpacity = 0.6F;
    constexpr std::string_view kMultipleValuesText = "<Multiple Values>";
    constexpr std::string_view kCompositeMixedText = "-";

    std::string validUtf8Text(std::string_view text)
    {
      if (text.empty())
      {
        return {};
      }

      if (::g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr) != 0)
      {
        return std::string{text};
      }

      auto validPtr = Glib::make_unique_ptr_gfree(::g_utf8_make_valid(text.data(), static_cast<gssize>(text.size())));

      if (!validPtr)
      {
        return {};
      }

      return std::string{validPtr.get()};
    }

    std::string displayTextForField(rt::TrackField field,
                                    rt::TrackDetailSnapshot const& snap,
                                    std::string_view mixedText,
                                    bool showTechnicalUnknown)
    {
      auto const& agg = rt::trackFieldArrayAt(snap.fields, field);
      auto const* uiDef = trackFieldUiDefinition(field);
      auto const* def = rt::trackFieldDefinition(field);

      if (agg.mixed)
      {
        return std::string{mixedText};
      }

      if (!agg.optValue)
      {
        if (showTechnicalUnknown && def != nullptr && def->category == rt::TrackFieldCategory::Technical)
        {
          return "Unknown";
        }

        return {};
      }

      if (uiDef != nullptr && uiDef->formatValue != nullptr)
      {
        return uiDef->formatValue(*agg.optValue);
      }

      return {};
    }

    bool isProtectedFieldEditValue(rt::TrackField field,
                                   rt::TrackDetailSnapshot const& snap,
                                   std::string_view newValue,
                                   bool protectCompositeMixedText)
    {
      if (newValue == kMultipleValuesText)
      {
        return true;
      }

      auto const& agg = rt::trackFieldArrayAt(snap.fields, field);
      return protectCompositeMixedText && agg.mixed && newValue == kCompositeMixedText;
    }

    using track_field_grid::AddCustomPropertyRow;
    using track_field_grid::BuiltInRow;
    using track_field_grid::CompositeBuiltInRow;
    using track_field_grid::ConstrainedGridBox;
    using track_field_grid::CustomPropertyUndoBar;
    using track_field_grid::CustomRow;
    using track_field_grid::FieldInlineEditor;
    using track_field_grid::FixedHeightMinimum;
    using track_field_grid::FixedHeightWidgetSlot;
    using track_field_grid::SeparatorRow;

    class TrackFieldGridComponent final : public ILayoutComponent
    {
    public:
      TrackFieldGridComponent(LayoutContext& ctx, LayoutNode const& node)
        : _mutation{ctx.runtime.mutation()}
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

        _wrapper.setGrid(_grid);
        _wrapper.set_vexpand(true);
        _grid.set_column_spacing(kGridColumnSpacing);
        _grid.set_row_spacing(8);
        _grid.set_valign(Gtk::Align::START);
        _grid.set_vexpand(true);

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

          if (def.field == rt::TrackField::TotalDiscs || def.field == rt::TrackField::TotalTracks)
          {
            continue; // Handled by composite rows
          }

          if (isMeta)
          {
            if (def.field == rt::TrackField::DiscNumber)
            {
              _compositeRows.emplace_back(rt::TrackField::DiscNumber, rt::TrackField::TotalDiscs);
              setupCompositeRow(_compositeRows.back());
            }
            else if (def.field == rt::TrackField::TrackNumber)
            {
              _compositeRows.emplace_back(rt::TrackField::TrackNumber, rt::TrackField::TotalTracks);
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
            setupBuiltInRow(_technicalRows.back());
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

      void onUndo()
      {
        auto optPendingUndo = _undoBar.takePendingUndo();

        if (!optPendingUndo)
        {
          return;
        }

        auto patch = rt::MetadataPatch{};
        patch.customUpdates[optPendingUndo->key] = optPendingUndo->value;

        auto const result = _mutation.updateMetadata(optPendingUndo->trackIds, patch);

        if (!result)
        {
          APP_LOG_ERROR("Failed to undo property deletion: {}", result.error().message);
        }
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

      void setupBuiltInRow(BuiltInRow& row)
      {
        auto const* def = rt::trackFieldDefinition(row.field);
        row.label.set_text(std::string{def != nullptr ? def->label : ""});
        configureKeyLabel(row.label);

        row.label.set_opacity(kLabelOpacity);
        row.label.add_css_class("ao-property-label");

        configureValueBox(row.valueBox);
        configureValueEditable(row.valueEditable);
        row.valueEditable.add_css_class("ao-property-value");
        row.valueBox.append(row.valueClip);

        if (row.editable)
        {
          row.valueEditable.add_css_class("ao-property-editable");
          row.valueEditable.setEditable(true);
          row.valueEditable.signalEditingChanged().connect([this, field = row.field]
                                                           { onBuiltInEditingChanged(field); });
          row.valueEditable.signalEditingCanceled().connect(
            [this, field = row.field]
            {
              if (auto* row = findBuiltInRow(field); row != nullptr)
              {
                row->discardNextEdit = true;

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

        configureValueEditable(row.primaryEditable);
        row.primaryEditable.add_css_class("ao-property-value");
        row.primaryEditable.removeMaxWidthConstraint();
        row.primaryClip.set_hexpand(false);
        row.primaryClip.set_halign(Gtk::Align::START);
        _compositePrimarySizeGroupPtr->add_widget(row.primaryClip);

        configureValueEditable(row.secondaryEditable);
        row.secondaryEditable.add_css_class("ao-property-value");
        row.secondaryEditable.removeMaxWidthConstraint();
        row.secondaryClip.set_hexpand(false);
        row.secondaryClip.set_halign(Gtk::Align::START);
        _compositeSecondarySizeGroupPtr->add_widget(row.secondaryClip);

        auto bindEditor = [this](FieldInlineEditor& editable, bool isEditable, rt::TrackField field, bool& discardFlag)
        {
          if (isEditable)
          {
            editable.add_css_class("ao-property-editable");
            editable.setEditable(true);
            editable.signalEditingChanged().connect([this, field] { onCompositeEditingChanged(field); });
            editable.signalEditingCanceled().connect(
              [this, field, &discardFlag]
              {
                if (auto* row = findCompositeBuiltInRow(field); row != nullptr)
                {
                  discardFlag = true;

                  if (_scope != nullptr)
                  {
                    updateCompositeRow(*row, _scope->snapshot());
                  }
                }
              });
          }
        };

        bindEditor(row.primaryEditable, row.primaryEditableFlag, row.primaryField, row.discardNextEditPrimary);
        bindEditor(row.secondaryEditable, row.secondaryEditableFlag, row.secondaryField, row.discardNextEditSecondary);
      }

      void updateBuiltInRow(BuiltInRow& row, rt::TrackDetailSnapshot const& snap)
      {
        if (row.valueEditable.getEditing())
        {
          return;
        }

        auto const displayText = validUtf8Text(displayTextForField(row.field, snap, kMultipleValuesText, true));
        row.valueEditable.setText(displayText);
        row.valueEditable.set_tooltip_text(displayText);
      }

      void updateCompositeRow(CompositeBuiltInRow& row, rt::TrackDetailSnapshot const& snap)
      {
        auto updateField = [](FieldInlineEditor& editable, rt::TrackField field, rt::TrackDetailSnapshot const& snap)
        {
          if (editable.getEditing())
          {
            return;
          }

          auto const displayText = validUtf8Text(displayTextForField(field, snap, kCompositeMixedText, false));
          editable.setText(displayText);
          editable.set_tooltip_text(displayText);
        };

        updateField(row.primaryEditable, row.primaryField, snap);
        updateField(row.secondaryEditable, row.secondaryField, snap);
      }

      void onBuiltInEditingChanged(rt::TrackField field)
      {
        auto* row = findBuiltInRow(field);

        if (row == nullptr || !row->editable || row->valueEditable.getEditing())
        {
          return;
        }

        auto const discardEdit = row->discardNextEdit;
        row->discardNextEdit = false;

        if (discardEdit)
        {
          return;
        }

        onBuiltInEdited(field);
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

        auto const result = _mutation.updateMetadata(snap.trackIds, patch);

        if (!result)
        {
          APP_LOG_ERROR("Failed to update {}: {}", rt::trackFieldId(field), result.error().message);
          return false;
        }

        return true;
      }

      void onBuiltInEdited(rt::TrackField field)
      {
        auto* row = findBuiltInRow(field);

        if (row == nullptr || !row->editable || row->valueEditable.getEditing() || _scope == nullptr)
        {
          return;
        }

        auto const snap = _scope->snapshot();

        if (snap.trackIds.empty())
        {
          return;
        }

        auto const newValue = row->valueEditable.getText().raw();

        if (isProtectedFieldEditValue(field, snap, newValue, false))
        {
          return;
        }

        if (!applyFieldEdit(field, newValue, snap))
        {
          updateBuiltInRow(*row, snap);
        }
      }

      void onCompositeEditingChanged(rt::TrackField field)
      {
        auto* row = findCompositeBuiltInRow(field);

        if (row == nullptr)
        {
          return;
        }

        bool const isPrimary = (row->primaryField == field);
        bool const editable = isPrimary ? row->primaryEditableFlag : row->secondaryEditableFlag;
        auto& editor = isPrimary ? row->primaryEditable : row->secondaryEditable;
        auto& discardFlag = isPrimary ? row->discardNextEditPrimary : row->discardNextEditSecondary;

        if (!editable || editor.getEditing())
        {
          return;
        }

        auto const discardEdit = discardFlag;
        discardFlag = false;

        if (discardEdit || _scope == nullptr)
        {
          return;
        }

        onCompositeEdited(field);
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
        auto& editor = isPrimary ? row->primaryEditable : row->secondaryEditable;

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
          for (auto i = 0U; i < snap.customMetadata.size(); ++i)
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
          _customRows.clear();

          for (auto const& item : snap.customMetadata)
          {
            _customRows.emplace_back(item.key, kGridColumnSpacing);
            setupCustomRow(_customRows.back());
          }

          buildGrid();
        }

        for (auto i = 0U; i < snap.customMetadata.size(); ++i)
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
        configureValueEditable(row.editable);
        row.editable.add_css_class("ao-property-value");
        row.editable.add_css_class("ao-property-editable");
        row.editable.setEditable(true);
        row.editable.signalEditingChanged().connect([this, key = row.key] { onCustomEditingChanged(key); });
        row.editable.signalEditingCanceled().connect(
          [this, key = row.key]
          {
            if (auto* row = findCustomRow(key); row != nullptr)
            {
              row->discardNextEdit = true;

              if (_scope != nullptr)
              {
                updateCustomRows(_scope->snapshot());
              }
            }
          });

        row.valueBox.append(row.valueClip);
        row.valueBox.append(row.partialIcon);

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
        if (row.editable.getEditing())
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
        row.editable.setText(displayText);
        row.editable.set_tooltip_text(displayText);
        row.partialIcon.set_visible(!item.presentOnAll);
      }

      void onCustomEditingChanged(std::string const& key)
      {
        auto* row = findCustomRow(key);

        if (row == nullptr || row->editable.getEditing())
        {
          return;
        }

        auto const discardEdit = row->discardNextEdit;
        row->discardNextEdit = false;

        if (discardEdit)
        {
          return;
        }

        onCustomEdited(key);
      }

      void onCustomEdited(std::string const& key)
      {
        auto* row = findCustomRow(key);

        if (row == nullptr || row->editable.getEditing() || _scope == nullptr)
        {
          return;
        }

        auto const snap = _scope->snapshot();
        auto const newValue = row->editable.getText().raw();

        if (newValue == kMultipleValuesText)
        {
          return;
        }

        auto patch = rt::MetadataPatch{};
        patch.customUpdates[key] = newValue;

        auto const result = _mutation.updateMetadata(snap.trackIds, patch);

        if (!result)
        {
          APP_LOG_ERROR("Failed to update custom property {}: {}", key, result.error().message);
        }
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

        auto const result = _mutation.updateMetadata(snap.trackIds, patch);

        if (result && optPrevValue)
        {
          _undoBar.show(key, snap.trackIds, *optPrevValue);
        }
        else if (!result)
        {
          APP_LOG_ERROR("Failed to delete custom property {}: {}", key, result.error().message);
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
        auto const result = _mutation.updateMetadata(snap.trackIds, patch);

        if (!result)
        {
          APP_LOG_ERROR("Failed to add custom property {}: {}", key, result.error().message);
          return;
        }

        _addPropertyRow.clearInputs();
      }

      void buildGrid()
      {
        clearGrid();
        std::int32_t rowIdx = 0;

        attachBuiltInGroup(_metadataRows, rowIdx);

        for (auto& row : _compositeRows)
        {
          attachCompositeRow(row, rowIdx);
          rowIdx++;
        }

        bool const hasMetadata = !_metadataRows.empty() || !_compositeRows.empty();
        attachSeparatorIf(hasMetadata, rowIdx);

        attachCustomRows(rowIdx);
        attachAddPropertyRow(rowIdx);

        bool const hasTechnical = !_technicalRows.empty();
        attachSeparatorIf(hasTechnical && (hasMetadata || !_customRows.empty()), rowIdx);

        attachBuiltInGroup(_technicalRows, rowIdx);
      }

      void clearGrid()
      {
        while (auto* child = _grid.get_first_child())
        {
          _grid.remove(*child);
        }

        _separatorRows.clear();
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

      void attachBuiltInGroup(std::deque<BuiltInRow>& rows, std::int32_t& rowIdx)
      {
        for (auto& row : rows)
        {
          attachBuiltInRow(row, rowIdx);
          rowIdx++;
        }
      }

      void attachCustomRows(std::int32_t& rowIdx)
      {
        for (auto& row : _customRows)
        {
          _grid.attach(row.labelSlot, 0, rowIdx, 1, 1);
          _grid.attach(row.valueSlot, 1, rowIdx, kValueColWidth, 1);
          rowIdx++;
        }
      }

      void attachAddPropertyRow(std::int32_t& rowIdx)
      {
        configureValueBox(_addPropertyRow.valueSlot());
        _grid.attach(_addPropertyRow.keySlot(), 0, rowIdx, 1, 1);
        _grid.attach(_addPropertyRow.valueSlot(), 1, rowIdx, kValueColWidth, 1);
        rowIdx++;
      }

      void attachSeparatorIf(bool condition, std::int32_t& rowIdx)
      {
        if (!condition)
        {
          return;
        }

        _separatorRows.emplace_back();
        _grid.attach(_separatorRows.back().separator, 0, rowIdx, 1 + kValueColWidth, 1);
        rowIdx++;
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

      void configureValueEditable(FieldInlineEditor& editable)
      {
        editable.set_halign(Gtk::Align::FILL);
        editable.set_hexpand(true);
        editable.set_overflow(Gtk::Overflow::HIDDEN);
        editable.set_size_request(0, -1);
        editable.setEditable(false);
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
      rt::LibraryMutationService& _mutation;
      ITrackDetailScope* _scope;
      std::deque<BuiltInRow> _metadataRows;
      std::deque<CompositeBuiltInRow> _compositeRows;
      std::deque<BuiltInRow> _technicalRows;
      std::deque<CustomRow> _customRows;
      std::deque<SeparatorRow> _separatorRows;

      Gtk::Box _mainBox;
      CustomPropertyUndoBar _undoBar;

      Glib::RefPtr<Gtk::SizeGroup> _compositePrimarySizeGroupPtr;
      Glib::RefPtr<Gtk::SizeGroup> _compositeSecondarySizeGroupPtr;

      AddCustomPropertyRow _addPropertyRow{kGridColumnSpacing};

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
       .category = "Tracks",
       .container = false,
       .props = {{.name = "categories", .kind = PropertyKind::StringList, .label = "Categories"}},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0},
      createTrackFieldGrid);
  }
} // namespace ao::gtk::layout
