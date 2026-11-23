// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnFactoryBuilder.h"

#include "test/MessageCatalogTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/linux-gtk/GtkWidgetTestSupport.h"
#include "test/runtime/source/TrackSourceTestSupport.h"
#include "track/TrackFieldUi.h"
#include "track/TrackListModel.h"
#include "track/TrackRowBinding.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <catch2/catch_test_macros.hpp>
#include <gdk/gdkenums.h>
#include <gdk/gdkkeysyms.h>
#include <glib-object.h>
#include <glibmm/refptr.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/listitem.h>
#include <gtkmm/selectionmodel.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/stack.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <sigc++/scoped_connection.h>

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

  TEST_CASE("TrackColumnFactoryBuilder - binds column factories to track row widgets")
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

        // Realizing the cell materializes the row for the displayed label.
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
        entry->set_text("Abandoned on focus leave");
        REQUIRE(tryEmitFocusLeave(*entry));
        CHECK(stack->get_visible_child_name() == "display");
        CHECK(entry->get_text() == "Test Title");

        // A terminal authoring session is detached before the editor can submit
        // again, even if an activation is delivered to its hidden entry.
        entry->set_text("Must not commit");
        emitActivate(*entry);
        CHECK(commits.empty());

        columnView.set_model(Glib::RefPtr<Gtk::SelectionModel>{});
        drainGtkEvents();
      }

      SECTION("Escape discards the draft and a reopened editor commits to the bound row")
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
        entry->set_text("Cancelled Title");
        auto const keyControllerPtr = findController<Gtk::EventControllerKey>(*entry);
        REQUIRE(keyControllerPtr);
        gboolean handled = FALSE;
        ::g_signal_emit_by_name(keyControllerPtr->gobj(),
                                "key-pressed",
                                static_cast<guint>(GDK_KEY_Escape),
                                0U,
                                static_cast<GdkModifierType>(0),
                                &handled);
        REQUIRE(handled != FALSE);
        REQUIRE(stack->get_visible_child_name() == "display");
        CHECK(commits.empty());

        stack->set_visible_child("edit");
        REQUIRE(tryEmitFocusEnter(*entry));
        CHECK(entry->get_text() == "Test Title");
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

      SECTION("in-place refresh preserves the draft and only follows the bound row")
      {
        auto const titleFactoryPtr = buildColumnFactory(rt::TrackField::Title, beginEditSession, commitEdit, *modelPtr);
        auto const durationFactoryPtr =
          buildColumnFactory(rt::TrackField::Duration, beginEditSession, commitEdit, *modelPtr);
        columnView.append_column(Gtk::ColumnViewColumn::create("Title", titleFactoryPtr));
        columnView.append_column(Gtk::ColumnViewColumn::create("Duration", durationFactoryPtr));
        realizeColumnView(window, columnView);

        auto const rowPtr = cache.trackRow(trackId);
        REQUIRE(rowPtr);
        auto* const stack = findWidget<Gtk::Stack>(columnView);
        auto* const entry = findWidget<Gtk::Entry>(columnView);
        auto* const titleLabel = findLabelByText(columnView, "Test Title");
        auto* const durationLabel = findLabelByText(columnView, "2:00");
        REQUIRE(stack != nullptr);
        REQUIRE(entry != nullptr);
        REQUIRE(titleLabel != nullptr);
        REQUIRE(durationLabel != nullptr);

        auto const repopulate = [](TrackRowObject& row,
                                   std::string title,
                                   std::chrono::minutes duration,
                                   library::FileStatus status = library::FileStatus::Available)
        {
          row.populate(
            rt::TrackRow{.id = row.trackId(), .title = std::move(title), .duration = duration, .status = status});
        };

        stack->set_visible_child("edit");
        REQUIRE(tryEmitFocusEnter(*entry));
        entry->set_text("Local draft");
        repopulate(*rowPtr, "Refreshed Title", std::chrono::minutes{4}, library::FileStatus::Missing);

        CHECK(titleLabel->get_text() == "Refreshed Title");
        CHECK(durationLabel->get_text() == "4:00");
        CHECK(findWidgetByClass<Gtk::Widget>(columnView, "ao-missing-row") != nullptr);
        CHECK(stack->get_visible_child() == entry);
        CHECK(entry->get_text() == "Local draft");

        REQUIRE(tryEmitFocusLeave(*entry));
        CHECK(stack->get_visible_child() == titleLabel);
        CHECK(entry->get_text() == "Refreshed Title");

        REQUIRE_FALSE(rowPtr->signalFieldsChanged().empty());
        sourcePtr->reset(std::array{secondTrackId});
        // GTK can map the replacement before it delivers unbind for the old cells.
        REQUIRE(
          tryPumpGtkEventsUntil([&rowPtr] { return rowPtr->signalFieldsChanged().empty(); }, std::chrono::seconds{1}));

        auto const secondRowPtr = cache.trackRow(secondTrackId);
        REQUIRE(secondRowPtr);
        auto* const replacementLabel = findLabelByText(columnView, "Second Title");
        auto* const replacementEntry = findWidget<Gtk::Entry>(columnView);
        REQUIRE(replacementLabel != nullptr);
        REQUIRE(replacementEntry != nullptr);
        repopulate(*rowPtr, "Retired Title", std::chrono::minutes{5});
        CHECK(replacementLabel->get_text() == "Second Title");
        CHECK(replacementEntry->get_text() == "Second Title");

        repopulate(*secondRowPtr, "Refreshed Second Title", std::chrono::minutes{6});
        CHECK(replacementLabel->get_text() == "Refreshed Second Title");
        CHECK(replacementEntry->get_text() == "Refreshed Second Title");
        CHECK(findWidgetByClass<Gtk::Widget>(columnView, "ao-missing-row") == nullptr);

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
        entry->set_text("Abandoned during replacement");

        // The entry never took actual focus, so its leave handler cannot retire
        // the session for us. Observe the old binding before GTK releases it.
        bool editorUnbound = false;
        bool editorClosedOnUnbind = false;
        auto const unbindConnection = sigc::scoped_connection{factoryPtr->signal_unbind().connect(
          [&, stack, entry](Glib::RefPtr<Gtk::ListItem> const& itemPtr)
          {
            if (!editorUnbound && itemPtr->get_child() == stack)
            {
              editorUnbound = true;
              editorClosedOnUnbind = stack->get_visible_child_name() == "display" && entry->get_text() == "Test Title";
              emitActivate(*entry);
            }
          })};
        sourcePtr->reset(std::array{secondTrackId});
        REQUIRE(
          tryPumpGtkEventsUntil([&] { return editorUnbound && findLabelByText(columnView, "Second Title") != nullptr; },
                                std::chrono::seconds{1}));
        CHECK(editorClosedOnUnbind);
        CHECK(commits.empty());

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
