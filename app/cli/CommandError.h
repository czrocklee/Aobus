// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/utility/StrongTypeFormatter.h>

#include <exception>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ao::cli
{
  template<typename... Args>
  struct CommandFormat final
  {
    std::format_string<Args...> format;
    std::source_location location;

    template<typename T>
    consteval CommandFormat(T const& formatString,
                            std::source_location sourceLocation = std::source_location::current())
      : format{formatString}, location{sourceLocation}
    {
    }
  };

  class CommandError final : public std::exception
  {
  public:
    explicit CommandError(Error error)
      : _error{std::move(error)}
    {
    }

    CommandError(Error::Code code, std::string message, std::source_location loc = std::source_location::current())
      : _error{.code = code, .message = std::move(message), .location = loc}
    {
    }

    char const* what() const noexcept override { return _error.message.c_str(); }
    Error const& error() const noexcept { return _error; }
    Error::Code code() const noexcept { return _error.code; }

  private:
    Error _error;
  };

  [[noreturn]] void throwCommandError(Error error);

  template<typename... Args>
    requires(sizeof...(Args) > 0)
  [[noreturn]] inline void throwCommandError(Error error,
                                             CommandFormat<std::type_identity_t<Args>...> fmt,
                                             Args&&... args)
  {
    AO_EXCEPTION_CARRIER(CommandBoundary);
    error.message = std::format(fmt.format, std::forward<Args>(args)...);
    throw CommandError{std::move(error)};
  }

  [[noreturn]] void throwCommandError(Error::Code code,
                                      std::string_view message,
                                      std::source_location loc = std::source_location::current());

  template<typename... Args>
    requires(sizeof...(Args) > 0)
  [[noreturn]] inline void throwCommandError(Error::Code code,
                                             CommandFormat<std::type_identity_t<Args>...> fmt,
                                             Args&&... args)
  {
    AO_EXCEPTION_CARRIER(CommandBoundary);
    throw CommandError{code, std::format(fmt.format, std::forward<Args>(args)...), fmt.location};
  }
} // namespace ao::cli
