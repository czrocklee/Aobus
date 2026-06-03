// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/component/track/TrackDetailSizing.h"
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

#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <glib.h>
#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/editablelabel.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>
#include <sigc++/functors/mem_fun.h>

#include <algorithm>
#include <cstddef>
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

    class TrackFieldGridComponent final : public ILayoutComponent
    {
    public:
      class ResponsiveGridBox final : public Gtk::Widget
      {
      public:
        explicit ResponsiveGridBox(TrackFieldGridComponent& owner)
          : _owner{owner}
        {
          set_overflow(Gtk::Overflow::HIDDEN);
        }

        ~ResponsiveGridBox() override
        {
          if (_gridPtr != nullptr)
          {
            _gridPtr->unparent();
          }
        }

        ResponsiveGridBox(ResponsiveGridBox const&) = delete;
        ResponsiveGridBox& operator=(ResponsiveGridBox const&) = delete;
        ResponsiveGridBox(ResponsiveGridBox&&) = delete;
        ResponsiveGridBox& operator=(ResponsiveGridBox&&) = delete;

        void setGrid(Gtk::Grid& grid)
        {
          _gridPtr = &grid;
          grid.set_parent(*this);
        }

      protected:
        Gtk::SizeRequestMode get_request_mode_vfunc() const override { return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH; }

        void measure_vfunc(Gtk::Orientation orientation,
                           int forSize,
                           int& minimum,
                           int& natural,
                           int& minimumBaseline,
                           int& naturalBaseline) const override
        {
          minimumBaseline = -1;
          naturalBaseline = -1;

          if (_gridPtr == nullptr)
          {
            minimum = 0;
            natural = 0;
            return;
          }

          if (orientation == Gtk::Orientation::HORIZONTAL)
          {
            minimum = 0;
            natural = 0;
            return;
          }

          auto const requestedWidth = forSize > 0 ? forSize : _lastAllocatedWidth;
          auto const width = std::max(requestedWidth, gridMinimumWidth());

          if (width > 0)
          {
            _gridPtr->measure(orientation, width, minimum, natural, minimumBaseline, naturalBaseline);
          }
          else
          {
            _gridPtr->measure(orientation, forSize, minimum, natural, minimumBaseline, naturalBaseline);
          }
        }

        void size_allocate_vfunc(int width, int height, int baseline) override
        {
          _lastAllocatedWidth = std::max(0, width);

          if (_gridPtr != nullptr)
          {
            auto const gridWidth = std::max(_lastAllocatedWidth, gridMinimumWidth());
            _gridPtr->size_allocate({0, 0, gridWidth, height}, baseline);
          }

          _owner.onResize(width);
        }

      private:
        std::int32_t gridMinimumWidth() const
        {
          if (_gridPtr == nullptr)
          {
            return 0;
          }

          auto minimum = 0;
          auto natural = 0;
          auto minimumBaseline = -1;
          auto naturalBaseline = -1;
          _gridPtr->measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);

          return std::max(0, minimum);
        }

        TrackFieldGridComponent& _owner;
        Gtk::Grid* _gridPtr = nullptr;
        std::int32_t _lastAllocatedWidth = 0;
      };

      TrackFieldGridComponent(LayoutContext& ctx, LayoutNode const& node)
        : _mutation{ctx.runtime.mutation()}
        , _scope{ctx.track.detailScope}
        , _addVBox{Gtk::Orientation::VERTICAL, 8}
        , _wrapper{*this}
      {
        _wrapper.setGrid(_grid);
        _grid.set_column_spacing(kGridColumnSpacing);
        _grid.set_row_spacing(8);
        _grid.set_valign(Gtk::Align::START);

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

          if (isMeta)
          {
            _metadataRows.emplace_back(def.field);
            setupBuiltInRow(_metadataRows.back());
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
          _lockConn = _scope->signalEditLockChanged().connect([this](bool locked) { onEditLockChanged(locked); });
          onSnapshot(_scope->snapshot());
          onEditLockChanged(_scope->isEditLocked());
        }
      }

      ~TrackFieldGridComponent() override
      {
        _layoutRebuildConn.disconnect();
        _addPopover.unparent();
      }

      TrackFieldGridComponent(TrackFieldGridComponent const&) = delete;
      TrackFieldGridComponent& operator=(TrackFieldGridComponent const&) = delete;
      TrackFieldGridComponent(TrackFieldGridComponent&&) = delete;
      TrackFieldGridComponent& operator=(TrackFieldGridComponent&&) = delete;

      Gtk::Widget& widget() override { return _wrapper; }

    private:
      struct ChildMeasure final
      {
        std::int32_t minimum = 0;
        std::int32_t natural = 0;
        std::int32_t minimumBaseline = -1;
        std::int32_t naturalBaseline = -1;
      };

      class CustomRowWidget final : public Gtk::Widget
      {
      public:
        CustomRowWidget(Gtk::Widget& label, Gtk::Widget& value, Gtk::Widget& partialIcon, Gtk::Widget& deleteButton)
          : _label{label}, _value{value}, _partialIcon{partialIcon}, _deleteButton{deleteButton}
        {
          set_overflow(Gtk::Overflow::HIDDEN);
          _label.set_parent(*this);
          _value.set_parent(*this);
          _partialIcon.set_parent(*this);
          _deleteButton.set_parent(*this);
        }

        ~CustomRowWidget() override
        {
          _deleteButton.unparent();
          _partialIcon.unparent();
          _value.unparent();
          _label.unparent();
        }

        CustomRowWidget(CustomRowWidget const&) = delete;
        CustomRowWidget& operator=(CustomRowWidget const&) = delete;
        CustomRowWidget(CustomRowWidget&&) = delete;
        CustomRowWidget& operator=(CustomRowWidget&&) = delete;

      protected:
        Gtk::SizeRequestMode get_request_mode_vfunc() const override { return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH; }

        void measure_vfunc(Gtk::Orientation orientation,
                           int forSize,
                           int& minimum,
                           int& natural,
                           int& minimumBaseline,
                           int& naturalBaseline) const override
        {
          minimumBaseline = -1;
          naturalBaseline = -1;

          if (orientation == Gtk::Orientation::HORIZONTAL)
          {
            minimum = 0;
            natural = 0;
            return;
          }

          auto const labelMeasure = measureChild(_label, Gtk::Orientation::VERTICAL, forSize);
          auto const valueMeasure = measureChild(_value, Gtk::Orientation::VERTICAL, forSize);
          auto const partialMeasure = measureChild(_partialIcon, Gtk::Orientation::VERTICAL, forSize);
          auto const deleteMeasure = measureChild(_deleteButton, Gtk::Orientation::VERTICAL, forSize);
          minimum =
            std::max({labelMeasure.minimum, valueMeasure.minimum, partialMeasure.minimum, deleteMeasure.minimum});
          natural =
            std::max({labelMeasure.natural, valueMeasure.natural, partialMeasure.natural, deleteMeasure.natural});
        }

        void size_allocate_vfunc(int width, int height, int baseline) override
        {
          auto right = std::max(0, width);
          allocateTrailingChild(_deleteButton, right, height, baseline);
          allocateTrailingChild(_partialIcon, right, height, baseline);

          auto const available = std::max(0, right);
          auto const labelMeasure = measureChild(_label, Gtk::Orientation::HORIZONTAL, -1);
          auto const valueSpacing = available > 0 ? kCustomRowSpacing : 0;
          auto const labelLimit = std::max(0, (available - valueSpacing) / 2);
          auto const labelWidth = std::min(labelMeasure.natural, labelLimit);
          auto const valueX = labelWidth + valueSpacing;
          auto const valueWidth = std::max(0, available - valueX);

          allocateChild(_label, 0, 0, labelWidth, height, baseline);
          allocateChild(_value, valueX, 0, valueWidth, height, baseline);
        }

      private:
        static ChildMeasure measureChild(Gtk::Widget& widget, Gtk::Orientation orientation, std::int32_t forSize)
        {
          auto result = ChildMeasure{};

          if (!widget.get_visible())
          {
            return result;
          }

          widget.measure(
            orientation, forSize, result.minimum, result.natural, result.minimumBaseline, result.naturalBaseline);
          return result;
        }

        static void allocateChild(Gtk::Widget& widget,
                                  std::int32_t const xPos,
                                  std::int32_t const yPos,
                                  std::int32_t const width,
                                  std::int32_t const height,
                                  std::int32_t const baseline)
        {
          if (!widget.get_visible())
          {
            return;
          }

          auto minimum = 0;
          auto natural = 0;
          auto minimumBaseline = -1;
          auto naturalBaseline = -1;
          widget.measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);
          widget.measure(Gtk::Orientation::VERTICAL, width, minimum, natural, minimumBaseline, naturalBaseline);
          widget.size_allocate({xPos, yPos, width, height}, baseline);
        }

        static void allocateTrailingChild(Gtk::Widget& widget,
                                          std::int32_t& right,
                                          std::int32_t const height,
                                          std::int32_t const baseline)
        {
          if (!widget.get_visible())
          {
            return;
          }

          auto const measure = measureChild(widget, Gtk::Orientation::HORIZONTAL, -1);
          auto const childWidth = std::min(measure.natural, std::max(0, right));
          auto const xPos = std::max(0, right - childWidth);
          allocateChild(widget, xPos, 0, childWidth, height, baseline);
          right = std::max(0, xPos - kCustomRowSpacing);
        }

        Gtk::Widget& _label;
        Gtk::Widget& _value;
        Gtk::Widget& _partialIcon;
        Gtk::Widget& _deleteButton;

        static constexpr std::int32_t kCustomRowSpacing = 8;
      };

      class ValueClipWidget final : public Gtk::Widget
      {
      public:
        explicit ValueClipWidget(Gtk::Widget& child)
          : _child{child}
        {
          set_halign(Gtk::Align::FILL);
          set_hexpand(true);
          set_overflow(Gtk::Overflow::HIDDEN);
          set_size_request(0, -1);
          _child.set_parent(*this);
        }

        ~ValueClipWidget() override { _child.unparent(); }

        ValueClipWidget(ValueClipWidget const&) = delete;
        ValueClipWidget& operator=(ValueClipWidget const&) = delete;
        ValueClipWidget(ValueClipWidget&&) = delete;
        ValueClipWidget& operator=(ValueClipWidget&&) = delete;

      protected:
        Gtk::SizeRequestMode get_request_mode_vfunc() const override { return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH; }

        void measure_vfunc(Gtk::Orientation orientation,
                           int forSize,
                           int& minimum,
                           int& natural,
                           int& minimumBaseline,
                           int& naturalBaseline) const override
        {
          minimumBaseline = -1;
          naturalBaseline = -1;

          if (orientation == Gtk::Orientation::HORIZONTAL)
          {
            minimum = 0;
            natural = 0;
            return;
          }

          auto const childWidth = childWidthFor(std::max(0, forSize));
          _child.measure(orientation, childWidth, minimum, natural, minimumBaseline, naturalBaseline);
        }

        void size_allocate_vfunc(int width, int height, int baseline) override
        {
          auto const childWidth = childWidthFor(width);
          measureChildForAllocation(childWidth);
          _child.size_allocate({0, 0, childWidth, height}, baseline);
        }

      private:
        std::int32_t childWidthFor(std::int32_t const width) const
        {
          auto minimum = 0;
          auto natural = 0;
          auto minimumBaseline = -1;
          auto naturalBaseline = -1;
          _child.measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);

          return std::max({0, width, minimum});
        }

        void measureChildForAllocation(int const width) const
        {
          auto minimum = 0;
          auto natural = 0;
          auto minimumBaseline = -1;
          auto naturalBaseline = -1;
          _child.measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);
          _child.measure(Gtk::Orientation::VERTICAL, width, minimum, natural, minimumBaseline, naturalBaseline);
        }

        Gtk::Widget& _child;
      };

      struct BuiltInRow final
      {
        rt::TrackField field;
        Gtk::Label label{};
        Gtk::Box valueBox{Gtk::Orientation::HORIZONTAL, 0};
        Gtk::EditableLabel valueEditable{};
        ValueClipWidget valueClip;
        bool editable = false;
        bool discardNextEdit = false;

        explicit BuiltInRow(rt::TrackField field)
          : field{field}, valueClip{valueEditable}
        {
        }
      };

      struct CustomRow final
      {
        std::string key;
        Gtk::Label label{};
        Gtk::Box valueBox{Gtk::Orientation::HORIZONTAL, 0};
        Gtk::EditableLabel editable{};
        ValueClipWidget valueClip;
        Gtk::Image partialIcon{};
        Gtk::Button deleteButton{};
        CustomRowWidget widget;
        bool discardNextEdit = false;

        explicit CustomRow(std::string key)
          : key{std::move(key)}, valueClip{editable}, widget{label, valueBox, partialIcon, deleteButton}
        {
        }
      };

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

        updateCustomRows(snap);
      }

      void onEditLockChanged(bool locked)
      {
        _editLocked = locked;

        for (auto& row : _metadataRows)
        {
          auto const active = !locked && row.editable;
          row.valueEditable.set_editable(active);
          setEditableAffordance(row.valueEditable, active);

          if (locked && row.valueEditable.get_editing())
          {
            row.valueEditable.stop_editing(true);
          }
        }

        for (auto& row : _technicalRows)
        {
          auto const active = !locked && row.editable;
          row.valueEditable.set_editable(active);
          setEditableAffordance(row.valueEditable, active);

          if (locked && row.valueEditable.get_editing())
          {
            row.valueEditable.stop_editing(true);
          }
        }

        for (auto& row : _customRows)
        {
          auto const active = !locked;
          row.editable.set_editable(active);
          setEditableAffordance(row.editable, active);

          if (locked && row.editable.get_editing())
          {
            row.editable.stop_editing(true);
          }

          row.deleteButton.set_sensitive(!locked);
        }

        _addBtn.set_sensitive(!locked);
        _addBtn.set_visible(!locked);
        _addLabel.set_visible(!locked);
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

        if (auto const* uiDef = trackFieldUiDefinition(row.field);
            uiDef != nullptr && uiDef->parseInlineEdit != nullptr && uiDef->writePatch != nullptr)
        {
          row.editable = true;
          row.valueEditable.add_css_class("ao-property-editable");
          row.valueEditable.property_editing().signal_changed().connect([this, field = row.field]
                                                                        { onBuiltInEditingChanged(field); });

          auto const keyPtr = Gtk::EventControllerKey::create();
          keyPtr->signal_key_pressed().connect(
            [this, field = row.field](guint keyval, guint, Gdk::ModifierType) -> bool
            {
              if (keyval == GDK_KEY_Escape)
              {
                if (auto* rowPtr = findBuiltInRow(field); rowPtr != nullptr)
                {
                  rowPtr->discardNextEdit = true;
                  rowPtr->valueEditable.stop_editing(false);

                  if (_scope != nullptr)
                  {
                    updateBuiltInRow(*rowPtr, _scope->snapshot());
                  }
                }

                return true;
              }

              return false;
            },
            false);
          row.valueEditable.add_controller(keyPtr);
        }
      }

      void updateBuiltInRow(BuiltInRow& row, rt::TrackDetailSnapshot const& snap)
      {
        auto const& agg = rt::trackFieldArrayAt(snap.fields, row.field);
        auto const* uiDef = trackFieldUiDefinition(row.field);
        auto const* def = rt::trackFieldDefinition(row.field);

        if (row.valueEditable.get_editing())
        {
          return;
        }

        auto text = std::string{};

        if (agg.mixed)
        {
          text = "<Multiple Values>";
        }
        else if (!agg.optValue)
        {
          text = (def != nullptr && def->category == rt::TrackFieldCategory::Technical) ? "Unknown" : "";
        }
        else if (uiDef != nullptr && uiDef->formatValue != nullptr)
        {
          text = uiDef->formatValue(*agg.optValue);
        }

        auto const displayText = validUtf8Text(text);
        row.valueEditable.set_text(displayText);
        row.valueEditable.set_tooltip_text(displayText);
      }

      void onBuiltInEditingChanged(rt::TrackField field)
      {
        auto* row = findBuiltInRow(field);

        if (row == nullptr || !row->editable || row->valueEditable.get_editing())
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

      void onBuiltInEdited(rt::TrackField field)
      {
        auto* row = findBuiltInRow(field);

        if (row == nullptr || !row->editable || row->valueEditable.get_editing() || _scope == nullptr)
        {
          return;
        }

        auto const snap = _scope->snapshot();

        if (snap.trackIds.empty())
        {
          return;
        }

        auto const newValue = row->valueEditable.get_text().raw();

        if (newValue == "<Multiple Values>")
        {
          return;
        }

        auto const* uiDef = trackFieldUiDefinition(field);

        if (uiDef == nullptr || uiDef->parseInlineEdit == nullptr || uiDef->writePatch == nullptr)
        {
          return;
        }

        auto const editValResult = uiDef->parseInlineEdit(newValue);

        if (!editValResult)
        {
          APP_LOG_ERROR(
            "Failed to parse edit value for {}: {}", rt::trackFieldId(field), editValResult.error().message);
          updateBuiltInRow(*row, snap);
          return;
        }

        auto patch = rt::MetadataPatch{};
        uiDef->writePatch({.patch = patch, .value = *editValResult});

        auto const result = _mutation.updateMetadata(snap.trackIds, patch);

        if (!result)
        {
          APP_LOG_ERROR("Failed to update {}: {}", rt::trackFieldId(field), result.error().message);
          updateBuiltInRow(*row, snap);
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
            _customRows.emplace_back(item.key);
            setupCustomRow(_customRows.back());
          }

          buildGrid();
          onEditLockChanged(_scope != nullptr ? _scope->isEditLocked() : true);
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
        row.editable.property_editing().signal_changed().connect([this, key = row.key]
                                                                 { onCustomEditingChanged(key); });

        auto const keyPtr = Gtk::EventControllerKey::create();
        keyPtr->signal_key_pressed().connect(
          [this, key = row.key](guint keyval, guint, Gdk::ModifierType) -> bool
          {
            if (keyval == GDK_KEY_Escape)
            {
              if (auto* rowPtr = findCustomRow(key); rowPtr != nullptr)
              {
                rowPtr->discardNextEdit = true;
                rowPtr->editable.stop_editing(false);

                if (_scope != nullptr)
                {
                  updateCustomRows(_scope->snapshot());
                }
              }

              return true;
            }

            return false;
          },
          false);
        row.editable.add_controller(keyPtr);
        row.valueBox.append(row.valueClip);

        row.deleteButton.set_icon_name("user-trash-symbolic");
        row.deleteButton.set_has_frame(false);
        row.deleteButton.add_css_class("ao-icon-button");
        row.deleteButton.set_tooltip_text("Delete Property");
        row.deleteButton.signal_clicked().connect([this, key = row.key] { onCustomDeleted(key); });

        row.partialIcon.set_from_icon_name("dialog-warning-symbolic");
        row.partialIcon.set_opacity(kLabelOpacity);
        row.partialIcon.set_tooltip_text("Missing on some tracks");
      }

      void updateCustomRow(CustomRow& row, rt::CustomMetadataItem const& item)
      {
        if (row.editable.get_editing())
        {
          return;
        }

        auto text = std::string{};

        if (item.value.mixed)
        {
          text = "<Multiple Values>";
        }
        else
        {
          text = item.value.optValue.value_or("");
        }

        auto const displayText = validUtf8Text(text);
        row.editable.set_text(displayText);
        row.editable.set_tooltip_text(displayText);
        row.partialIcon.set_visible(!item.presentOnAll);
      }

      void onCustomEditingChanged(std::string const& key)
      {
        auto* row = findCustomRow(key);

        if (row == nullptr || row->editable.get_editing())
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

        if (row == nullptr || row->editable.get_editing() || _scope == nullptr)
        {
          return;
        }

        auto const snap = _scope->snapshot();
        auto const newValue = row->editable.get_text().raw();

        if (newValue == "<Multiple Values>")
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

        auto patch = rt::MetadataPatch{};
        patch.customUpdates[key] = std::nullopt;

        auto const result = _mutation.updateMetadata(snap.trackIds, patch);

        if (!result)
        {
          APP_LOG_ERROR("Failed to delete custom property {}: {}", key, result.error().message);
        }
      }

      void buildGrid()
      {
        clearGrid();
        std::int32_t rowIdx = 0;

        attachBuiltInGroup(_metadataRows, rowIdx);
        attachCustomRows(rowIdx);
        attachAddPropertyRow(rowIdx);
        attachTechnicalSeparator(rowIdx);
        attachBuiltInGroup(_technicalRows, rowIdx);
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
        _grid.attach(row.label, 0, rowNum, 1, 1);
        _grid.attach(row.valueBox, 1, rowNum, kValueColWidth, 1);
      }

      void attachBuiltInRowsWide(std::deque<BuiltInRow>& rows, std::int32_t& rowIdx)
      {
        std::size_t index = 0;

        for (auto& row : rows)
        {
          auto const col = static_cast<std::int32_t>((index % 2) * 2);
          auto const rowNum = rowIdx + static_cast<std::int32_t>(index / 2);
          _grid.attach(row.label, col, rowNum, 1, 1);
          _grid.attach(row.valueBox, col + 1, rowNum, 1, 1);
          index++;
        }

        rowIdx += static_cast<std::int32_t>((rows.size() + 1) / 2);
      }

      void attachBuiltInGroup(std::deque<BuiltInRow>& rows, std::int32_t& rowIdx)
      {
        if (_layoutMode == LayoutMode::Wide)
        {
          attachBuiltInRowsWide(rows, rowIdx);
          return;
        }

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
          _grid.attach(row.widget, 0, rowIdx, 4, 1);
          rowIdx++;
        }
      }

      void attachAddPropertyRow(std::int32_t& rowIdx)
      {
        _grid.attach(_addLabel, 0, rowIdx, kValueColWidth, 1);
        _grid.attach(_addBtn, kActionColumn, rowIdx, 1, 1);
        rowIdx++;
      }

      void attachTechnicalSeparator(std::int32_t& rowIdx)
      {
        if (_technicalRows.empty() || (_metadataRows.empty() && _customRows.empty()))
        {
          return;
        }

        auto* sep = Gtk::make_managed<Gtk::Separator>();
        sep->set_margin_top(4);
        sep->set_margin_bottom(4);
        _grid.attach(*sep, 0, rowIdx, 4, 1);
        rowIdx++;
      }

      void configureValueBox(Gtk::Box& box)
      {
        box.set_halign(Gtk::Align::FILL);
        box.set_hexpand(true);
        box.set_overflow(Gtk::Overflow::HIDDEN);
        box.set_size_request(0, -1);
      }

      void configureKeyLabel(Gtk::Label& label)
      {
        label.set_halign(Gtk::Align::START);
        label.set_xalign(0.0F);
        label.set_hexpand(false);
        label.set_overflow(Gtk::Overflow::HIDDEN);
        label.set_size_request(0, -1);
        label.set_ellipsize(Pango::EllipsizeMode::NONE);
        label.set_wrap(false);
        label.set_lines(1);
      }

      void configureValueEditable(Gtk::EditableLabel& editable)
      {
        editable.set_halign(Gtk::Align::FILL);
        editable.set_hexpand(true);
        editable.set_overflow(Gtk::Overflow::HIDDEN);
        editable.set_size_request(0, -1);
        editable.set_width_chars(0);
        editable.set_max_width_chars(1);
        editable.set_editable(false);
      }

      static void setEditableAffordance(Gtk::EditableLabel& editable, bool const active)
      {
        if (active)
        {
          editable.add_css_class("ao-property-editable-active");
          return;
        }

        editable.remove_css_class("ao-property-editable-active");
      }

      void setupAddPropertyUi()
      {
        _addLabel.set_text("Add Property");
        configureKeyLabel(_addLabel);
        _addLabel.set_opacity(kLabelOpacity);
        _addLabel.add_css_class("ao-property-label");

        _addBtn.set_icon_name("list-add-symbolic");
        _addBtn.set_halign(Gtk::Align::CENTER);
        _addBtn.set_has_frame(false);
        _addBtn.add_css_class("ao-icon-button");
        _addBtn.set_tooltip_text("Add Custom Property");
        _addBtn.signal_clicked().connect(sigc::mem_fun(_addPopover, &Gtk::Popover::popup));

        _addVBox.set_margin(8);

        _addKeyEntry.set_placeholder_text("Property Name");
        _addVBox.append(_addKeyEntry);

        _addValEntry.set_placeholder_text("Value");
        _addVBox.append(_addValEntry);

        _addHintLabel.set_halign(Gtk::Align::START);
        _addHintLabel.set_opacity(kLabelOpacity);
        _addHintLabel.add_css_class("ao-property-hint");
        _addHintLabel.set_visible(false);
        _addVBox.append(_addHintLabel);

        _addSubmitBtn.set_label("Add");
        _addSubmitBtn.add_css_class("suggested-action");
        _addVBox.append(_addSubmitBtn);

        _addPopover.set_child(_addVBox);
        _addPopover.set_parent(_addBtn);

        auto const onAdd = [this]
        {
          auto key = std::string{_addKeyEntry.get_text().raw()};
          auto val = std::string{_addValEntry.get_text().raw()};

          // Trim key
          key.erase(0, key.find_first_not_of(" \t\n\r\f\v"));
          key.erase(key.find_last_not_of(" \t\n\r\f\v") + 1);

          if (key.empty() || _scope == nullptr)
          {
            return;
          }

          auto const& snap = _scope->snapshot();

          for (auto const& item : snap.customMetadata)
          {
            if (item.key == key)
            {
              _addHintLabel.set_text("Property already exists");
              _addHintLabel.set_visible(true);
              return;
            }
          }

          if (auto const optField = rt::trackFieldFromId(key); optField)
          {
            _addHintLabel.set_text("Reserved for built-in field");
            _addHintLabel.set_visible(true);
            return;
          }

          auto patch = rt::MetadataPatch{};
          patch.customUpdates[key] = val;
          auto const result = _mutation.updateMetadata(snap.trackIds, patch);

          if (!result)
          {
            APP_LOG_ERROR("Failed to add custom property {}: {}", key, result.error().message);
            return;
          }

          _addPopover.popdown();
          _addKeyEntry.set_text("");
          _addValEntry.set_text("");
          _addHintLabel.set_visible(false);
        };

        _addSubmitBtn.signal_clicked().connect(onAdd);
        _addKeyEntry.signal_activate().connect(onAdd);
        _addValEntry.signal_activate().connect(onAdd);

        _addKeyEntry.property_text().signal_changed().connect([this] { _addHintLabel.set_visible(false); });
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

      void onResize(int const width)
      {
        if (auto const nextMode = computeLayoutMode(width); nextMode != _layoutMode)
        {
          _layoutMode = nextMode;
          scheduleRebuild();
        }
      }

      void scheduleRebuild()
      {
        if (_layoutRebuildPending)
        {
          return;
        }

        _layoutRebuildPending = true;
        _layoutRebuildConn = Glib::signal_idle().connect(
          [this]
          {
            _layoutRebuildPending = false;
            buildGrid();
            return false;
          });
      }

      static constexpr std::int32_t kGridColumnSpacing = 12;
      static constexpr std::int32_t kValueColWidth = 3;
      static constexpr std::int32_t kActionColumn = 3;

      Gtk::Grid _grid;
      rt::LibraryMutationService& _mutation;
      ITrackDetailScope* _scope;
      std::deque<BuiltInRow> _metadataRows;
      std::deque<BuiltInRow> _technicalRows;
      std::deque<CustomRow> _customRows;

      Gtk::Label _addLabel;
      Gtk::Button _addBtn;
      Gtk::Popover _addPopover;
      Gtk::Box _addVBox;
      Gtk::Entry _addKeyEntry;
      Gtk::Entry _addValEntry;
      Gtk::Label _addHintLabel;
      Gtk::Button _addSubmitBtn;

      sigc::connection _scopeConn;
      sigc::connection _lockConn;

      LayoutMode _layoutMode = LayoutMode::Standard;
      bool _editLocked = true;
      bool _layoutRebuildPending = false;
      sigc::connection _layoutRebuildConn;
      ResponsiveGridBox _wrapper;
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
