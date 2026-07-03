// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/Exception.h>

#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ao::cli
{
  class CommandError final : public Exception
  {
  public:
    explicit CommandError(Error error)
      : Exception{error.message, error.location}, _error{std::move(error)}
    {
    }

    CommandError(Error::Code code, std::string message, std::source_location loc = std::source_location::current())
      : Exception{message, loc}, _error{.code = code, .message = std::move(message), .location = loc}
    {
    }

    Error const& error() const noexcept { return _error; }
    Error::Code code() const noexcept { return _error.code; }

  private:
    Error _error;
  };

  [[noreturn]] inline void throwCommandError(Error error)
  {
    throw CommandError{std::move(error)};
  }

  template<typename... Args>
    requires(sizeof...(Args) > 0)
  [[noreturn]] inline void throwCommandError(Error error,
                                             FormatWithLocation<std::type_identity_t<Args>...> fmt,
                                             Args&&... args)
  {
    error.message = std::format(fmt.fmt, std::forward<Args>(args)...);
    throw CommandError{std::move(error)};
  }

  [[noreturn]] inline void throwCommandError(Error::Code code,
                                             std::string_view message,
                                             std::source_location loc = std::source_location::current())
  {
    throw CommandError{code, std::string{message}, loc};
  }

  template<typename... Args>
    requires(sizeof...(Args) > 0)
  [[noreturn]] inline void throwCommandError(Error::Code code,
                                             FormatWithLocation<std::type_identity_t<Args>...> fmt,
                                             Args&&... args)
  {
    throw CommandError{code, std::format(fmt.fmt, std::forward<Args>(args)...), fmt.loc};
  }
} // namespace ao::cli
