// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

namespace ao
{
  struct Error final
  {
    enum class Code : std::uint8_t
    {
      Generic,
      NotFound,
      DeviceNotFound,
      FormatRejected,
      InitFailed,
      IoError,
      DecodeFailed,
      SeekFailed,
      NotSupported,
      InvalidState,
    };

    Code code = Code::Generic;
    std::string message{};
  };

  template<typename T = void>
  using Result = std::expected<T, Error>;

  inline std::unexpected<Error> makeError(Error::Code code, std::string message)
  {
    return std::unexpected(Error{.code = code, .message = std::move(message)});
  }
} // namespace ao
