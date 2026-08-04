// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CommonLayoutProps.h"

#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPlacement.h>

#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ao::gtk::layout
{
  namespace
  {
    /// The document's alignment vocabulary in the spelling GTK's own enum uses.
    Gtk::Align toGtkAlign(uimodel::LayoutAlignment const alignment)
    {
      switch (alignment)
      {
        case uimodel::LayoutAlignment::Fill: return Gtk::Align::FILL;
        case uimodel::LayoutAlignment::Start: return Gtk::Align::START;
        case uimodel::LayoutAlignment::End: return Gtk::Align::END;
        case uimodel::LayoutAlignment::Center: return Gtk::Align::CENTER;
      }

      return Gtk::Align::FILL;
    }

    /// GTK spells "no minimum" as a negative request, which is what an unset field means.
    std::int32_t toSizeRequest(std::optional<double> const& optMinimum)
    {
      return optMinimum ? static_cast<std::int32_t>(*optMinimum) : -1;
    }

    /**
     * @brief Apply the CSS classes @p node authors.
     *
     * Styling is the one common field the two shells do not share: GTK styles
     * through CSS classes and the Windows shell through XAML resources, so each
     * frontend reads its own styling field and rejects the other's.
     */
    void applyCssClasses(Gtk::Widget& widget, uimodel::LayoutNode const& node)
    {
      auto const it = node.layout.find("cssClasses");

      if (it == node.layout.end())
      {
        return;
      }

      if (auto const* classes = it->second.getIf<std::vector<std::string>>(); classes != nullptr)
      {
        for (auto const& className : *classes)
        {
          widget.add_css_class(className);
        }

        return;
      }

      if (auto const className = it->second.asString(); !className.empty())
      {
        widget.add_css_class(className);
      }
    }
  } // namespace

  void applyCommonProps(Gtk::Widget& widget, uimodel::LayoutNode const& node)
  {
    auto const placement = uimodel::planLayoutPlacement(node);

    // Left alone when unauthored: GTK derives expansion from a widget's
    // children until something states it, and stating false would stop that.
    if (placement.optHorizontalExpand)
    {
      widget.set_hexpand(*placement.optHorizontalExpand);
    }

    if (placement.optVerticalExpand)
    {
      widget.set_vexpand(*placement.optVerticalExpand);
    }

    if (placement.optHorizontalAlignment)
    {
      widget.set_halign(toGtkAlign(*placement.optHorizontalAlignment));
    }

    if (placement.optVerticalAlignment)
    {
      widget.set_valign(toGtkAlign(*placement.optVerticalAlignment));
    }

    if (placement.widthRequestAuthored || placement.heightRequestAuthored)
    {
      widget.set_size_request(toSizeRequest(placement.optMinWidth), toSizeRequest(placement.optMinHeight));
    }

    // Left alone when unauthored, like expansion, but for a sharper reason: a
    // component may already have hidden itself because it has nothing to show,
    // and a document that said nothing must not reveal it.
    if (placement.optAuthoredVisible)
    {
      widget.set_visible(*placement.optAuthoredVisible);
    }

    applyCssClasses(widget, node);
  }
} // namespace ao::gtk::layout
