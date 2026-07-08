// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/GtkStyleRuntime.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtk/gtkstyleprovider.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/label.h>

namespace ao::gtk::test
{
  TEST_CASE("GtkStyleRuntime - initializes providers and emits reload notifications", "[gtk][unit][app][style]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto& manager = GtkStyleRuntime::instance();

    SECTION("singleton instance exists")
    {
      CHECK(&manager == &GtkStyleRuntime::instance());
    }

    SECTION("initialize is idempotent")
    {
      CHECK_NOTHROW(manager.initialize());
      CHECK_NOTHROW(manager.initialize());
    }

    SECTION("reload triggers signal after debounce")
    {
      bool refreshed = false;
      auto conn = manager.signalRefreshed().connect([&] { refreshed = true; });

      manager.reload();

      CHECK(pumpGtkEventsUntil([&] { return refreshed; }));
      conn.disconnect();
    }

    SECTION("register/unregister widget provider")
    {
      auto label = Gtk::Label{"Styled Label"};
      auto providerPtr = Gtk::CssProvider::create();

      CHECK_NOTHROW(manager.addProviderForDisplayOf(label, providerPtr, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION));
      CHECK_NOTHROW(manager.removeProviderForDisplayOf(label, providerPtr));
    }
  }
} // namespace ao::gtk::test
