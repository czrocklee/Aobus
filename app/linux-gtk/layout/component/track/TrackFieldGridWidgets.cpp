// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackFieldGridWidgets.h"

#include "sigc++/signal.h"

#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/gestureclick.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <cstdint>

namespace ao::gtk::layout::track_field_grid
{
  namespace
  {
    struct MeasureResult final
    {
      std::int32_t minimum = 0;
      std::int32_t natural = 0;
    };

    MeasureResult measureWidget(Gtk::Widget const& widget, Gtk::Orientation orientation, std::int32_t forSize)
    {
      auto result = MeasureResult{};
      auto minimumBaseline = -1;
      auto naturalBaseline = -1;
      widget.measure(orientation, forSize, result.minimum, result.natural, minimumBaseline, naturalBaseline);
      return result;
    }

    std::int32_t minimumWidth(Gtk::Widget const& widget)
    {
      return std::max(0, measureWidget(widget, Gtk::Orientation::HORIZONTAL, -1).minimum);
    }

    std::int32_t naturalWidth(Gtk::Widget const& widget)
    {
      return std::max(0, measureWidget(widget, Gtk::Orientation::HORIZONTAL, -1).natural);
    }

    std::int32_t widthAtLeastMinimum(Gtk::Widget const& widget, std::int32_t const width)
    {
      return std::max({0, width, minimumWidth(widget)});
    }

    std::int32_t heightAtLeastMinimum(Gtk::Widget const& widget, std::int32_t const width, std::int32_t const height)
    {
      auto const childMeasure = measureWidget(widget, Gtk::Orientation::VERTICAL, width);
      return std::max({0, height, childMeasure.minimum});
    }
  } // namespace

  ConstrainedGridBox::ConstrainedGridBox()
  {
    set_overflow(Gtk::Overflow::HIDDEN);
  }

  ConstrainedGridBox::~ConstrainedGridBox()
  {
    if (_grid != nullptr)
    {
      _grid->unparent();
    }
  }

  void ConstrainedGridBox::setGrid(Gtk::Grid& grid)
  {
    _grid = &grid;
    grid.set_parent(*this);
  }

