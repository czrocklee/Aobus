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

#include <rs/lmdb/Type.h>
#include <rs/lmdb/Transaction.h>

namespace rs::lmdb
{
  // Error implementation
  detail::Error::Error(std::string origin, int code)
    : std::runtime_error{origin + ": " + mdb_strerror(code)}
    , _origin{std::move(origin)}
    , _code{code}
  {
  }

  void detail::Error::raise(const char* origin, int code) { throw Error{origin, code}; }

  // Environment implementation
  Environment Environment::create(unsigned int flags)
  {
    MDB_env* handle = nullptr;
    throwOnError("mdb_env_create", mdb_env_create(&handle));
    if (flags != 0)
    {
      try
      {
        throwOnError("mdb_env_set_flags", mdb_env_set_flags(handle, flags, 1));
      }
      catch (...)
      {
        mdb_env_close(handle);
        throw;
      }
    }

    return Environment{handle};
  }

  Environment::Environment() = default;

  Environment::Environment(MDB_env* handle) noexcept : _handle{handle} {}

  Environment::Environment(Environment&& other) noexcept : _handle{other._handle} { other._handle = nullptr; }

  Environment& Environment::operator=(Environment&& other) noexcept
  {
    if (this != &other)
    {
      close();
      _handle = other._handle;
      other._handle = nullptr;
    }

    return *this;
  }

  Environment::~Environment() noexcept { close(); }

  Environment& Environment::open(const char* path, unsigned int flags, mdb_mode_t mode)
  {
    throwOnError("mdb_env_open", mdb_env_open(_handle, path, flags, mode));
    return *this;
  }

  Environment& Environment::setMapSize(std::size_t size)
  {
    throwOnError("mdb_env_set_mapsize", mdb_env_set_mapsize(_handle, size));
    return *this;
  }

  Environment& Environment::setMaxDatabases(MDB_dbi count)
  {
    throwOnError("mdb_env_set_maxdbs", mdb_env_set_maxdbs(_handle, count));
    return *this;
  }

  Environment& Environment::setMaxReaders(unsigned int count)
  {
    throwOnError("mdb_env_set_maxreaders", mdb_env_set_maxreaders(_handle, count));
    return *this;
  }

  void Environment::close() noexcept
  {
    if (_handle != nullptr)
    {
      mdb_env_close(_handle);
      _handle = nullptr;
    }
  }

  // Cursor implementation
  detail::Cursor detail::Cursor::open(MDB_txn* transaction, MDB_dbi database)
  {
    MDB_cursor* handle = nullptr;
    throwOnError("mdb_cursor_open", mdb_cursor_open(transaction, database, &handle));
    return detail::Cursor{handle};
  }

  detail::Cursor detail::Cursor::open(ReadTransaction& transaction, MDB_dbi database)
  {
    return open(transaction.raw(), database);
  }

  detail::Cursor detail::Cursor::open(WriteTransaction& transaction, MDB_dbi database)
  {
    return open(transaction.raw(), database);
  }

  detail::Cursor::Cursor() = default;

  detail::Cursor::Cursor(MDB_cursor* handle) noexcept : _handle{handle} {}

  detail::Cursor::Cursor(Cursor&& other) noexcept : _handle{other._handle} { other._handle = nullptr; }

  detail::Cursor& detail::Cursor::operator=(Cursor&& other) noexcept
  {
    if (this != &other)
    {
      close();
      _handle = other._handle;
      other._handle = nullptr;
    }

    return *this;
  }

  detail::Cursor::~Cursor() noexcept { close(); }

  void detail::Cursor::close() noexcept
  {
    if (_handle != nullptr)
    {
      mdb_cursor_close(_handle);
      _handle = nullptr;
    }
  }

  bool detail::Cursor::get(std::string_view& key, MDB_cursor_op op) const
  {
    MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
    const int rc = mdb_cursor_get(_handle, &keyValue, nullptr, op);
    if (rc == MDB_NOTFOUND)
    {
      return false;
    }
    throwOnError("mdb_cursor_get", rc);
    key = {static_cast<const char*>(keyValue.mv_data), keyValue.mv_size};
    return true;
  }

  bool detail::Cursor::get(std::string_view& key, std::string_view& value, MDB_cursor_op op) const
  {
    MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
    MDB_val valueBuffer{value.size(), const_cast<char*>(value.data())};
    const int rc = mdb_cursor_get(_handle, &keyValue, &valueBuffer, op);
    if (rc == MDB_NOTFOUND)
    {
      return false;
    }
    throwOnError("mdb_cursor_get", rc);
    key = {static_cast<const char*>(keyValue.mv_data), keyValue.mv_size};
    value = {static_cast<const char*>(valueBuffer.mv_data), valueBuffer.mv_size};
    return true;
  }

  bool detail::Cursor::put(std::string_view key, std::string_view value, unsigned int flags) const
  {
    MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
    MDB_val valueBuffer{value.size(), const_cast<char*>(value.data())};
    const int rc = mdb_cursor_put(_handle, &keyValue, &valueBuffer, flags);
    if (rc == MDB_KEYEXIST)
    {
      return false;
    }
    throwOnError("mdb_cursor_put", rc);
    return true;
  }

  void* detail::Cursor::reserve(std::string_view key, std::size_t size, unsigned int flags) const
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - LMDB C API requires non-const
    MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
    MDB_val valueBuffer{size, nullptr};
    throwOnError("mdb_cursor_put", mdb_cursor_put(_handle, &keyValue, &valueBuffer, flags | MDB_RESERVE));
    return valueBuffer.mv_data;
  }
}
