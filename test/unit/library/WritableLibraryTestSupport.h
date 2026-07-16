// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/unit/TestUtils.h"
#include <ao/library/MusicLibrary.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>

#include <utility>

namespace ao::library::test
{
  inline WritableMusicLibrary requireWritableLibrary(MusicLibrary& library)
  {
    return ao::test::requireValue(WritableMusicLibrary::acquire(library));
  }

  inline WriteTransaction writeTransaction(MusicLibrary& library, WriteTransaction::Options options = {})
  {
    auto writableLibrary = requireWritableLibrary(library);
    return writableLibrary.writeTransaction(std::move(options));
  }
} // namespace ao::library::test
