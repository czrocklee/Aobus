// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/lmdb/LmdbTestSupport.h"

#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Path.h>
#include <ao/utility/Raii.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
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
    auto res = Environment::open(path, options);
    REQUIRE(res);
    return std::move(*res);
  }

  ReadTransaction beginReadTransaction(Environment const& env)
  {
    auto res = ReadTransaction::begin(env);
    REQUIRE(res);
    return std::move(*res);
  }

  WriteTransaction beginWriteTransaction(Environment& env)
  {
    auto res = WriteTransaction::begin(env);
    REQUIRE(res);
    return std::move(*res);
  }

  IntegerKeyDatabase openIntegerKeyDatabase(WriteTransaction& txn, std::string const& name)
  {
    auto res = IntegerKeyDatabase::open(txn, name);
    REQUIRE(res);
    return std::move(*res);
  }

  ByteKeyDatabase openByteKeyDatabase(WriteTransaction& txn, std::string const& name)
  {
    auto res = ByteKeyDatabase::open(txn, name);
    REQUIRE(res);
    return std::move(*res);
  }

  std::optional<std::vector<std::byte>> readExistingIntegerKeyRecord(std::filesystem::path const& path,
                                                                     std::string const& databaseName,
                                                                     std::uint32_t const id)
  {
    auto const requireNative = [](int const code)
    {
      if (code != MDB_SUCCESS)
      {
        throw std::runtime_error{std::format("Readonly LMDB fixture inspection: {}", ::mdb_strerror(code))};
      }
    };

    ::MDB_env* environment = nullptr;
    requireNative(::mdb_env_create(&environment));
    auto environmentPtr = utility::makeUniquePtr<::mdb_env_close>(environment);
    // Only the requested DBI is opened, regardless of the catalog's size.
    requireNative(::mdb_env_set_maxdbs(environment, 1));
    auto const utf8Path = utility::pathToUtf8(path);
    requireNative(::mdb_env_open(environment, utf8Path.c_str(), MDB_RDONLY | MDB_NOTLS, 0644));
    ::MDB_txn* transaction = nullptr;
    requireNative(::mdb_txn_begin(environment, nullptr, MDB_RDONLY, &transaction));
    auto transactionPtr = utility::makeUniquePtr<::mdb_txn_abort>(transaction);
    ::MDB_dbi database = 0;
    requireNative(::mdb_dbi_open(transaction, databaseName.c_str(), 0, &database));
    unsigned int flags = 0;
    requireNative(::mdb_dbi_flags(transaction, database, &flags));

    if (flags != MDB_INTEGERKEY)
    {
      throw std::runtime_error{"Readonly LMDB fixture inspection requires exact MDB_INTEGERKEY flags"};
    }

    // The persisted key is a native uint32, not lexical text or size_t.
    auto keyValue = id;
    auto key = ::MDB_val{.mv_size = sizeof(keyValue), .mv_data = &keyValue};
    auto value = ::MDB_val{};
    auto const code = ::mdb_get(transaction, database, &key, &value);

    if (code == MDB_NOTFOUND)
    {
      return std::nullopt;
    }

    requireNative(code);

    if (value.mv_size == 0)
    {
      return std::vector<std::byte>{};
    }

    auto const* bytes = static_cast<std::byte const*>(value.mv_data);
    return std::vector<std::byte>{bytes, bytes + value.mv_size};
  }
} // namespace ao::lmdb::test
