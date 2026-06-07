// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/ComponentActionPolicy.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <glibmm/refptr.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/widget.h>
#include <sigc++/connection.h>

#include <functional>

namespace ao::gtk::layout
{
  class ActionBinder;
  struct LayoutContext;

  /**
   * @brief Handles standard layout actions (click, secondary click, long press) for a widget.
   */
  class ComponentInteractionController final
  {
  public:
    ComponentInteractionController();
    ~ComponentInteractionController();

    ComponentInteractionController(ComponentInteractionController const&) = delete;
    ComponentInteractionController& operator=(ComponentInteractionController const&) = delete;
    ComponentInteractionController(ComponentInteractionController&&) = delete;
    ComponentInteractionController& operator=(ComponentInteractionController&&) = delete;

    /**
     * @brief Attaches action controllers to the target widget based on the layout node properties.
     */
    void attach(LayoutContext& ctx,
                uimodel::layout::LayoutNode const& node,
                Gtk::Widget& target,
                uimodel::layout::ComponentActionPolicy policy);

  private:
    void attachPrimaryClick(uimodel::layout::LayoutNode const& node,
                            Gtk::Widget& target,
                            uimodel::layout::ComponentActionPolicy const& policy,
                            ActionBinder const& binder);
    void attachSecondaryClick(uimodel::layout::LayoutNode const& node,
                              Gtk::Widget& target,
                              uimodel::layout::ComponentActionPolicy const& policy,
                              ActionBinder const& binder);
    void attachPrimaryLongPress(uimodel::layout::LayoutNode const& node,
                                Gtk::Widget& target,
                                uimodel::layout::ComponentActionPolicy const& policy,
                                ActionBinder const& binder);
    void attachSecondaryLongPress(uimodel::layout::LayoutNode const& node,
                                  Gtk::Widget& target,
                                  uimodel::layout::ComponentActionPolicy const& policy,
                                  ActionBinder const& binder);

    std::function<void()> _primaryClick;
    std::function<void()> _secondaryClick;
    std::function<void()> _primaryLongPress;
    std::function<void()> _secondaryLongPress;

    Glib::RefPtr<Gtk::GestureClick> _primaryClickGesturePtr;
    Glib::RefPtr<Gtk::GestureClick> _secondaryClickGesturePtr;
    Glib::RefPtr<Gtk::GestureLongPress> _primaryLongPressGesturePtr;
    Glib::RefPtr<Gtk::GestureLongPress> _secondaryLongPressGesturePtr;

    sigc::connection _primaryClickConn;
    sigc::connection _secondaryClickConn;
    sigc::connection _primaryLongPressConn;
    sigc::connection _secondaryLongPressConn;

    bool _primaryLongPressHandled = false;
    bool _secondaryLongPressHandled = false;
  };
} // namespace ao::gtk::layout
