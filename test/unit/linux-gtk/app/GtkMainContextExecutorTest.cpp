// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/GtkMainContextExecutor.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  TEST_CASE("GtkMainContextExecutor - dispatch runs immediately on the owner thread", "[gtk][unit][app][executor]")
  {
    auto executor = GtkMainContextExecutor{};
    bool ran = false;

    executor.dispatch(
      [&]
      {
        ran = true;
        CHECK(executor.isCurrent());
      });

    CHECK(ran);
  }
} // namespace ao::gtk::test
