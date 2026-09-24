// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/GtkStyleRuntime.h"

#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gdkmm/rgba.h>
#include <glibmm/main.h>
#include <gtk/gtkstyleprovider.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/label.h>
#include <sigc++/scoped_connection.h>

#include <chrono>
#include <cstdint>

namespace ao::gtk::test
{
  namespace
  {
    class [[nodiscard]] StyleRuntimeScope final
    {
    public:
      explicit StyleRuntimeScope(GtkStyleRuntime& runtime)
        : _runtime{runtime}
      {
        _runtime.shutdown();
      }

      ~StyleRuntimeScope() { _runtime.shutdown(); }

      StyleRuntimeScope(StyleRuntimeScope const&) = delete;
      StyleRuntimeScope& operator=(StyleRuntimeScope const&) = delete;
      StyleRuntimeScope(StyleRuntimeScope&&) = delete;
      StyleRuntimeScope& operator=(StyleRuntimeScope&&) = delete;

    private:
      GtkStyleRuntime& _runtime;
    };
  } // namespace

  TEST_CASE("GtkStyleRuntime - provider lifecycle is idempotent and reinitializable", "[gtk][unit][app][style]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto& manager = GtkStyleRuntime::instance();
    auto runtimeScope = StyleRuntimeScope{manager};

    CHECK(&manager == &GtkStyleRuntime::instance());

    manager.initialize();
    auto const firstProviderPtr = manager.appProvider();
    REQUIRE(firstProviderPtr);

    manager.initialize();
    manager.initialize();
    CHECK(manager.appProvider() == firstProviderPtr);

    manager.shutdown();
    CHECK_FALSE(manager.appProvider());
    manager.shutdown();
    CHECK_FALSE(manager.appProvider());

    manager.initialize();
    REQUIRE(manager.appProvider());
    CHECK(manager.appProvider() != firstProviderPtr);
  }

  TEST_CASE("GtkStyleRuntime - reload is debounced and shutdown cancels pending delivery",
            "[gtk][unit][app][style][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto& manager = GtkStyleRuntime::instance();
    auto runtimeScope = StyleRuntimeScope{manager};
    manager.initialize();

    SECTION("reload triggers signal after debounce")
    {
      std::int32_t refreshCount = 0;
      auto refreshConnection = sigc::scoped_connection{manager.signalRefreshed().connect([&] { ++refreshCount; })};

      manager.reload();
      manager.reload();

      REQUIRE(tryPumpGtkEventsUntil([&] { return refreshCount == 1; }));

      bool deadlineReached = false;
      auto deadlineConnection = sigc::scoped_connection{Glib::signal_timeout().connect(
        [&]
        {
          deadlineReached = true;
          return false;
        },
        std::chrono::milliseconds{200}.count())};
      REQUIRE(tryPumpGtkEventsUntil([&] { return deadlineReached; }));
      CHECK(refreshCount == 1);
    }

    SECTION("shutdown cancels a pending reload")
    {
      std::int32_t refreshCount = 0;
      auto refreshConnection = sigc::scoped_connection{manager.signalRefreshed().connect([&] { ++refreshCount; })};

      // First establish that this live runtime and connection deliver a reload.
      manager.reload();
      REQUIRE(tryPumpGtkEventsUntil([&] { return refreshCount == 1; }));

      manager.reload();
      manager.shutdown();

      bool deadlineReached = false;
      auto deadlineConnection = sigc::scoped_connection{Glib::signal_timeout().connect(
        [&]
        {
          deadlineReached = true;
          return false;
        },
        std::chrono::milliseconds{200}.count())};
      REQUIRE(tryPumpGtkEventsUntil([&] { return deadlineReached; }));
      CHECK(refreshCount == 1);
    }
  }

  TEST_CASE("GtkStyleRuntime - display provider attachment applies and removes CSS", "[gtk][unit][app][style]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto& manager = GtkStyleRuntime::instance();
    auto runtimeScope = StyleRuntimeScope{manager};
    manager.initialize();

    auto label = Gtk::Label{"Styled Label"};
    auto providerPtr = Gtk::CssProvider::create();
    providerPtr->load_from_data("@define-color ao_style_runtime_probe rgb(18, 52, 86);");
    auto styleContextPtr = label.get_style_context();
    auto color = Gdk::RGBA{};
    CHECK_FALSE(styleContextPtr->lookup_color("ao_style_runtime_probe", color));

    auto providerRegistration =
      utility::ScopedRegistration{[&] { manager.removeProviderForDisplayOf(label, providerPtr); }};
    manager.addProviderForDisplayOf(label, providerPtr, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    drainGtkEvents();

    REQUIRE(styleContextPtr->lookup_color("ao_style_runtime_probe", color));
    CHECK(color.get_red() == Catch::Approx(18.0 / 255.0));
    CHECK(color.get_green() == Catch::Approx(52.0 / 255.0));
    CHECK(color.get_blue() == Catch::Approx(86.0 / 255.0));

    providerRegistration.reset();
    drainGtkEvents();
    CHECK_FALSE(styleContextPtr->lookup_color("ao_style_runtime_probe", color));
  }
} // namespace ao::gtk::test
