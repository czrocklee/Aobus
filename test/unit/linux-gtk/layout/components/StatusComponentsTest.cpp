// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/component/ComponentRegistrations.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>

#include <memory>

namespace ao::gtk::layout::test
{
  TEST_CASE("StatusComponents - status bar components register status schema entries", "[gtk][unit][layout][status]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.status_test");
    auto const tempDir = ao::test::TempDir{};
    std::unique_ptr<rt::AppRuntime> runtimePtr = ao::gtk::test::makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    registerStatusComponents(registry, *runtimePtr, ao::test::englishMessageCatalog());

    auto const optComponentSchema = registry.schema().component("status.activity");
    REQUIRE(optComponentSchema);
    CHECK(optComponentSchema->displayName == "Activity Status");

    auto const optSelectionSchema = registry.schema().component("status.selectionInfo");
    REQUIRE(optSelectionSchema);
    CHECK(optSelectionSchema->displayName == "Selection Info");

    CHECK_FALSE(registry.schema().component("status.statusSlot").has_value());
    CHECK_FALSE(registry.schema().component("status.notificationCenter").has_value());
  }
} // namespace ao::gtk::layout::test
