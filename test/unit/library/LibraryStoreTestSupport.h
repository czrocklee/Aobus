// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/unit/TestFixtureSupport.h"
#include <ao/library/MusicLibrary.h>

namespace ao::library::test
{
  struct LibraryStoreFixture final
  {
    ao::test::TempDir temp;
    MusicLibrary library;

    LibraryStoreFixture();
  };
} // namespace ao::library::test
