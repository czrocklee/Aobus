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

namespace rs::lmdb
{
  Error::Error(std::string origin, int code)
    : std::runtime_error{origin + ": " + mdb_strerror(code)}
    , _origin{std::move(origin)}
    , _code{code}
  {
  }

  void Error::raise(const char* origin, int code) { throw Error{origin, code}; }

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

  Transaction Transaction::begin(MDB_env* env, MDB_txn* parent, unsigned int flags)
  {
    MDB_txn* handle = nullptr;
    throwOnError("mdb_txn_begin", mdb_txn_begin(env, parent, flags, &handle));
    return Transaction{handle};
  }

  Transaction Transaction::begin(const Environment& env, MDB_txn* parent, unsigned int flags)
  {
    return begin(env.raw(), parent, flags);
  }

  Transaction::Transaction() = default;

  Transaction::Transaction(MDB_txn* handle) noexcept : _handle{handle} {}

  Transaction::Transaction(Transaction&& other) noexcept : _handle{other._handle} { other._handle = nullptr; }

  Transaction& Transaction::operator=(Transaction&& other) noexcept
  {
    if (this != &other)
    {
      abort();
      _handle = other._handle;
      other._handle = nullptr;
    }

    return *this;
  }

  Transaction::~Transaction() noexcept { abort(); }

  void Transaction::commit()
  {
    auto* handle = _handle;
    _handle = nullptr;
    throwOnError("mdb_txn_commit", mdb_txn_commit(handle));
  }

  void Transaction::abort() noexcept
  {
    if (_handle != nullptr)
    {
      mdb_txn_abort(_handle);
      _handle = nullptr;
    }
  }

  MDB MDB::open(Transaction& transaction, const char* name, unsigned int flags)
  {
    MDB_dbi handle = (std::numeric_limits<MDB_dbi>::max)();
    throwOnError("mdb_dbi_open", mdb_dbi_open(transaction.raw(), name, flags, &handle));
    return MDB{handle};
  }

  MDB MDB::open(Transaction& transaction, std::string_view name, unsigned int flags)
  {
    const std::string ownedName{name};
    return open(transaction, ownedName.c_str(), flags);
  }

  MDB::MDB() = default;

  MDB::MDB(MDB_dbi handle) noexcept : _handle{handle} {}

  bool MDB::get(Transaction& transaction, std::string_view key, std::string_view& value) const
  {
    MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
    MDB_val valueBuffer{0, nullptr};
    const int rc = mdb_get(transaction.raw(), _handle, &keyValue, &valueBuffer);
    if (rc == MDB_NOTFOUND)
    {
      return false;
    }
    throwOnError("mdb_get", rc);
    value = {static_cast<const char*>(valueBuffer.mv_data), valueBuffer.mv_size};
    return true;
  }

  bool MDB::put(Transaction& transaction, std::string_view key, std::string_view value, unsigned int flags) const
  {
    MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
    MDB_val valueBuffer{value.size(), const_cast<char*>(value.data())};
    const int rc = mdb_put(transaction.raw(), _handle, &keyValue, &valueBuffer, flags);
    if (rc == MDB_KEYEXIST)
    {
      return false;
    }
    throwOnError("mdb_put", rc);
    return true;
  }

  bool MDB::del(Transaction& transaction, std::string_view key) const
  {
    MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
    const int rc = mdb_del(transaction.raw(), _handle, &keyValue, nullptr);
    if (rc == MDB_NOTFOUND)
    {
      return false;
    }
    throwOnError("mdb_del", rc);
    return true;
  }

  Cursor Cursor::open(MDB_txn* transaction, MDB_dbi database)
  {
    MDB_cursor* handle = nullptr;
    throwOnError("mdb_cursor_open", mdb_cursor_open(transaction, database, &handle));
    return Cursor{handle};
  }

  Cursor Cursor::open(Transaction& transaction, MDB database) { return open(transaction.raw(), database.raw()); }

  Cursor::Cursor() = default;

  Cursor::Cursor(MDB_cursor* handle) noexcept : _handle{handle} {}

  Cursor::Cursor(Cursor&& other) noexcept : _handle{other._handle} { other._handle = nullptr; }

  Cursor& Cursor::operator=(Cursor&& other) noexcept
  {
    if (this != &other)
    {
      close();
      _handle = other._handle;
      other._handle = nullptr;
    }

    return *this;
  }

  Cursor::~Cursor() noexcept { close(); }

  void Cursor::close() noexcept
  {
    if (_handle != nullptr)
    {
      mdb_cursor_close(_handle);
      _handle = nullptr;
    }
  }

  bool Cursor::get(std::string_view& key, MDB_cursor_op op) const
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

  bool Cursor::get(std::string_view& key, std::string_view& value, MDB_cursor_op op) const
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

  bool Cursor::put(std::string_view key, std::string_view value, unsigned int flags) const
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

  void* Cursor::reserve(std::string_view key, std::size_t size, unsigned int flags) const
  {
    MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
    MDB_val valueBuffer{size, nullptr};
    throwOnError("mdb_cursor_put", mdb_cursor_put(_handle, &keyValue, &valueBuffer, flags | MDB_RESERVE));
    return valueBuffer.mv_data;
  }
}
