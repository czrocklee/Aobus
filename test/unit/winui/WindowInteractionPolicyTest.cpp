// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/WindowInteractionPolicy.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::winui
{
  TEST_CASE("Window interaction policy - reports every modal workflow as active", "[winui][regression][window-modal]")
  {
    CHECK_FALSE(hasActiveWindowModalWorkflow({}));
    CHECK(hasActiveWindowModalWorkflow(WindowModalWorkflowState{.listAuthoring = true}));
    CHECK(hasActiveWindowModalWorkflow(WindowModalWorkflowState{.libraryTransfer = true}));
    CHECK(hasActiveWindowModalWorkflow(WindowModalWorkflowState{.trackProperties = true}));
  }
} // namespace ao::winui
