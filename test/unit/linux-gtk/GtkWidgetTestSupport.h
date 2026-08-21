// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <glibmm/refptr.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Gtk
{
  class Button;
  class Entry;
  class Label;
  class ListBox;
  class ListBoxRow;
  class Popover;
} // namespace Gtk

namespace ao::gtk::test
{
  template<typename T>
  std::vector<T*> collectAll(Gtk::Widget& root)
  {
    auto result = std::vector<T*>{};

    if (auto* const match = dynamic_cast<T*>(&root); match != nullptr)
    {
      result.push_back(match);
    }

    for (auto* child = root.get_first_child(); child != nullptr; child = child->get_next_sibling())
    {
      auto nested = collectAll<T>(*child);
      result.insert(result.end(), nested.begin(), nested.end());
    }

    return result;
  }

  Gtk::Label* findLabelByText(Gtk::Widget& root, std::string const& text);
  std::vector<std::string> directChildLabelTextsByClass(Gtk::Widget& container, std::string_view cssClass);
  Gtk::Button* findButtonByLabel(Gtk::Widget& root, std::string const& labelText);
  bool hasCssClass(Gtk::Widget const& widget, std::string_view cssClass);
  bool hasAccessibleLabel(Gtk::Widget& widget, std::string_view label);

  void emitClicked(Gtk::Button& button);
  void emitActivate(Gtk::Entry& entry);
  void emitClosed(Gtk::Popover& popover);
  void emitShow(Gtk::Widget& widget);
  void emitRowActivated(Gtk::ListBox& listBox, Gtk::ListBoxRow& row);

  template<typename T>
  Glib::RefPtr<T> findController(Gtk::Widget& widget)
  {
    auto const controllersPtr = widget.observe_controllers();

    if (!controllersPtr)
    {
      return {};
    }

    auto const count = controllersPtr->get_n_items();

    for (std::uint32_t i = 0U; i < count; ++i)
    {
      if (auto const controllerPtr = std::dynamic_pointer_cast<T>(controllersPtr->get_object(i)); controllerPtr)
      {
        return controllerPtr;
      }
    }

    return {};
  }

  template<typename T, typename Predicate>
  Glib::RefPtr<T> findControllerIf(Gtk::Widget& widget, Predicate const& predicate)
  {
    auto const controllersPtr = widget.observe_controllers();

    if (!controllersPtr)
    {
      return {};
    }

    auto const count = controllersPtr->get_n_items();

    for (std::uint32_t i = 0U; i < count; ++i)
    {
      auto const controllerPtr = std::dynamic_pointer_cast<T>(controllersPtr->get_object(i));

      if (controllerPtr && predicate(*controllerPtr))
      {
        return controllerPtr;
      }
    }

    return {};
  }

  template<typename T>
  bool hasController(Gtk::Widget& widget)
  {
    return findController<T>(widget) != nullptr;
  }

  bool emitFocusEnter(Gtk::Widget& widget);
  bool emitFocusLeave(Gtk::Widget& widget);
  bool emitPointerEnter(Gtk::Widget& widget, double x = 0.0, double y = 0.0);
  bool emitPointerLeave(Gtk::Widget& widget);
  bool emitGesturePressed(Gtk::Widget& widget,
                          std::int32_t nPress = 1,
                          double x = 1.0,
                          double y = 1.0,
                          std::optional<Gtk::PropagationPhase> optPhase = std::nullopt);
  bool emitGestureReleased(Gtk::Widget& widget,
                           std::int32_t nPress = 1,
                           double x = 1.0,
                           double y = 1.0,
                           std::optional<Gtk::PropagationPhase> optPhase = std::nullopt);

  template<typename Visitor>
  void walkWidgets(Gtk::Widget& root, Visitor const& visit)
  {
    visit(root);

    for (auto* child = root.get_first_child(); child != nullptr; child = child->get_next_sibling())
    {
      walkWidgets(*child, visit);
    }
  }

  template<typename T>
  T* findWidget(Gtk::Widget& root)
  {
    T* result = nullptr;

    walkWidgets(root,
                [&result](Gtk::Widget& widget)
                {
                  if (result == nullptr)
                  {
                    result = dynamic_cast<T*>(&widget);
                  }
                });

    return result;
  }

  template<typename T>
  T* findWidgetByClass(Gtk::Widget& root, std::string_view cssClass)
  {
    T* result = nullptr;

    walkWidgets(root,
                [&result, cssClass](Gtk::Widget& widget)
                {
                  if (result == nullptr && hasCssClass(widget, cssClass))
                  {
                    result = dynamic_cast<T*>(&widget);
                  }
                });

    return result;
  }
} // namespace ao::gtk::test
