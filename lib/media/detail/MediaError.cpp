// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MediaError.h"

#include <ao/Contract.h>
#include <ao/Error.h>

#include <source_location>
#include <string>
#include <utility>

namespace ao::media::detail
{
  [[noreturn]] void throwMediaError(Error error)
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    throw MediaException{std::move(error)};
  }

  [[noreturn]] void throwMediaError(Error::Code const code, std::string message, std::source_location const loc)
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    throw MediaException{code, std::move(message), loc};
  }
} // namespace ao::media::detail
