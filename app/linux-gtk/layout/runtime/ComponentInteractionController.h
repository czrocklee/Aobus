// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <glibmm/refptr.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>

#include <functional>

namespace ao::gtk::layout
{
  class ActionBinder;
  struct LayoutBuildContext;

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
     * @pre target remains valid until this controller is detached or destroyed.
     */
    void attach(LayoutBuildContext& ctx,
                uimodel::LayoutNode const& node,
                Gtk::Widget& target,
                uimodel::LayoutComponentActionPolicy policy);
    void detach();

  private:
    void attachPrimaryClick(uimodel::LayoutNode const& node,
                            Gtk::Widget& target,
                            uimodel::LayoutComponentActionPolicy const& policy,
                            ActionBinder const& binder);
    void attachSecondaryClick(uimodel::LayoutNode const& node,
                              Gtk::Widget& target,
                              uimodel::LayoutComponentActionPolicy const& policy,
                              ActionBinder const& binder);
    void attachPrimaryLongPress(uimodel::LayoutNode const& node,
                                Gtk::Widget& target,
                                uimodel::LayoutComponentActionPolicy const& policy,
                                ActionBinder const& binder);
    void attachSecondaryLongPress(uimodel::LayoutNode const& node,
                                  Gtk::Widget& target,
                                  uimodel::LayoutComponentActionPolicy const& policy,
                                  ActionBinder const& binder);

    std::function<void()> _primaryClick;
    std::function<void()> _secondaryClick;
    std::function<void()> _primaryLongPress;
    std::function<void()> _secondaryLongPress;

    Glib::RefPtr<Gtk::GestureClick> _secondaryClickGesturePtr;
    Glib::RefPtr<Gtk::GestureLongPress> _primaryLongPressGesturePtr;
    Glib::RefPtr<Gtk::GestureLongPress> _secondaryLongPressGesturePtr;

    Gtk::Widget* _target = nullptr;
    sigc::scoped_connection _primaryClickConn;
    sigc::scoped_connection _secondaryClickConn;
    sigc::scoped_connection _primaryLongPressConn;
    sigc::scoped_connection _secondaryLongPressConn;

    bool _primaryLongPressHandled = false;
    bool _secondaryLongPressHandled = false;
  };
} // namespace ao::gtk::layout
