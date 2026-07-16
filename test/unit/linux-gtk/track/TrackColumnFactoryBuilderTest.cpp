// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnFactoryBuilder.h"

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include "track/TrackFieldUi.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/selectionmodel.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/stack.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace ao::gtk::test
{
  namespace
  {
    void realizeColumnView(Gtk::Window& window, Gtk::ColumnView& columnView)
    {
      window.set_default_size(400, 200);
      window.set_visible(true);
      drainGtkEvents();

      std::int32_t minimum = std::int32_t{};
      std::int32_t natural = std::int32_t{};
      std::int32_t minimumBaseline = std::int32_t{};
      std::int32_t naturalBaseline = std::int32_t{};
      columnView.measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);
      columnView.measure(Gtk::Orientation::VERTICAL, 400, minimum, natural, minimumBaseline, naturalBaseline);
      columnView.size_allocate(Gtk::Allocation{0, 0, 400, 200}, -1);
      drainGtkEvents();
    }

    bool queryTooltip(Gtk::Widget& widget)
    {
      gboolean handled = FALSE;
      ::g_signal_emit_by_name(
        widget.gobj(), "query-tooltip", 0, 0, gboolean{FALSE}, static_cast<gpointer>(nullptr), &handled);
      return handled != FALSE;
    }
  } // namespace

  TEST_CASE("TrackColumnFactoryBuilder - binds column factories to track row widgets", "[gtk][unit][track][column]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture =
      GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                        {
                          trackId = library::test::addTrack(
                            musicLibrary,
                            library::test::TrackSpec{
                              .title = "Test Title", .artist = "Test Artist", .duration = std::chrono::minutes{2}});
                        }};
    auto const& library = fixture.runtime().musicLibrary();
    auto cache = TrackRowCache{fixture.runtime().library()};

    {
      auto window = Gtk::Window{};
      auto columnView = Gtk::ColumnView{};
      window.set_child(columnView);

      auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
      sourcePtr->addInitial(trackId);
      auto projectionPtr =
        std::make_shared<rt::LiveTrackListProjection>(rt::ViewId{1}, rt::TrackSourceLease{sourcePtr}, library);
      auto modelPtr = TrackListModel::create(cache);
      modelPtr->bindProjection(projectionPtr);

      auto selectionPtr = Gtk::SingleSelection::create(modelPtr);
      columnView.set_model(selectionPtr);

      auto const beginEditSession = [&fixture](Glib::RefPtr<TrackRowObject> const& rowPtr)
      { return uimodel::TrackAuthoringSession::begin(fixture.runtime().library(), std::array{rowPtr->trackId()}); };
      std::int32_t committedEditCount = 0;
      auto const commitEdit = [&committedEditCount](Glib::RefPtr<TrackRowObject> const&,
                                                    rt::TrackField,
                                                    std::string,
                                                    uimodel::TrackAuthoringSession&) { ++committedEditCount; };

      SECTION("static column (e.g. Duration)")
      {
        auto factoryPtr = buildColumnFactory(rt::TrackField::Duration, beginEditSession, commitEdit, *modelPtr);
        auto columnPtr = Gtk::ColumnViewColumn::create("Duration", factoryPtr);
        columnView.append_column(columnPtr);

        realizeColumnView(window, columnView);

        auto* const label = findLabelByText(columnView, "2:00");
        REQUIRE(label != nullptr);
        CHECK(label->get_single_line_mode());
        CHECK(label->get_lines() == 1);
        CHECK(label->get_has_tooltip());
        CHECK(label->get_tooltip_text().empty());
        CHECK_FALSE(queryTooltip(*label));

        columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
        drainGtkEvents();
      }

      SECTION("tooltip appears only when text is ellipsized")
      {
        auto factoryPtr = buildColumnFactory(rt::TrackField::Title, beginEditSession, commitEdit, *modelPtr);
        auto columnPtr = Gtk::ColumnViewColumn::create("Title", factoryPtr);
        columnPtr->set_fixed_width(24);
        columnView.append_column(columnPtr);

        realizeColumnView(window, columnView);

        auto* const label = findLabelByText(columnView, "Test Title");
        REQUIRE(label != nullptr);
        REQUIRE(label->get_layout() != nullptr);
        CHECK(label->get_layout()->is_ellipsized());
        CHECK(label->get_has_tooltip());
        CHECK(label->get_tooltip_text().empty());
        CHECK(queryTooltip(*label));

        columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
        drainGtkEvents();
      }

      SECTION("editable column (e.g. Title)")
      {
        auto factoryPtr = buildColumnFactory(rt::TrackField::Title, beginEditSession, commitEdit, *modelPtr);
        auto columnPtr = Gtk::ColumnViewColumn::create("Title", factoryPtr);
        columnView.append_column(columnPtr);

        drainGtkEvents();

        auto rowPtr = cache.trackRow(trackId);
        CHECK(rowPtr);

        // Realize the cell and check the display label is rendered.
        realizeColumnView(window, columnView);

        auto* const label = findLabelByText(columnView, "Test Title");
        REQUIRE(label != nullptr);
        CHECK(label->get_single_line_mode());
        CHECK(label->get_lines() == 1);

        // Drive the now-playing highlight through the model signal (the path used
        // in production); the per-cell subscription must restyle the realized row.
        CHECK(findWidgetByClass<Gtk::Widget>(columnView, "ao-playing-row") == nullptr);

        modelPtr->setPlayingTrackId(trackId);
        drainGtkEvents();
        CHECK(findWidgetByClass<Gtk::Widget>(columnView, "ao-playing-row") != nullptr);

        modelPtr->setPlayingTrackId(kInvalidTrackId);
        drainGtkEvents();
        CHECK(findWidgetByClass<Gtk::Widget>(columnView, "ao-playing-row") == nullptr);

        auto* const entry = findWidget<Gtk::Entry>(columnView);
        REQUIRE(entry != nullptr);
        auto* const stack = findWidget<Gtk::Stack>(columnView);
        REQUIRE(stack != nullptr);

        stack->set_visible_child("edit");
        REQUIRE(emitFocusEnter(*entry));
        REQUIRE(fixture.runtime().library().writer().createList(rt::LibraryWriter::ListDraft{.name = "Unrelated"}));
        drainGtkEvents();

        CHECK(stack->get_visible_child_name() == "display");

        // A terminal authoring session is detached before the editor can submit
        // again, even if an activation is delivered to its hidden entry.
        entry->set_text("Must not commit");
        emitActivate(*entry);
        CHECK(committedEditCount == 0);

        columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
        drainGtkEvents();
      }

      window.unset_child();
      drainGtkEvents();
    }

    drainGtkEvents();
  }
} // namespace ao::gtk::test
