// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/LibraryWindowPolicy.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  TEST_CASE("LibraryWindowPolicy - fallback empty library is replaced", "[gtk][unit][app][library-window]")
  {
    CHECK(openLibraryWindowModeFor(LibraryWindowKind::FallbackEmptyLibrary) ==
          OpenLibraryWindowMode::ReplaceSourceWindow);
  }

  TEST_CASE("LibraryWindowPolicy - real library opens additional window", "[gtk][unit][app][library-window]")
  {
    CHECK(openLibraryWindowModeFor(LibraryWindowKind::Library) == OpenLibraryWindowMode::AddWindow);
  }
} // namespace ao::gtk::test
