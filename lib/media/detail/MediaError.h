// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <exception>
#include <source_location>
#include <string>
#include <utility>

namespace ao::media::detail
{
  // Short-range control-flow exception internal to the media byte-view/layout
  // layer. Carries a recoverable Error raised with throwMediaError and caught
  // once at a format-parser Result boundary (currently MP4 decoder demuxing).
  // Implementation detail (detail/ + ao::media::detail): never catch
  // it outside a boundary that explicitly translates media-layer corruption;
  // keep the catch narrow to this leaf so a non-domain fault (e.g.
  // std::bad_alloc or a real invariant throw) fail-fast terminates instead of
  // being laundered into a recoverable code.
  class MediaException final : public std::exception
  {
  public:
    explicit MediaException(Error error)
      : _error{std::move(error)}
    {
    }

    MediaException(Error::Code code, std::string message, std::source_location loc = std::source_location::current())
      : _error{.code = code, .message = std::move(message), .location = loc}
    {
    }

    char const* what() const noexcept override { return _error.message.c_str(); }
    Error const& error() const noexcept { return _error; }

  private:
    Error _error;
  };

  [[noreturn]] void throwMediaError(Error error);

  [[noreturn]] void throwMediaError(Error::Code code,
                                    std::string message,
                                    std::source_location loc = std::source_location::current());
} // namespace ao::media::detail
