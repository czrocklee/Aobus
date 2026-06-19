// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/GtkStyleRuntime.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtk/gtkstyleprovider.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/label.h>

#include <cstdint>

namespace ao::gtk::test
{
  TEST_CASE("GtkStyleRuntime - initialization and reloading", "[gtk][app][style]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto& manager = GtkStyleRuntime::instance();

    SECTION("singleton instance exists")
    {
      CHECK(&manager == &GtkStyleRuntime::instance());
    }

    SECTION("initialize is idempotent")
    {
      manager.initialize();
      manager.initialize();
    }

    SECTION("reload triggers signal after debounce")
    {
      bool refreshed = false;
      auto conn = manager.signalRefreshed().connect([&] { refreshed = true; });

      manager.reload();

      // Debounce is 150ms. We wait a bit more.
      for (std::int32_t i = 0; i < 20; ++i)
      {
        drainGtkEvents();

        if (refreshed)
        {
          break;
        }

        ::g_usleep(10000); // 10ms
      }

      CHECK(refreshed == true);
      conn.disconnect();
    }

    SECTION("register/unregister widget provider")
    {
      auto label = Gtk::Label{"Styled Label"};
      auto providerPtr = Gtk::CssProvider::create();

      manager.addProviderForDisplayOf(label, providerPtr, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      manager.removeProviderForDisplayOf(label, providerPtr);
    }
  }
} // namespace ao::gtk::test
