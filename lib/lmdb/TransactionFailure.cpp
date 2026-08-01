// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/lmdb/TransactionFailure.h>

#include <ao/Error.h>
#include <ao/Exception.h>

#include <utility>

namespace ao::lmdb
{
  [[noreturn]] void throwTransactionFailure(Error error)
  {
    throwException<TransactionFailure>(std::move(error));
  }
} // namespace ao::lmdb
