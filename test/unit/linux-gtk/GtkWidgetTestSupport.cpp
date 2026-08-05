// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkWidgetTestSupport.h"

#include <glib-object.h>
#include <gtk/gtk.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/popover.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::gtk::test
{
  Gtk::Label* findLabelByText(Gtk::Widget& root, std::string const& text)
  {
    for (auto* const label : collectAll<Gtk::Label>(root))
    {
      if (label->get_text() == text)
      {
        return label;
      }
    }

    return nullptr;
  }

  Gtk::Button* findButtonByLabel(Gtk::Widget& root, std::string const& labelText)
  {
    for (auto* const button : collectAll<Gtk::Button>(root))
    {
      if (button->get_label() == labelText)
      {
        return button;
      }
    }

    return nullptr;
  }

  bool hasCssClass(Gtk::Widget const& widget, std::string_view const cssClass)
  {
    auto const classes = widget.get_css_classes();
    return std::ranges::any_of(
      classes, [cssClass](auto const& name) { return std::string_view{name.raw()} == cssClass; });
  }

  bool hasAccessibleLabel(Gtk::Widget& widget, std::string_view const label)
  {
    auto const expected = std::string{label};
    auto* const mismatch = ::gtk_test_accessible_check_property(
      GTK_ACCESSIBLE(widget.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, expected.c_str());

    if (mismatch == nullptr)
    {
      return true;
    }

    ::g_free(mismatch);
    return false;
  }

  void emitClicked(Gtk::Button& button)
  {
    ::g_signal_emit_by_name(button.gobj(), "clicked");
  }

  void emitActivate(Gtk::Entry& entry)
  {
    ::g_signal_emit_by_name(entry.gobj(), "activate");
  }

  void emitClosed(Gtk::Popover& popover)
  {
    ::g_signal_emit_by_name(popover.gobj(), "closed");
  }

  void emitShow(Gtk::Widget& widget)
  {
    ::g_signal_emit_by_name(widget.gobj(), "show");
  }

  void emitRowActivated(Gtk::ListBox& listBox, Gtk::ListBoxRow& row)
  {
    ::g_signal_emit_by_name(listBox.gobj(), "row-activated", row.gobj());
  }

  bool emitFocusEnter(Gtk::Widget& widget)
  {
    auto const focusControllerPtr = findController<Gtk::EventControllerFocus>(widget);

    if (!focusControllerPtr)
    {
      return false;
    }

    ::g_signal_emit_by_name(focusControllerPtr->gobj(), "enter");
    return true;
  }

  bool emitFocusLeave(Gtk::Widget& widget)
  {
    auto const focusControllerPtr = findController<Gtk::EventControllerFocus>(widget);

    if (!focusControllerPtr)
    {
      return false;
    }

    ::g_signal_emit_by_name(focusControllerPtr->gobj(), "leave");
    return true;
  }

  bool emitPointerEnter(Gtk::Widget& widget, double const x, double const y)
  {
    auto const motionControllerPtr = findController<Gtk::EventControllerMotion>(widget);

    if (!motionControllerPtr)
    {
      return false;
    }

    ::g_signal_emit_by_name(motionControllerPtr->gobj(), "enter", x, y);
    return true;
  }

  bool emitPointerLeave(Gtk::Widget& widget)
  {
    auto const motionControllerPtr = findController<Gtk::EventControllerMotion>(widget);

    if (!motionControllerPtr)
    {
      return false;
    }

    ::g_signal_emit_by_name(motionControllerPtr->gobj(), "leave");
    return true;
  }

  bool emitGesturePressed(Gtk::Widget& widget,
                          std::int32_t const nPress,
                          double const x,
                          double const y,
                          std::optional<Gtk::PropagationPhase> const optPhase)
  {
    auto const gesturePtr =
      findControllerIf<Gtk::GestureClick>(widget,
                                          [optPhase](Gtk::GestureClick const& gesture)
                                          { return !optPhase || gesture.get_propagation_phase() == *optPhase; });

    if (!gesturePtr)
    {
      return false;
    }

    ::g_signal_emit_by_name(gesturePtr->gobj(), "pressed", nPress, x, y);
    return true;
  }

  bool emitGestureReleased(Gtk::Widget& widget,
                           std::int32_t const nPress,
                           double const x,
                           double const y,
                           std::optional<Gtk::PropagationPhase> const optPhase)
  {
    auto const gesturePtr =
      findControllerIf<Gtk::GestureClick>(widget,
                                          [optPhase](Gtk::GestureClick const& gesture)
                                          { return !optPhase || gesture.get_propagation_phase() == *optPhase; });

    if (!gesturePtr)
    {
      return false;
    }

    ::g_signal_emit_by_name(gesturePtr->gobj(), "released", nPress, x, y);
    return true;
  }
} // namespace ao::gtk::test
