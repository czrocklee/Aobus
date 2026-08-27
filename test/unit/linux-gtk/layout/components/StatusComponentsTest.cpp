// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/component/status/StatusRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>

#include <memory>

namespace ao::gtk::layout::test
{
  TEST_CASE("StatusComponents - status bar components register status descriptors", "[gtk][unit][layout][status]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.status_test");
    auto const tempDir = ao::test::TempDir{};
    std::unique_ptr<rt::AppRuntime> runtimePtr = ao::gtk::test::makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    registerStatusComponents(registry, *runtimePtr, ao::test::englishMessageCatalog());

    auto const optDesc = registry.descriptor("status.activity");
    REQUIRE(optDesc);
    CHECK(optDesc->displayName == "Activity Status");

    auto const optSelectionDesc = registry.descriptor("status.selectionInfo");
    REQUIRE(optSelectionDesc);
    CHECK(optSelectionDesc->displayName == "Selection Info");

    CHECK_FALSE(registry.descriptor("status.statusSlot").has_value());
    CHECK_FALSE(registry.descriptor("status.notificationCenter").has_value());
  }
} // namespace ao::gtk::layout::test
