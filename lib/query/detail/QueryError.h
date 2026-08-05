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

namespace ao::query::detail
{
  // Short-range control-flow exception internal to the query-compilation layer.
  // It carries a recoverable Error raised with throwQueryError and caught once
  // at the Result boundary (QueryCompiler::compile, FormatCompiler::compile).
  // It replaces the previous pattern of threading Result<T> through every
  // private compile helper.
  //
  // Implementation detail (detail/ and ao::query::detail): never catch it
  // outside the query subsystem; keep the boundary catch narrow to this leaf
  // so a non-domain fault (e.g. std::bad_alloc or a real invariant throw from
  // a transitive helper) fail-fast terminates instead of being laundered into
  // FormatRejected.
  class QueryException final : public Exception
  {
  public:
    explicit QueryException(Error error)
      : Exception{error.message, error.location}, _error{std::move(error)}
    {
    }

    QueryException(Error::Code code, std::string message, std::source_location loc = std::source_location::current())
      : Exception{message, loc}, _error{.code = code, .message = std::move(message), .location = loc}
    {
    }

    Error const& error() const noexcept { return _error; }

  private:
    Error _error;
  };

  // Re-throws an already-constructed Error, preserving its original source
  // location. Use when propagating an Error from an inner Result; the
  // format-string overloads below capture the call site for a fresh failure.
  [[noreturn]] inline void throwQueryError(Error error)
  {
    throw QueryException{std::move(error)};
  }

  // Format-string capture helper (mirrors ao::FormatWithLocation in
  // <ao/Exception.h>). The consteval ctor forces compile-time format
  // verification and lets the embedded std::format_string<Args...> bypass
  // GCC's explicit-ctor copy-init issue that arises when a format_string
  // is matched as a function parameter.
  template<typename... Args>
  struct FormatWithLocation
  {
    std::format_string<Args...> fmt;
    std::source_location loc;

    template<typename T>
    consteval FormatWithLocation(T const& fmtStr, std::source_location location = std::source_location::current())
      : fmt{fmtStr}, loc{location}
    {
    }
  };

  // Plain (no format args) overload — message is a literal string. The
  // format overload below is selected only when extra format args are
  // provided, so the two never overlap. Uses std::string_view rather than
  // std::format_string<> to avoid the explicit-ctor copy-init issue.
  [[noreturn]] inline void throwQueryError(std::string_view what,
                                           std::source_location loc = std::source_location::current())
  {
    throw QueryException{Error::Code::FormatRejected, std::string{what}, loc};
  }

  // Format-string overload. The query layer only ever raises
  // FormatRejected, so the code is hard-coded here; callers pass the format
  // string and args inline. The type_identity_t<Args>... wrapper makes the
  // format parameter a non-deduced context so Args is deduced solely from
  // Args&&... args below.
  template<typename... Args>
    requires(sizeof...(Args) > 0)
  [[noreturn]] inline void throwQueryError(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args)
  {
    throw QueryException{Error::Code::FormatRejected, std::format(fmt.fmt, std::forward<Args>(args)...), fmt.loc};
  }
} // namespace ao::query::detail
