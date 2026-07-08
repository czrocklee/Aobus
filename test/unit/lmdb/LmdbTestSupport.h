// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::lmdb::test
{
  /**
   * Create a vector filled with test data.
   */
  inline std::vector<std::byte> createTestData(std::size_t size)
  {
    auto data = std::vector<std::byte>(size);

    for (std::size_t i = 0; i < size; ++i)
    {
      data[i] = static_cast<std::byte>(i % 256);
    }

    return data;
  }

  /**
   * Simple string data for testing.
   */
  inline std::vector<std::byte> createStringData(std::string_view str)
  {
    auto bytes = utility::bytes::view(str);
    return {bytes.begin(), bytes.end()};
  }

  inline Environment openEnvironment(std::filesystem::path const& path, Environment::Options const& options = {})
  {
    auto result = Environment::open(path.string(), options);
    REQUIRE(result);
    return std::move(*result);
  }

  inline ReadTransaction beginReadTransaction(Environment const& env)
  {
    auto result = ReadTransaction::begin(env);
    REQUIRE(result);
    return std::move(*result);
  }

  inline WriteTransaction beginWriteTransaction(Environment& env)
  {
    auto result = WriteTransaction::begin(env);
    REQUIRE(result);
    return std::move(*result);
  }

  inline WriteTransaction beginWriteTransaction(WriteTransaction& parent)
  {
    auto result = WriteTransaction::begin(parent);
    REQUIRE(result);
    return std::move(*result);
  }

  inline Database openDatabase(WriteTransaction& txn,
                               std::string const& name,
                               Database::KeyKind kind = Database::KeyKind::Integer)
  {
    auto result = Database::open(txn, name, kind);
    REQUIRE(result);
    return std::move(*result);
  }

  inline Database openDatabase(ReadTransaction& txn,
                               std::string const& name,
                               Database::KeyKind kind = Database::KeyKind::Integer)
  {
    auto result = Database::open(txn, name, kind);
    REQUIRE(result);
    return std::move(*result);
  }
} // namespace ao::lmdb::test
