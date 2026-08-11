// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ao::lmdb::test
{
  /**
   * Create a vector filled with test data.
   */
  std::vector<std::byte> createTestData(std::size_t size);

  /**
   * Simple string data for testing.
   */
  std::vector<std::byte> createStringData(std::string_view str);

  Environment openEnvironment(std::filesystem::path const& path, Environment::Options const& options = {});

  ReadTransaction beginReadTransaction(Environment const& env);

  WriteTransaction beginWriteTransaction(Environment& env);

  IntegerKeyDatabase openIntegerKeyDatabase(WriteTransaction& txn, std::string const& name);

  ByteKeyDatabase openByteKeyDatabase(WriteTransaction& txn, std::string const& name);
} // namespace ao::lmdb::test
