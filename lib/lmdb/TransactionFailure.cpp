// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "detail/TransactionFailure.h"

#include <ao/Contract.h>
#include <ao/Error.h>

#include <utility>

namespace ao::lmdb::detail
{
  [[noreturn]] void throwTransactionFailure(Error error)
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    throw TransactionFailure{std::move(error)};
  }
} // namespace ao::lmdb::detail
