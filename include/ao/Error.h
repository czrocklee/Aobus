#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace ao
{
  struct Error final
  {
    enum class Code : std::uint8_t
    {
      Generic,
      DeviceNotFound,
      FormatRejected,
      InitFailed,
      IoError,
      DecodeFailed,
      SeekFailed,
      NotSupported,
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