  Gtk::SizeRequestMode ConstrainedGridBox::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH;
  }

  void ConstrainedGridBox::measure_vfunc(Gtk::Orientation orientation,
                                         int forSize,
                                         int& minimum,
                                         int& natural,
                                         int& minimumBaseline,
                                         int& naturalBaseline) const
  {
    minimumBaseline = -1;
    naturalBaseline = -1;

    if (_grid == nullptr)
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

    if (auto const width = std::max(0, requestedWidth); width > 0)
    {
      _grid->measure(orientation, width, minimum, natural, minimumBaseline, naturalBaseline);
    }
    else
    {
      _grid->measure(orientation, forSize, minimum, natural, minimumBaseline, naturalBaseline);
    }
  }

  void ConstrainedGridBox::size_allocate_vfunc(int width, int height, int baseline)
  {
    _lastAllocatedWidth = std::max(0, width);

    if (_grid != nullptr)
    {
      _grid->size_allocate({0, 0, _lastAllocatedWidth, height}, baseline);
    }
  }

  FieldInlineEditor::FieldInlineEditor()
  {
    set_halign(Gtk::Align::FILL);
    set_hexpand(true);
    set_overflow(Gtk::Overflow::HIDDEN);
    set_size_request(0, -1);

    _displayLabel.set_halign(Gtk::Align::FILL);
    _displayLabel.set_xalign(0.0F);
    _displayLabel.set_hexpand(true);
    _displayLabel.set_overflow(Gtk::Overflow::HIDDEN);
    _displayLabel.set_width_chars(0);
    _displayLabel.set_max_width_chars(1);
    _displayLabel.set_ellipsize(Pango::EllipsizeMode::END);
    _displayLabel.set_wrap(false);
    _displayLabel.set_lines(1);
    _displayLabel.set_parent(*this);

    _entry.set_halign(Gtk::Align::FILL);
    _entry.set_hexpand(true);
    _entry.set_overflow(Gtk::Overflow::HIDDEN);
    _entry.set_size_request(0, -1);
    _entry.set_visible(false);
    _entry.set_parent(*this);
    _entry.signal_activate().connect([this] { stopEditing(true); });

    auto const keyPtr = Gtk::EventControllerKey::create();
    keyPtr->signal_key_pressed().connect(
      [this](guint keyval, guint, Gdk::ModifierType) -> bool
      {
        if (keyval == GDK_KEY_Escape)
        {
          _editingCanceled.emit();
          stopEditing(false);
          return true;
        }

        return false;
      },
      false);
    _entry.add_controller(keyPtr);

    auto const focusPtr = Gtk::EventControllerFocus::create();
    focusPtr->signal_leave().connect([this] { stopEditing(true); });
    _entry.add_controller(focusPtr);

    auto const clickPtr = Gtk::GestureClick::create();
    clickPtr->signal_pressed().connect([this](std::int32_t, double, double) { startEditing(); });
    add_controller(clickPtr);
  }

  FieldInlineEditor::~FieldInlineEditor()
  {
    _entry.unparent();
    _displayLabel.unparent();
  }

  void FieldInlineEditor::setText(Glib::ustring const& text)
  {
    _text = text;
    _displayLabel.set_text(_text);

    if (!_editing)
    {
      _entry.set_text(_text);
    }
  }

  Glib::ustring FieldInlineEditor::getText() const
  {
    return _editing ? _entry.get_text() : _text;
  }

  void FieldInlineEditor::setEditable(bool editable)
  {
    _editable = editable;
  }

  bool FieldInlineEditor::getEditable() const
  {
    return _editable;
  }

  bool FieldInlineEditor::getEditing() const
  {
    return _editing;
  }

  void FieldInlineEditor::startEditing()
  {
    if (!_editable || _editing)
    {
      return;
    }

    _editing = true;
    _entry.set_text(_text);
    _displayLabel.set_visible(false);
    _entry.set_visible(true);
    _editingChanged.emit();
    _entry.grab_focus();
    _entry.select_region(0, -1);
  }

  void FieldInlineEditor::stopEditing(bool commit)
  {
    if (!_editing)
    {
      return;
    }

    if (commit)
    {
      _text = _entry.get_text();
      _displayLabel.set_text(_text);
    }
    else
    {
      _entry.set_text(_text);
    }

    _editing = false;
    _entry.set_visible(false);
    _displayLabel.set_visible(true);
    _editingChanged.emit();
  }

  sigc::signal<void()>& FieldInlineEditor::signalEditingChanged()
  {
    return _editingChanged;
  }

  sigc::signal<void()>& FieldInlineEditor::signalEditingCanceled()
  {
    return _editingCanceled;
  }

  void FieldInlineEditor::removeMaxWidthConstraint()
  {
    _displayLabel.set_max_width_chars(-1);
  }

  Gtk::SizeRequestMode FieldInlineEditor::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH;
  }

  void FieldInlineEditor::measure_vfunc(Gtk::Orientation orientation,
                                        int forSize,
                                        int& minimum,
                                        int& natural,
                                        int& minimumBaseline,
                                        int& naturalBaseline) const
  {
    minimumBaseline = -1;
    naturalBaseline = -1;

    if (orientation == Gtk::Orientation::HORIZONTAL)
    {
      auto const measured = measureWidget(visibleChild(), orientation, forSize);
      minimum = 0;
      natural = measured.natural;
      return;
    }

    auto const width = widthForVisibleChild(std::max(0, forSize));
    visibleChild().measure(orientation, width, minimum, natural, minimumBaseline, naturalBaseline);
  }

  void FieldInlineEditor::size_allocate_vfunc(int width, int height, int baseline)
  {
    visibleChild().size_allocate({0, 0, widthForVisibleChild(width), height}, baseline);
  }

  Gtk::Widget& FieldInlineEditor::visibleChild()
  {
    if (_editing)
    {
      return _entry;
    }

    return _displayLabel;
  }

  Gtk::Widget const& FieldInlineEditor::visibleChild() const
  {
    if (_editing)
    {
      return _entry;
    }

    return _displayLabel;
  }

  std::int32_t FieldInlineEditor::widthForVisibleChild(std::int32_t const width) const
  {
    if (!_editing)
    {
      return std::max({0, width, displayLabelMinimumWidth()});
    }

    return widthAtLeastMinimum(_entry, width);
  }

  std::int32_t FieldInlineEditor::displayLabelMinimumWidth() const
  {
    return minimumWidth(_displayLabel);
  }

  FixedHeightWidgetSlot::FixedHeightWidgetSlot(Gtk::Widget& child,
                                               bool const expand,
                                               bool const propagateNatural,
                                               std::int32_t const height,
                                               FixedHeightMinimum const minimum)
    : _child{child}
    , _expand{expand}
    , _propagateNatural{propagateNatural}
    , _minimumHeight{minimum == FixedHeightMinimum::Fixed ? height : 0}
    , _height{height}
  {
    set_halign(Gtk::Align::FILL);
    set_hexpand(_expand);
    set_overflow(Gtk::Overflow::HIDDEN);
    set_size_request(0, -1);
    _child.set_parent(*this);
  }

  FixedHeightWidgetSlot::~FixedHeightWidgetSlot()
  {
    _child.unparent();
  }

  Gtk::SizeRequestMode FixedHeightWidgetSlot::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH;
  }

  void FixedHeightWidgetSlot::measure_vfunc(Gtk::Orientation orientation,
                                            int forSize,
                                            int& minimum,
                                            int& natural,
                                            int& minimumBaseline,
                                            int& naturalBaseline) const
  {
    minimumBaseline = -1;
    naturalBaseline = -1;

    if (orientation == Gtk::Orientation::HORIZONTAL)
    {
      if (_propagateNatural)
      {
        natural = measureWidget(_child, orientation, forSize).natural;
      }
      else
      {
        natural = 0;
      }

      minimum = 0;
      return;
    }

    minimum = _minimumHeight;
    natural = _height;
  }

  void FixedHeightWidgetSlot::size_allocate_vfunc(int width, int height, int baseline)
  {
    auto const childWidth = widthAtLeastMinimum(_child, width);
    auto const childHeight = heightAtLeastMinimum(_child, childWidth, height);
    _child.size_allocate({0, 0, childWidth, childHeight}, baseline);
  }

  FieldValueWrapper::FieldValueWrapper(Gtk::Widget& valueWidget,
                                       bool const editable,
                                       bool const technical,
                                       bool const showEditHint,
                                       bool const propagateNaturalWidth)
    : _valueWidget{valueWidget}
    , _editable{editable}
    , _showEditHint{showEditHint}
    , _propagateNaturalWidth{propagateNaturalWidth}
  {
    set_halign(Gtk::Align::FILL);
    set_hexpand(true);
    set_overflow(Gtk::Overflow::HIDDEN);
    set_size_request(0, -1);
    _valueWidget.set_parent(*this);

    add_css_class("ao-detail-field-value");

    if (_editable)
    {
      add_css_class("ao-detail-field-editable");

      if (_showEditHint)
      {
        _editHint.set_from_icon_name("document-edit-symbolic");
        _editHint.add_css_class("ao-detail-field-edit-hint");
        _editHint.set_parent(*this);
      }
    }

    if (_editable)
    {
      auto motionPtr = Gtk::EventControllerMotion::create();
      motionPtr->signal_enter().connect([this](double, double) { updateHover(true); });
      motionPtr->signal_leave().connect([this] { updateHover(false); });
      add_controller(motionPtr);

      auto focusPtr = Gtk::EventControllerFocus::create();
      focusPtr->signal_enter().connect([this] { updateHover(true); });
      focusPtr->signal_leave().connect([this] { updateHover(false); });
      add_controller(focusPtr);
    }

    if (technical)
    {
      add_css_class("ao-detail-field-technical");
    }
  }

  FieldValueWrapper::~FieldValueWrapper()
  {
    if (_editable && _showEditHint)
    {
      _editHint.unparent();
    }

    _valueWidget.unparent();
  }

  void FieldValueWrapper::updateHover(bool hovered)
  {
    if (hovered)
    {
      add_css_class("ao-hover");
    }
    else
    {
      remove_css_class("ao-hover");
    }
  }

  Gtk::SizeRequestMode FieldValueWrapper::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH;
  }

  void FieldValueWrapper::measure_vfunc(Gtk::Orientation orientation,
                                        int forSize,
                                        int& minimum,
                                        int& natural,
                                        int& minimumBaseline,
                                        int& naturalBaseline) const
  {
    minimumBaseline = -1;
    naturalBaseline = -1;

    if (orientation == Gtk::Orientation::HORIZONTAL)
    {
      minimum = 0;
      natural = 0;

      if (_propagateNaturalWidth)
      {
        natural = naturalWidth(_valueWidget);

        if (_editable && _showEditHint)
        {
          natural += naturalWidth(_editHint) + 4;
        }
      }

      return;
    }

    auto const childWidth = widthAtLeastMinimum(_valueWidget, std::max(0, forSize));
    _valueWidget.measure(orientation, childWidth, minimum, natural, minimumBaseline, naturalBaseline);
  }

  void FieldValueWrapper::size_allocate_vfunc(int width, int height, int baseline)
  {
    auto const childWidth = widthAtLeastMinimum(_valueWidget, width);
    _valueWidget.size_allocate({0, 0, childWidth, height}, baseline);

    if (_editable && _showEditHint)
    {
      auto min = 0;
      auto nat = 0;
      auto minB = -1;
      auto natB = -1;
      _editHint.measure(Gtk::Orientation::HORIZONTAL, -1, min, nat, minB, natB);
      auto const hintWidth = nat;
      _editHint.measure(Gtk::Orientation::VERTICAL, hintWidth, min, nat, minB, natB);
      auto const hintHeight = nat;

      _editHint.size_allocate({width - hintWidth - 4, (height - hintHeight) / 2, hintWidth, hintHeight}, baseline);
    }
  }

  CompressibleActionRow::CompressibleActionRow(Gtk::Widget& content, Gtk::Widget& action, std::int32_t const spacing)
    : _content{content}, _action{action}, _spacing{spacing}
  {
    set_halign(Gtk::Align::FILL);
    set_hexpand(true);
    set_overflow(Gtk::Overflow::HIDDEN);
    set_size_request(0, -1);
    _content.set_parent(*this);
    _action.set_parent(*this);
  }

  CompressibleActionRow::~CompressibleActionRow()
  {
    _action.unparent();
    _content.unparent();
  }

  Gtk::SizeRequestMode CompressibleActionRow::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH;
  }

  void CompressibleActionRow::measure_vfunc(Gtk::Orientation orientation,
                                            int forSize,
                                            int& minimum,
                                            int& natural,
                                            int& minimumBaseline,
                                            int& naturalBaseline) const
  {
    minimumBaseline = -1;
    naturalBaseline = -1;

    if (orientation == Gtk::Orientation::HORIZONTAL)
    {
      auto const contentMeasure = measureWidget(_content, orientation, forSize);
      auto const actionMeasure = measureWidget(_action, orientation, forSize);
      minimum = 0;
      natural = contentMeasure.natural + actionMeasure.natural + spacingForAction(actionMeasure.natural);
      return;
    }

    auto const availableWidth = std::max(0, forSize);
    auto const actionWidth = actionNaturalWidth();
    auto const contentWidth =
      std::max(minimumWidth(_content), availableWidth - actionWidth - spacingForAction(actionWidth));

    auto const contentMeasure = measureWidget(_content, orientation, contentWidth);
    auto const actionMeasure = measureWidget(_action, orientation, actionWidth);
    minimum = std::max(contentMeasure.minimum, actionMeasure.minimum);
    natural = std::max(contentMeasure.natural, actionMeasure.natural);
  }

  void CompressibleActionRow::size_allocate_vfunc(int width, int height, int baseline)
  {
    auto const actionWidth = actionNaturalWidth();
    auto const spacing = spacingForAction(actionWidth);
    auto const contentMinWidth = minimumWidth(_content);
    auto const contentWidth = std::max(contentMinWidth, width - actionWidth - spacing);
    auto const actionX = std::max(contentWidth + spacing, width - actionWidth);

    _content.size_allocate({0, 0, contentWidth, height}, baseline);
    _action.size_allocate({actionX, 0, actionWidth, height}, baseline);
  }

  std::int32_t CompressibleActionRow::actionNaturalWidth() const
  {
    return naturalWidth(_action);
  }

  std::int32_t CompressibleActionRow::spacingForAction(std::int32_t const actionWidth) const
  {
    return actionWidth > 0 ? _spacing : 0;
  }
} // namespace ao::gtk::layout::track_field_grid
