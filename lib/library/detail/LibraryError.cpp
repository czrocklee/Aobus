// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryError.h"

#include <ao/Contract.h>
#include <ao/Error.h>

#include <cstddef>
#include <format>
#include <source_location>
#include <string>
#include <utility>

namespace ao::library::detail
{
  Error oversizedResourceError(std::size_t const byteLength, std::source_location const loc)
  {
    return {.code = Error::Code::ValueTooLarge,
            .message = std::format("Resource content of {} bytes exceeds the stored length field", byteLength),
            .location = loc};
  }

  [[noreturn]] void throwLibraryError(Error error)
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    throw LibraryException{std::move(error)};
  }

  [[noreturn]] void throwLibraryError(Error::Code const code, std::string message, std::source_location const loc)
  {
    throwLibraryError(Error{.code = code, .message = std::move(message), .location = loc});
  }
} // namespace ao::library::detail
