// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/Exception.h>

#include <source_location>
#include <string>
#include <utility>

namespace ao::audio::detail
{
  // Short-range control-flow exception internal to the decoder/source layer. It
  // carries a recoverable Error that is raised with throwDecoderError and caught
  // once at the subsystem's noexcept Result boundary, replacing repeated
  // `if (!result) return std::unexpected{...}` plumbing. It is an implementation
  // detail (hence detail/ and ao::audio::detail): never catch it outside the
  // audio subsystem, and keep the boundary catch narrow to this leaf type so a
  // non-domain fault (e.g. std::bad_alloc) fail-fast terminates rather than being
  // laundered into a recoverable decode error.
  class DecoderException final : public Exception
  {
  public:
    explicit DecoderException(Error error)
      : Exception{error.message, error.location}, _error{std::move(error)}
    {
    }

    DecoderException(Error::Code code, std::string message, std::source_location loc = std::source_location::current())
      : Exception{message, loc}, _error{.code = code, .message = std::move(message), .location = loc}
    {
    }

    Error const& error() const noexcept { return _error; }

  private:
    Error _error;
  };

  // Re-throws an already-constructed Error, preserving its original source
  // location. Use this when propagating an Error obtained from an inner Result;
  // the (code, message) overload below instead captures the call site, which is
  // what you want when originating a fresh failure.
  [[noreturn]] inline void throwDecoderError(Error error)
  {
    throw DecoderException{std::move(error)};
  }

  [[noreturn]] inline void throwDecoderError(Error::Code code,
                                             std::string message,
                                             std::source_location loc = std::source_location::current())
  {
    throw DecoderException{code, std::move(message), loc};
  }
} // namespace ao::audio::detail
