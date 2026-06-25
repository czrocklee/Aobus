// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/Exception.h>

#include <format>
#include <optional>
#include <source_location>
#include <string_view>
#include <utility>

namespace ao::rt
{
  template<typename T>
  std::optional<T> storageValueOrNullopt(Result<T> result,
                                         std::string_view action,
                                         std::source_location /*loc*/ = std::source_location::current())
  {
    if (result)
    {
      return std::optional<T>{std::move(*result)};
    }

    if (result.error().code == Error::Code::NotFound)
    {
      return std::nullopt;
    }

    auto const& error = result.error();
    auto const message = std::format("{}: {}", action, error.message);
    throwException<Exception>(std::string_view{message}, error.location);
  }

  // Identity overload for stores whose only recoverable miss is absence: the
  // value already arrives as std::optional (non-NotFound storage faults threw
  // deeper at the lmdb layer), so there is nothing left to translate. The
  // action label is kept only so call sites read the same regardless of whether
  // the store returns Result<T> or std::optional<T>.
  template<typename T>
  std::optional<T> storageValueOrNullopt(std::optional<T> optValue,
                                         std::string_view /*action*/ = {},
                                         std::source_location /*loc*/ = std::source_location::current())
  {
    return optValue;
  }
} // namespace ao::rt
