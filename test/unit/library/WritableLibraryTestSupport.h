// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/library/WritableMusicLibrary.h>
#include <ao/library/WriteTransaction.h>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::library::test
{
  WritableMusicLibrary requireWritableLibrary(MusicLibrary& library);
  WriteTransaction writeTransaction(MusicLibrary& library, WriteTransaction::Options options = {});
} // namespace ao::library::test
