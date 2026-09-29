// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackSelectionControllerTestSupport.h"

#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "track/TrackFieldUi.h"
#include "track/TrackRowBinding.h"
#include "track/TrackRowObject.h"

#include <catch2/catch_test_macros.hpp>
#include <gdkmm/graphene_point.h>
#include <glib-object.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/label.h>
#include <gtkmm/listitem.h>
#include <gtkmm/object.h>
#include <gtkmm/signallistitemfactory.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

namespace ao::gtk::test
{
  namespace
  {
    Glib::RefPtr<Gtk::SignalListItemFactory> createTestCellFactory(std::string_view const cellText, bool const tagsCell)
    {
      auto const factoryPtr = Gtk::SignalListItemFactory::create();
      factoryPtr->signal_setup().connect([](Glib::RefPtr<Gtk::ListItem> const& itemPtr)
                                         { itemPtr->set_child(*Gtk::make_managed<Gtk::Label>()); });

      factoryPtr->signal_bind().connect(
        [cellText, tagsCell](Glib::RefPtr<Gtk::ListItem> const& itemPtr)
        {
          auto* const label = dynamic_cast<Gtk::Label*>(itemPtr->get_child());
          auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(itemPtr->get_item());

          if (label != nullptr && rowPtr != nullptr)
          {
            label->set_text(std::string{cellText});

            if (tagsCell)
            {
              label->add_css_class(kTagsCellCssClass);
            }

            ::g_object_set_data(G_OBJECT(label->gobj()),
                                kBoundTrackIdDataKey,
                                GUINT_TO_POINTER(static_cast<guint>(rowPtr->trackId().raw())));
          }
        });

      factoryPtr->signal_unbind().connect(
        [](Glib::RefPtr<Gtk::ListItem> const& itemPtr)
        {
          if (auto* const child = itemPtr->get_child(); child != nullptr)
          {
            ::g_object_set_data(G_OBJECT(child->gobj()), kBoundTrackIdDataKey, nullptr);
          }
        });

      return factoryPtr;
    }
  } // namespace

  void appendTestColumn(Gtk::ColumnView& columnView)
  {
    auto const columnPtr = Gtk::ColumnViewColumn::create("Track", createTestCellFactory("track", false));
    columnPtr->set_fixed_width(160);
    columnView.append_column(columnPtr);
  }

  void appendTagsTestColumn(Gtk::ColumnView& columnView)
  {
    auto const columnPtr = Gtk::ColumnViewColumn::create("Tags", createTestCellFactory("tags", true));
    columnPtr->set_fixed_width(160);
    columnView.append_column(columnPtr);
  }

  Gdk::Graphene::Point cellCenterInView(Gtk::ColumnView& columnView, char const* const cellText, TrackId const trackId)
  {
    auto const labels = collectAll<Gtk::Label>(columnView);
    auto const labelIter = std::ranges::find_if(
      labels,
      [cellText, trackId](Gtk::Label* label)
      {
        return label->get_text() == cellText &&
               GPOINTER_TO_UINT(::g_object_get_data(G_OBJECT(label->gobj()), kBoundTrackIdDataKey)) == trackId.raw();
      });
    REQUIRE(labelIter != labels.end());
    auto& label = **labelIter;
    REQUIRE(label.get_mapped());
    REQUIRE(label.get_width() > 0);
    REQUIRE(label.get_height() > 0);
    auto const optPoint = label.compute_point(columnView,
                                              Gdk::Graphene::Point{static_cast<float>(label.get_width()) / 2.0F,
                                                                   static_cast<float>(label.get_height()) / 2.0F});
    REQUIRE(optPoint);
    return *optPoint;
  }
} // namespace ao::gtk::test
