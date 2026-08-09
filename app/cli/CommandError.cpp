// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CommandError.h"

#include <ao/Contract.h>
#include <ao/Error.h>

#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace ao::cli
{
  [[noreturn]] void throwCommandError(Error error)
  {
    AO_EXCEPTION_CARRIER(CommandBoundary);
    throw CommandError{std::move(error)};
  }

  [[noreturn]] void throwCommandError(Error::Code const code,
                                      std::string_view const message,
                                      std::source_location const loc)
  {
    AO_EXCEPTION_CARRIER(CommandBoundary);
    throw CommandError{code, std::string{message}, loc};
  }
} // namespace ao::cli
