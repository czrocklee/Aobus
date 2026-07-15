// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/unit/TestUtils.h"
#include <ao/library/MusicLibrary.h>

namespace ao::library::test
{
  struct LibraryStoreFixture final
  {
    ao::test::TempDir temp;
    MusicLibrary library;

    LibraryStoreFixture()
      : temp{}, library{temp.path(), temp.path() / "db"}
    {
    }
  };
} // namespace ao::library::test
