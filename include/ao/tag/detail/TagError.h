// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/Exception.h>

#include <source_location>
#include <string>
#include <utility>

namespace ao::tag::detail
{
  // Short-range control-flow exception internal to the tag-parsing layer. It
  // carries a recoverable Error raised with throwTagError and caught once at the
  // per-format Result boundary (flac/mp4/mpeg::File::loadTrackImpl). It replaces
  // direct throws of the public ao::Exception base from corruption-detection
  // sites (id3v2::FrameView ctor + layout<>()) and the std::out_of_range throws
  // from std::array::at() in mpeg::Frame.cpp table accesses. Implementation
  // detail (detail/ and ao::tag::detail): never catch it outside the tag
  // subsystem, keep the boundary catch narrow to this leaf so a non-domain fault
  // (e.g. std::bad_alloc or a real invariant throw from a transitive helper)
  // fail-fast terminates instead of being laundered into CorruptData. With the
  // std::out_of_range catch removed, a stray std::out_of_range that somehow
  // escapes a future helper likewise terminates rather than being silently
  // reinterpreted as parse corruption.
  class TagException final : public Exception
  {
  public:
    explicit TagException(Error error)
      : Exception{error.message, error.location}, _error{std::move(error)}
    {
    }

    TagException(Error::Code code, std::string message, std::source_location loc = std::source_location::current())
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
  [[noreturn]] inline void throwTagError(Error error)
  {
    throw TagException{std::move(error)};
  }

  [[noreturn]] inline void throwTagError(Error::Code code,
                                         std::string message,
                                         std::source_location loc = std::source_location::current())
  {
    throw TagException{code, std::move(message), loc};
  }
} // namespace ao::tag::detail
