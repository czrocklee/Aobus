// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryStoreTestSupport.h"

#include "MusicLibraryTestSupport.h"

namespace ao::library::test
{
  LibraryStoreFixture::LibraryStoreFixture()
    : temp{}, library{makeTestMusicLibrary(temp.path(), temp.path() / "db")}
  {
  }
} // namespace ao::library::test
