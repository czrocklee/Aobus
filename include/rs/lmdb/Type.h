/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <lmdb.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rs::lmdb
{
  class Error : public std::runtime_error
  {
  public:
    Error(std::string origin, int code);
    static void raise(const char* origin, int code);

    [[nodiscard]] int code() const noexcept { return _code; }
    [[nodiscard]] const std::string& origin() const noexcept { return _origin; }

  private:
    std::string _origin;
    int _code;
  };

  inline void throwOnError(const char* origin, int code)
  {
    if (code != MDB_SUCCESS)
    {
      Error::raise(origin, code);
    }
  }

  class Environment
  {
  public:
    static constexpr unsigned int DefaultFlags = 0;
    static constexpr mdb_mode_t DefaultMode = 0644;

    static Environment create(unsigned int flags = DefaultFlags);
    Environment();
    explicit Environment(MDB_env* handle) noexcept;

    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;

    Environment(Environment&& other) noexcept;
    Environment& operator=(Environment&& other) noexcept;

    ~Environment() noexcept;

    [[nodiscard]] MDB_env* raw() const noexcept { return _handle; }

    Environment& open(const char* path, unsigned int flags = DefaultFlags, mdb_mode_t mode = DefaultMode);
    Environment& setMapSize(std::size_t size);
    Environment& setMaxDatabases(MDB_dbi count);
    Environment& setMaxReaders(unsigned int count);
    void close() noexcept;

  private:
    MDB_env* _handle = nullptr;
  };

  class Transaction
  {
  public:
    static Transaction begin(MDB_env* env, MDB_txn* parent = nullptr, unsigned int flags = 0);
    static Transaction begin(const Environment& env, MDB_txn* parent = nullptr, unsigned int flags = 0);

    Transaction();
    explicit Transaction(MDB_txn* handle) noexcept;

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&& other) noexcept;

    ~Transaction() noexcept;

    [[nodiscard]] MDB_txn* raw() const noexcept { return _handle; }
    [[nodiscard]] MDB_env* environment() const noexcept { return _handle != nullptr ? mdb_txn_env(_handle) : nullptr; }

    void commit();
    void abort() noexcept;

  private:
    MDB_txn* _handle = nullptr;
  };

  class MDB
  {
  public:
    static constexpr unsigned int DefaultFlags = 0;

    static MDB open(Transaction& transaction, const char* name = nullptr, unsigned int flags = DefaultFlags);
    static MDB open(Transaction& transaction, std::string_view name, unsigned int flags = DefaultFlags);

    MDB();
    explicit MDB(MDB_dbi handle) noexcept;

    [[nodiscard]] MDB_dbi raw() const noexcept { return _handle; }

    [[nodiscard]] bool get(Transaction& transaction, std::string_view key, std::string_view& value) const;
    [[nodiscard]] bool put(Transaction& transaction, std::string_view key, std::string_view value, unsigned int flags = 0) const;
    [[nodiscard]] bool del(Transaction& transaction, std::string_view key) const;

  private:
    MDB_dbi _handle = (std::numeric_limits<MDB_dbi>::max)();
  };

  class Cursor
  {
  public:
    static Cursor open(MDB_txn* transaction, MDB_dbi database);
    static Cursor open(Transaction& transaction, MDB database);

    Cursor();
    explicit Cursor(MDB_cursor* handle) noexcept;

    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    Cursor(Cursor&& other) noexcept;
    Cursor& operator=(Cursor&& other) noexcept;

    ~Cursor() noexcept;

    [[nodiscard]] MDB_cursor* raw() const noexcept { return _handle; }
    [[nodiscard]] MDB_txn* transaction() const noexcept
    {
      return _handle != nullptr ? mdb_cursor_txn(_handle) : nullptr;
    }
    [[nodiscard]] MDB_dbi database() const noexcept { return _handle != nullptr ? mdb_cursor_dbi(_handle) : 0; }
    [[nodiscard]] bool valid() const noexcept { return _handle != nullptr; }

    void close() noexcept;

    [[nodiscard]] bool get(std::string_view& key, MDB_cursor_op op) const;
    [[nodiscard]] bool get(std::string_view& key, std::string_view& value, MDB_cursor_op op) const;
    [[nodiscard]] bool put(std::string_view key, std::string_view value, unsigned int flags = 0) const;
    [[nodiscard]] void* reserve(std::string_view key, std::size_t size, unsigned int flags = 0) const;

  private:
    MDB_cursor* _handle = nullptr;
  };

  template<typename T>
  [[nodiscard]] std::string_view bytesOf(const T& value)
  {
    return {reinterpret_cast<const char*>(std::addressof(value)), sizeof(value)};
  }

  template<typename T>
  [[nodiscard]] std::string_view bytesOf(const T* value)
  {
    return {reinterpret_cast<const char*>(value), sizeof(T)};
  }

  template<typename T>
  [[nodiscard]] T read(std::string_view bytes)
  {
    if (bytes.size() != sizeof(T))
    {
      Error::raise("lmdb::read", MDB_BAD_VALSIZE);
    }
    T value;
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
  }
}
