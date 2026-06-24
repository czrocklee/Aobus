// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "CommonLayoutProps.h"

#include <ao/uimodel/layout/LayoutNode.h>

#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel::layout;
  namespace
  {
    std::optional<Gtk::Align> parseAlign(std::string_view alignment)
    {
      if (alignment == "fill")
      {
        return Gtk::Align::FILL;
      }

      if (alignment == "start")
      {
        return Gtk::Align::START;
      }

      if (alignment == "end")
      {
        return Gtk::Align::END;
      }

      if (alignment == "center")
      {
        return Gtk::Align::CENTER;
      }

      return std::nullopt;
    }
  } // namespace

  void applyCommonProps(Gtk::Widget& widget, LayoutNode const& node)
  {
    auto const& layout = node.layout;

    if (auto const it = layout.find("hexpand"); it != layout.end())
    {
      widget.set_hexpand(it->second.asBool());
    }

    if (auto const it = layout.find("vexpand"); it != layout.end())
    {
      widget.set_vexpand(it->second.asBool());
    }

    if (auto const it = layout.find("halign"); it != layout.end())
    {
      if (auto const optAlignment = parseAlign(it->second.asString()); optAlignment)
      {
        widget.set_halign(*optAlignment);
      }
    }

    if (auto const it = layout.find("valign"); it != layout.end())
    {
      if (auto const optAlignment = parseAlign(it->second.asString()); optAlignment)
      {
        widget.set_valign(*optAlignment);
      }
    }

    std::int32_t width = -1;
    std::int32_t height = -1;
    bool sizeChanged = false;

    if (auto const it = layout.find("widthRequest"); it != layout.end())
    {
      width = static_cast<std::int32_t>(it->second.asInt());
      sizeChanged = true;
    }

    if (auto const it = layout.find("heightRequest"); it != layout.end())
    {
      height = static_cast<std::int32_t>(it->second.asInt());
      sizeChanged = true;
    }

    if (sizeChanged)
    {
      widget.set_size_request(width, height);
    }

    if (auto const it = layout.find("visible"); it != layout.end())
    {
      widget.set_visible(it->second.asBool());
    }

    if (auto const it = layout.find("cssClasses"); it != layout.end())
    {
      if (auto const* classes = it->second.getIf<std::vector<std::string>>(); classes != nullptr)
      {
        for (auto const& className : *classes)
        {
          widget.add_css_class(className);
        }
      }
      else if (auto const className = it->second.asString(); !className.empty())
      {
        widget.add_css_class(className);
      }
    }
  }
} // namespace ao::gtk::layout
