// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "WritableLibraryTestSupport.h"

#include "test/unit/TestFixtureSupport.h"
#include <ao/library/MusicLibrary.h>
#include <ao/library/WriteTransaction.h>

#include <utility>

namespace ao::library::test
{
  WritableMusicLibrary requireWritableLibrary(MusicLibrary& library)
  {
    return ao::test::requireValue(WritableMusicLibrary::acquire(library));
  }

  WriteTransaction writeTransaction(MusicLibrary& library, WriteTransaction::Options options)
  {
    auto writableLibrary = requireWritableLibrary(library);
    return writableLibrary.writeTransaction(std::move(options));
  }
} // namespace ao::library::test
