// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnFactoryBuilder.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include "track/TrackFieldUi.h"
#include "track/TrackListModel.h"
#include "track/TrackRowBinding.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <catch2/catch_test_macros.hpp>
#include <glib-object.h>
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
#include <utility>
#include <vector>

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

    bool tryQueryTooltip(Gtk::Widget& widget)
    {
      gboolean handled = FALSE;
      ::g_signal_emit_by_name(
        widget.gobj(), "query-tooltip", 0, 0, gboolean{FALSE}, static_cast<gpointer>(nullptr), &handled);
      return handled != FALSE;
    }
  } // namespace

  TEST_CASE("TrackColumnFactoryBuilder - binds column factories to track row widgets",
            "[gtk][unit][track-column][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto secondTrackId = kInvalidTrackId;
    auto fixture =
      GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                        {
                          trackId = library::test::addTrackWithUniqueFixtureUri(
                            musicLibrary,
                            library::test::TrackSpec{
                              .title = "Test Title", .artist = "Test Artist", .duration = std::chrono::minutes{2}});
                          secondTrackId = library::test::addTrackWithUniqueFixtureUri(
                            musicLibrary,
                            library::test::TrackSpec{
                              .title = "Second Title", .artist = "Second Artist", .duration = std::chrono::minutes{3}});
                        }};
    auto cache = TrackRowCache{fixture.runtime().library(), ao::test::englishMessageCatalog()};

    {
      auto window = Gtk::Window{};
      auto columnView = Gtk::ColumnView{};
      window.set_child(columnView);

      auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
      sourcePtr->addInitial(trackId);
      auto projectionPtr = std::shared_ptr<rt::TrackListProjection>{
        fixture.runtime().views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr})};
      auto modelPtr = TrackListModel::create(cache);
      modelPtr->bindProjection(projectionPtr);

      auto selectionPtr = Gtk::SingleSelection::create(modelPtr);
      columnView.set_model(selectionPtr);

      auto const beginEditSession = [&fixture](Glib::RefPtr<TrackRowObject> const& rowPtr)
      { return uimodel::TrackAuthoringSession::begin(fixture.runtime().library(), std::array{rowPtr->trackId()}); };
      struct CommitObservation final
      {
        TrackId rowId{kInvalidTrackId};
        rt::TrackField field = rt::TrackField::Title;
        std::string text;
        std::vector<TrackId> sessionTargets;
      };
      auto commits = std::vector<CommitObservation>{};
      auto const commitEdit = [&commits](Glib::RefPtr<TrackRowObject> const& rowPtr,
                                         rt::TrackField field,
                                         std::string text,
                                         uimodel::TrackAuthoringSession& session)
      {
        commits.push_back(CommitObservation{
          .rowId = rowPtr->trackId(),
          .field = field,
          .text = std::move(text),
          .sessionTargets = std::vector<TrackId>{session.targetIds().begin(), session.targetIds().end()},
        });
      };

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
        CHECK_FALSE(tryQueryTooltip(*label));

        columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
        drainGtkEvents();
      }

      SECTION("technical scalar column uses shared end alignment")
      {
        auto factoryPtr = buildColumnFactory(rt::TrackField::SampleRate, beginEditSession, commitEdit, *modelPtr);
        auto columnPtr = Gtk::ColumnViewColumn::create("Sample Rate", factoryPtr);
        columnView.append_column(columnPtr);

        realizeColumnView(window, columnView);

        auto* const label = findLabelByText(columnView, "44100 Hz");
        REQUIRE(label != nullptr);
        CHECK(label->get_halign() == Gtk::Align::END);
        CHECK(label->get_xalign() == 1.0F);

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
        CHECK(tryQueryTooltip(*label));

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
        REQUIRE(tryEmitFocusEnter(*entry));
        auto const initialRevision = fixture.runtime().library().authoringAvailability().libraryRevision;
        bool replacementRequested = false;
        auto replacementSubscription = fixture.runtime().library().onAuthoringAvailabilityChanged(
          [entry, initialRevision, &replacementRequested](rt::LibraryAuthoringAvailability const& availability)
          {
            if (availability.libraryRevision != initialRevision)
            {
              replacementRequested = tryEmitFocusEnter(*entry);
            }
          });
        REQUIRE(runGtkTask(fixture.runtime(),
                           fixture.runtime().library().commands().createListAsync(rt::ListDraft{.name = "Unrelated"})));

        // Replace the invalidated session before its deferred teardown runs.
        // Clearing the old session must disconnect that exact idle callback.
        REQUIRE(replacementRequested);

        CHECK(stack->get_visible_child_name() == "edit");
        REQUIRE(tryEmitFocusLeave(*entry));
        CHECK(stack->get_visible_child_name() == "display");

        // A terminal authoring session is detached before the editor can submit
        // again, even if an activation is delivered to its hidden entry.
        entry->set_text("Must not commit");
        emitActivate(*entry);
        CHECK(commits.empty());

        columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
        drainGtkEvents();
      }

      SECTION("editable column routes the exact bound row field text and session identity")
      {
        auto factoryPtr = buildColumnFactory(rt::TrackField::Title, beginEditSession, commitEdit, *modelPtr);
        auto columnPtr = Gtk::ColumnViewColumn::create("Title", factoryPtr);
        columnView.append_column(columnPtr);
        realizeColumnView(window, columnView);

        auto* const entry = findWidget<Gtk::Entry>(columnView);
        auto* const stack = findWidget<Gtk::Stack>(columnView);
        REQUIRE(entry != nullptr);
        REQUIRE(stack != nullptr);
        REQUIRE(GPOINTER_TO_UINT(::g_object_get_data(G_OBJECT(stack->gobj()), kBoundTrackIdDataKey)) == trackId.raw());

        stack->set_visible_child("edit");
        REQUIRE(tryEmitFocusEnter(*entry));
        entry->set_text("Committed Title");
        emitActivate(*entry);

        REQUIRE(commits.size() == 1);
        CHECK(commits[0].rowId == trackId);
        CHECK(commits[0].field == rt::TrackField::Title);
        CHECK(commits[0].text == "Committed Title");
        CHECK(commits[0].sessionTargets == std::vector<TrackId>{trackId});
        CHECK(stack->get_visible_child_name() == "display");

        columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
        drainGtkEvents();
      }

      SECTION("model replacement retires the old edit and starts a session for the new row")
      {
        auto factoryPtr = buildColumnFactory(rt::TrackField::Title, beginEditSession, commitEdit, *modelPtr);
        auto columnPtr = Gtk::ColumnViewColumn::create("Title", factoryPtr);
        columnView.append_column(columnPtr);
        realizeColumnView(window, columnView);

        auto* const entry = findWidget<Gtk::Entry>(columnView);
        auto* const stack = findWidget<Gtk::Stack>(columnView);
        REQUIRE(entry != nullptr);
        REQUIRE(stack != nullptr);
        REQUIRE(GPOINTER_TO_UINT(::g_object_get_data(G_OBJECT(stack->gobj()), kBoundTrackIdDataKey)) == trackId.raw());

        stack->set_visible_child("edit");
        REQUIRE(tryEmitFocusEnter(*entry));
        sourcePtr->reset(std::array{secondTrackId});
        drainGtkEvents();
        // GTK may replace the cell rather than recycle the same wrapper. Observe
        // the current binding without dereferencing an obsolete widget pointer.
        auto* const replacementStack = findWidget<Gtk::Stack>(columnView);
        REQUIRE(replacementStack != nullptr);
        auto* const replacementEntry = findWidget<Gtk::Entry>(*replacementStack);
        REQUIRE(replacementEntry != nullptr);
        REQUIRE(GPOINTER_TO_UINT(::g_object_get_data(G_OBJECT(replacementStack->gobj()), kBoundTrackIdDataKey)) ==
                secondTrackId.raw());

        replacementEntry->set_text("Must not cross row identity");
        emitActivate(*replacementEntry);
        CHECK(commits.empty());
        CHECK(replacementStack->get_visible_child_name() == "display");

        replacementStack->set_visible_child("edit");
        REQUIRE(tryEmitFocusEnter(*replacementEntry));
        replacementEntry->set_text("Second committed title");
        emitActivate(*replacementEntry);
        REQUIRE(commits.size() == 1);
        CHECK(commits[0].rowId == secondTrackId);
        CHECK(commits[0].field == rt::TrackField::Title);
        CHECK(commits[0].text == "Second committed title");
        CHECK(commits[0].sessionTargets == std::vector<TrackId>{secondTrackId});

        columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
        drainGtkEvents();
      }

      window.unset_child();
      drainGtkEvents();
    }

    drainGtkEvents();
  }
} // namespace ao::gtk::test
