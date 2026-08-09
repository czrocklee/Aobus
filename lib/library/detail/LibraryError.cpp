// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryError.h"

#include <ao/Contract.h>
#include <ao/Error.h>

#include <source_location>
#include <string>
#include <utility>

namespace ao::library::detail
{
  [[noreturn]] void throwLibraryError(Error error)
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    throw LibraryException{std::move(error)};
  }

  [[noreturn]] void throwLibraryError(Error::Code const code, std::string message, std::source_location const loc)
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    throw LibraryException{code, std::move(message), loc};
  }
} // namespace ao::library::detail
