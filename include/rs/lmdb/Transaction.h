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
  class ReadTransaction
  {
  public:
    ReadTransaction(const lmdb::Environment& env) : _txn{lmdb::Transaction::begin(env, nullptr, MDB_RDONLY)} {}

    ReadTransaction(ReadTransaction&&) = default;

  protected:
    ReadTransaction(lmdb::Transaction&& txn) : _txn{std::move(txn)} {}

    lmdb::Transaction _txn;
    friend class Database;
  };

  class WriteTransaction : public ReadTransaction
  {
  public:
    WriteTransaction(lmdb::Environment& env) : ReadTransaction{lmdb::Transaction::begin(env, nullptr)} {}

    WriteTransaction(WriteTransaction& parent)
      : ReadTransaction{lmdb::Transaction::begin(parent._txn.environment(), parent._txn.raw())}
    {
    }

    WriteTransaction(WriteTransaction&&) = default;

    ~WriteTransaction()
    {
      if (_toCommit) { _txn.commit(); }
    }

    void commit() { _toCommit = true; }

  private:
    friend class Database;
    bool _toCommit = false;
  };
}
