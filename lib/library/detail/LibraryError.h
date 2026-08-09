// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <exception>
#include <source_location>
#include <string>
#include <utility>

namespace ao::library::detail
{
  // Short-range control-flow exception internal to the library store/serialization
  // layer. Carries a recoverable Error raised with throwLibraryError across a
  // short implementation-only call chain. Catch it only at the nearest
  // ao::library-owned Result boundary that owns the complete operation. Runtime,
  // CLI, and frontend code must never name this type. Keep the catch narrow so a
  // non-domain fault (e.g. std::bad_alloc) is never laundered into a recoverable
  // code.
  class LibraryException final : public std::exception
  {
  public:
    explicit LibraryException(Error error)
      : _error{std::move(error)}
    {
    }

    LibraryException(Error::Code code, std::string message, std::source_location loc = std::source_location::current())
      : _error{.code = code, .message = std::move(message), .location = loc}
    {
    }

    char const* what() const noexcept override { return _error.message.c_str(); }
    Error const& error() const noexcept { return _error; }

  private:
    Error _error;
  };

  // Re-throws an already-constructed Error, preserving its original source
  // location. Use when propagating an Error from an inner Result; the
  // (code, message) overload instead captures the call site for a fresh failure.
  [[noreturn]] void throwLibraryError(Error error);

  [[noreturn]] void throwLibraryError(Error::Code code,
                                      std::string message,
                                      std::source_location loc = std::source_location::current());
} // namespace ao::library::detail
