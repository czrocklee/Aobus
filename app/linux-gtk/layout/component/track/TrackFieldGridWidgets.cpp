// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/track/TrackFieldGridWidgets.h"

#include "completion/EntryCompletionController.h"
#include "sigc++/signal.h"
#include <ao/rt/CompletionResult.h>

#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/grid.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

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

    if (forSize <= 0)
    {
      minimum = 0;
      natural = 0;
      return;
    }

    _grid->measure(
      orientation, widthAtLeastMinimum(*_grid, forSize), minimum, natural, minimumBaseline, naturalBaseline);
  }

  void ConstrainedGridBox::size_allocate_vfunc(int width, int height, int baseline)
  {
    _lastAllocatedWidth = (_grid != nullptr) ? widthAtLeastMinimum(*_grid, width) : std::max(0, width);

    if (_grid != nullptr)
    {
      _grid->size_allocate({0, 0, _lastAllocatedWidth, height}, baseline);
    }
  }

  DetailFieldEditor::DetailFieldEditor()
    : Gtk::Box{Gtk::Orientation::HORIZONTAL, 4}
  {
    set_halign(Gtk::Align::FILL);
    set_hexpand(true);
    set_overflow(Gtk::Overflow::HIDDEN);
    set_size_request(0, -1);
    add_css_class("ao-detail-field-value");
    add_css_class("ao-detail-field-editor");

    _stack.set_halign(Gtk::Align::FILL);
    _stack.set_hexpand(true);
    _stack.set_hhomogeneous(false);
    _stack.set_vhomogeneous(false);
    _stack.set_overflow(Gtk::Overflow::HIDDEN);

    _displayLabel.set_halign(Gtk::Align::FILL);
    _displayLabel.set_xalign(0.0F);
    _displayLabel.set_hexpand(true);
    _displayLabel.set_overflow(Gtk::Overflow::HIDDEN);
    _displayLabel.set_width_chars(0);
    _displayLabel.set_max_width_chars(1);
    _displayLabel.set_ellipsize(Pango::EllipsizeMode::END);
    _displayLabel.set_wrap(false);
    _displayLabel.set_lines(1);
    _stack.add(_displayLabel, "display");

    _entry.set_halign(Gtk::Align::FILL);
    _entry.set_hexpand(true);
    _entry.set_overflow(Gtk::Overflow::HIDDEN);
    _entry.set_size_request(0, -1);
    _entry.set_width_chars(0);
    _entry.add_css_class("ao-detail-field-entry");
    _stack.add(_entry, "edit");
    _stack.set_visible_child("display");
    _entry.signal_activate().connect([this] { stopEditing(true); });

    _editButton.set_icon_name("document-edit-symbolic");
    _editButton.set_has_frame(false);
    _editButton.set_focusable(true);
    _editButton.add_css_class("ao-icon-button");
    _editButton.add_css_class("ao-detail-field-edit-hint");
    _editButton.set_tooltip_text("Edit Value");
    _editButton.set_visible(false);
    _editButton.signal_clicked().connect([this] { startEditing(); });

    append(_stack);
    append(_editButton);

    auto const keyPtr = Gtk::EventControllerKey::create();
    keyPtr->signal_key_pressed().connect(
      [this](guint keyval, guint, Gdk::ModifierType) -> bool
      {
        if (keyval == GDK_KEY_Escape)
        {
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
  }

  DetailFieldEditor::~DetailFieldEditor() = default;

  void DetailFieldEditor::setText(Glib::ustring const& text)
  {
    _text = text;
    _displayLabel.set_text(_text);

    if (!_editing)
    {
      setEntryTextSilently(_text);
    }
  }

  Glib::ustring DetailFieldEditor::getText() const
  {
    return _editing ? _entry.get_text() : _text;
  }

  void DetailFieldEditor::setEditable(bool editable)
  {
    _editable = editable;
    _editButton.set_visible(_editable && !_editing);

    if (_editable)
    {
      add_css_class("ao-detail-field-editable");
    }
    else
    {
      remove_css_class("ao-detail-field-editable");
    }
  }

  bool DetailFieldEditor::getEditable() const
  {
    return _editable;
  }

  bool DetailFieldEditor::getEditing() const
  {
    return _editing;
  }

  void DetailFieldEditor::startEditing()
  {
    if (!_editable || _editing)
    {
      return;
    }

    _editStarted.emit();
    _editing = true;
    setEntryTextSilently(_text);
    _stack.set_visible_child("edit");
    _editButton.set_visible(false);
    _entry.grab_focus();
    _entry.select_region(0, -1);
  }

  void DetailFieldEditor::stopEditing(bool commit)
  {
    if (!_editing)
    {
      return;
    }

    _editing = false;

    if (commit)
    {
      _text = _entry.get_text();
      _displayLabel.set_text(_text);
    }
    else
    {
      setEntryTextSilently(_text);
    }

    _stack.set_visible_child("display");
    _editButton.set_visible(_editable);

    if (commit)
    {
      _committed.emit();
    }
    else
    {
      _canceled.emit();
    }
  }

  void DetailFieldEditor::setCompletionProvider(rt::CompletionProvider provider)
  {
    _completionControllerPtr = std::make_unique<EntryCompletionController>(_entry, std::move(provider));
  }

  sigc::signal<void()>& DetailFieldEditor::signalEditStarted()
  {
    return _editStarted;
  }

  sigc::signal<void()>& DetailFieldEditor::signalCommitted()
  {
    return _committed;
  }

  sigc::signal<void()>& DetailFieldEditor::signalCanceled()
  {
    return _canceled;
  }

  void DetailFieldEditor::removeMaxWidthConstraint()
  {
    _displayLabel.set_max_width_chars(-1);
  }

  void DetailFieldEditor::setEntryTextSilently(Glib::ustring const& text)
  {
    if (_completionControllerPtr != nullptr)
    {
      _completionControllerPtr->setTextProgrammatically(text);
      return;
    }

    _entry.set_text(text);
  }

  DetailEditCoordinator::DetailEditCoordinator(Gtk::Window& parentWindow)
    : _parentWindow{parentWindow}, _outsideClickPtr{Gtk::GestureClick::create()}
  {
    _outsideClickPtr->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    _outsideClickPtr->signal_pressed().connect(
      [this](std::int32_t, double const xPos, double const yPos)
      {
        if (auto* const target = _parentWindow.pick(xPos, yPos);
            _activeEditor != nullptr && !isDescendantOf(target, *_activeEditor))
        {
          _activeEditor->stopEditing(true);
        }
      });
    _parentWindow.add_controller(_outsideClickPtr);
  }

  DetailEditCoordinator::~DetailEditCoordinator()
  {
    _parentWindow.remove_controller(_outsideClickPtr);
  }

  void DetailEditCoordinator::registerEditor(DetailFieldEditor& editor)
  {
    editor.signalEditStarted().connect(
      [this, &editor]
      {
        if (_activeEditor != nullptr && _activeEditor != &editor)
        {
          _activeEditor->stopEditing(true);
        }

        _activeEditor = &editor;
      });

    auto clearActive = [this, &editor]
    {
      if (_activeEditor == &editor)
      {
        _activeEditor = nullptr;
      }
    };
    editor.signalCommitted().connect(clearActive);
    editor.signalCanceled().connect(clearActive);
  }

  void DetailEditCoordinator::forgetEditor(DetailFieldEditor& editor)
  {
    if (_activeEditor == &editor)
    {
      _activeEditor = nullptr;
    }
  }

  bool DetailEditCoordinator::isDescendantOf(Gtk::Widget const* widget, Gtk::Widget const& ancestor)
  {
    for (auto const* current = widget; current != nullptr; current = current->get_parent())
    {
      if (current == &ancestor)
      {
        return true;
      }
    }

    return false;
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
} // namespace ao::gtk::layout::track_field_grid
