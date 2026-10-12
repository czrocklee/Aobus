// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
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

  // Independently owns a native readonly environment, without layout repair.
  // All other environments for this path must already be closed. Missing keys
  // return nullopt; a present empty value returns an engaged empty vector.
  // Missing databases, incompatible flags and native faults are fixture errors
  // reported as std::runtime_error; they are never treated as missing records.
  std::optional<std::vector<std::byte>> readExistingIntegerKeyRecord(std::filesystem::path const& path,
                                                                     std::string const& databaseName,
                                                                     std::uint32_t id);
} // namespace ao::lmdb::test
