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

#include <rs/lmdb/Type.h>

namespace rs::lmdb
{
  class MusicLibrary;

  class ReadTransaction
  {
  public:
    ReadTransaction(const lmdb::Environment& env) : _txn{lmdb::detail::Transaction::begin(env, nullptr, MDB_RDONLY)} {}

    ReadTransaction(ReadTransaction&&) = default;

    [[nodiscard]] lmdb::detail::Transaction& transaction() noexcept { return _txn; }
    [[nodiscard]] const lmdb::detail::Transaction& transaction() const noexcept { return _txn; }

  protected:
    ReadTransaction(lmdb::detail::Transaction&& txn) : _txn{std::move(txn)} {}

    lmdb::detail::Transaction _txn;
    friend class Database;
  };

  class WriteTransaction : public ReadTransaction
  {
  public:
    WriteTransaction(lmdb::Environment& env) : ReadTransaction{lmdb::detail::Transaction::begin(env, nullptr)} {}

    WriteTransaction(WriteTransaction& parent)
      : ReadTransaction{lmdb::detail::Transaction::begin(parent._txn.environment(), parent._txn.raw())}
    {
    }

    WriteTransaction(WriteTransaction&&) = default;

    ~WriteTransaction()
    {
      if (_toCommit)
      {
        _txn.commit();
      }
    }

    void commit() { _toCommit = true; }

  private:
    friend class Database;
    bool _toCommit = false;
  };
}
