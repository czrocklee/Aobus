// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/component/status/StatusRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>

namespace ao::gtk::layout::test
{
  TEST_CASE("Status bar components register descriptors", "[gtk][unit][shell]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.status_test");

    auto registry = ComponentRegistry{};
    registerStatusComponents(registry);

    auto const optDesc = registry.descriptor("status.statusSlot");
    REQUIRE(optDesc.has_value());
    CHECK(optDesc->displayName == "Status Slot");
  }
} // namespace ao::gtk::layout::test
