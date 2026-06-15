// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackQuickFilter.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/listmodel.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>
#include <string_view>

namespace ao::gtk::test
{
  namespace
  {
    template<typename T>
    T* findWidgetByClass(Gtk::Widget& root, std::string_view const className)
    {
      if (root.has_css_class(std::string{className}))
      {
        if (auto* const typed = dynamic_cast<T*>(&root); typed != nullptr)
        {
          return typed;
        }
      }

      for (auto* child = root.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        if (auto* const found = findWidgetByClass<T>(*child, className); found != nullptr)
        {
          return found;
        }
      }

      return nullptr;
    }

    void emitFocusSignal(TrackQuickFilter& filter, char const* signalName)
    {
      auto const controllersPtr = filter.observe_controllers();
      REQUIRE(controllersPtr);

      auto const count = controllersPtr->get_n_items();

      for (auto i = 0U; i < count; ++i)
      {
        auto const focusControllerPtr =
          std::dynamic_pointer_cast<Gtk::EventControllerFocus>(controllersPtr->get_object(i));

        if (focusControllerPtr)
        {
          ::g_signal_emit_by_name(focusControllerPtr->gobj(), signalName);
          return;
        }
      }

      FAIL("TrackQuickFilter did not install a focus controller");
    }
  } // namespace

  TEST_CASE("TrackQuickFilter - smoke test", "[gtk][track][viewmodel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter = TrackQuickFilter{runtime};
    CHECK(filter.has_css_class("ao-quick-filter"));
    CHECK(filter.entry().has_css_class("ao-quick-filter-entry"));

    // Just verify it wires up and doesn't crash
    auto const reply = runtime.views().createView({.listId = ListId{1}});
    runtime.workspace().setFocusedView(reply.viewId);

    drainGtkEvents();
  }

  TEST_CASE("TrackQuickFilter - typing does not overwrite presentation", "[gtk][track][viewmodel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter = TrackQuickFilter{runtime};

    auto config = rt::TrackListViewConfig{.listId = ListId{1}};
    config.optPresentation = rt::defaultTrackPresentationSpec();
    config.optPresentation->id = "custom";
    auto const reply = runtime.views().createView(config);
    runtime.workspace().setFocusedView(reply.viewId);

    drainGtkEvents();

    filter.setText("artist == 'Muse'");
    // Simulate activate (Enter key) if it requires it, or maybe it updates on changed?
    filter.activate();
    drainGtkEvents();

    // Wait for debounce timer (200ms)
    ::g_usleep(static_cast<gulong>(250) * 1000);
    drainGtkEvents();

    auto const state = runtime.views().trackListState(reply.viewId);
    CHECK(state.filterExpression == "artist == 'Muse'");
    CHECK(state.presentation.id == "custom"); // Should remain unchanged
  }

  TEST_CASE("TrackQuickFilter - clear button clears current filter text", "[gtk][track][viewmodel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter = TrackQuickFilter{runtime};
    auto* const clearButton = findWidgetByClass<Gtk::Button>(filter, "ao-quick-filter-clear");
    REQUIRE(clearButton != nullptr);

    filter.setText("artist == 'Muse'");
    drainGtkEvents();
    REQUIRE(filter.getText() == "artist == 'Muse'");
    CHECK(clearButton->get_visible());

    ::g_signal_emit_by_name(clearButton->gobj(), "clicked");
    drainGtkEvents();

    CHECK(filter.getText().empty());
    CHECK_FALSE(clearButton->get_visible());
  }

  TEST_CASE("TrackQuickFilter - active class follows focus within the compound control", "[gtk][track][viewmodel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter = TrackQuickFilter{runtime};
    CHECK_FALSE(filter.has_css_class("ao-quick-filter-active"));

    emitFocusSignal(filter, "enter");
    CHECK(filter.has_css_class("ao-quick-filter-active"));

    emitFocusSignal(filter, "leave");
    CHECK_FALSE(filter.has_css_class("ao-quick-filter-active"));
  }

  TEST_CASE("TrackQuickFilter - accepts query completion trigger text", "[gtk][track][completion]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto filter = TrackQuickFilter{runtime};
    filter.setText("$al");
    filter.setPosition(3);
    drainGtkEvents();

    CHECK(filter.getText() == "$al");
    CHECK(filter.getPosition() == 3);
  }
} // namespace ao::gtk::test
