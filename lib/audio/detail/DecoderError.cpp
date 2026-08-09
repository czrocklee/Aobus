// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "DecoderError.h"

#include <ao/Contract.h>
#include <ao/Error.h>

#include <source_location>
#include <string>
#include <utility>

namespace ao::audio::detail
{
  [[noreturn]] void throwDecoderError(Error error)
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    throw DecoderException{std::move(error)};
  }

  [[noreturn]] void throwDecoderError(Error::Code const code, std::string message, std::source_location const loc)
  {
    AO_EXCEPTION_CARRIER(PrivateErrorTransport);
    throw DecoderException{code, std::move(message), loc};
  }
} // namespace ao::audio::detail
