// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/Exception.h>

#include <string>
#include <utility>

namespace ao::lmdb::detail
{
  /**
   * Signals a write-operation failure that makes the owning transaction
   * unusable. The exception is control flow inside a transaction-owning
   * operation; the nearest library transaction owner explicitly aborts before
   * translating it to Result or propagating an infrastructure exception.
   */
  class TransactionFailure final : public Exception
  {
  public:
    explicit TransactionFailure(Error error)
      : Exception{error.message, error.location}, _error{std::move(error)}
    {
    }

    Error const& error() const noexcept { return _error; }

  private:
    Error _error;
  };

  [[noreturn]] void throwTransactionFailure(Error error);
} // namespace ao::lmdb::detail
