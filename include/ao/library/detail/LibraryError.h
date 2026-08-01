// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/Exception.h>

#include <source_location>
#include <string>
#include <utility>

namespace ao::library::detail
{
  // Short-range control-flow exception internal to the library store/serialization
  // layer. Carries a recoverable Error raised with throwLibraryError across a
  // short implementation-only call chain. Catch it only at the nearest existing
  // Result, command, or task boundary that owns the complete library operation.
  // Keep the catch narrow to this type so a non-domain fault (e.g.
  // std::bad_alloc) is never laundered into a recoverable code.
  class LibraryException final : public Exception
  {
  public:
    explicit LibraryException(Error error)
      : Exception{error.message, error.location}, _error{std::move(error)}
    {
    }

    LibraryException(Error::Code code, std::string message, std::source_location loc = std::source_location::current())
      : Exception{message, loc}, _error{.code = code, .message = std::move(message), .location = loc}
    {
    }

    Error const& error() const noexcept { return _error; }

  private:
    Error _error;
  };

  // Re-throws an already-constructed Error, preserving its original source
  // location. Use when propagating an Error from an inner Result; the
  // (code, message) overload instead captures the call site for a fresh failure.
  [[noreturn]] inline void throwLibraryError(Error error)
  {
    throw LibraryException{std::move(error)};
  }

  [[noreturn]] inline void throwLibraryError(Error::Code code,
                                             std::string message,
                                             std::source_location loc = std::source_location::current())
  {
    throw LibraryException{code, std::move(message), loc};
  }
} // namespace ao::library::detail
