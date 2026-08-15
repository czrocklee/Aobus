// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/lmdb/LmdbTestSupport.h"

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
  std::vector<std::byte> createTestData(std::size_t const size)
  {
    auto data = std::vector<std::byte>(size);

    for (std::size_t i = 0; i < size; ++i)
    {
      data[i] = static_cast<std::byte>(i % 256);
    }

    return data;
  }

  std::vector<std::byte> createStringData(std::string_view const str)
  {
    auto const bytes = utility::bytes::view(str);
    return {bytes.begin(), bytes.end()};
  }

  Environment openEnvironment(std::filesystem::path const& path, Environment::Options const& options)
  {
    auto result = Environment::open(path, options);
    REQUIRE(result);
    return std::move(*result);
  }

  ReadTransaction beginReadTransaction(Environment const& env)
  {
    auto result = ReadTransaction::begin(env);
    REQUIRE(result);
    return std::move(*result);
  }

  WriteTransaction beginWriteTransaction(Environment& env)
  {
    auto result = WriteTransaction::begin(env);
    REQUIRE(result);
    return std::move(*result);
  }

  IntegerKeyDatabase openIntegerKeyDatabase(WriteTransaction& txn, std::string const& name)
  {
    auto result = IntegerKeyDatabase::open(txn, name);
    REQUIRE(result);
    return std::move(*result);
  }

  ByteKeyDatabase openByteKeyDatabase(WriteTransaction& txn, std::string const& name)
  {
    auto result = ByteKeyDatabase::open(txn, name);
    REQUIRE(result);
    return std::move(*result);
  }
} // namespace ao::lmdb::test
