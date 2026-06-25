// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <expected>
#include <source_location>
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
      InvalidInput,
      CorruptData,
      FormatRejected,
      InitFailed,
      IoError,
      DecodeFailed,
      SeekFailed,
      NotSupported,
      InvalidState,
      Conflict,
      ValueTooLarge,
      ResourceExhausted,
    };

    Code code = Code::Generic;
    std::string message{};
    // Cold-path diagnostic only: where the failure was constructed. Captured for
    // logging, never shown to users and never part of the recoverable contract
    // (code + message). The default member initializer resolves to the site of a
    // direct aggregate `Error{...}`; makeError threads the caller's location in.
    std::source_location location = std::source_location::current();
  };

  template<typename T = void>
  class [[nodiscard]] Result : public std::expected<T, Error>
  {
  public:
    using Base = std::expected<T, Error>;

    // clang-tidy misattributes std::expected's inherited forwarding constructors
    // to this declaration and reports its internal rvalue references as unused.
    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
    using Base::Base;

    Result() = default;
    Result(Result const&) = default;
    Result(Result&&) = default;
    Result& operator=(Result const&) = default;
    Result& operator=(Result&&) = default;
    ~Result() = default;

    Result(Base const& value)
      : Base{value}
    {
    }

    Result(Base&& value)
      : Base{std::move(value)}
    {
    }
  };

  inline std::unexpected<Error> makeError(Error::Code code,
                                          std::string message = {},
                                          std::source_location location = std::source_location::current())
  {
    return std::unexpected(Error{.code = code, .message = std::move(message), .location = location});
  }
} // namespace ao
