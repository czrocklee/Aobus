// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "QueryError.h"

#include <ao/Contract.h>
#include <ao/Error.h>

#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace ao::query::detail
{
  [[noreturn]] void throwQueryError(Error error)
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    throw QueryException{std::move(error)};
  }

  [[noreturn]] void throwQueryError(std::string_view const what, std::source_location const loc)
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    throw QueryException{Error::Code::FormatRejected, std::string{what}, loc};
  }
} // namespace ao::query::detail
