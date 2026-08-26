// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreFoundationString.h"

#include "CoreFoundationOwnership.h"
#include <ao/Error.h>

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFString.h>
#include <MacTypes.h>

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace ao::audio::backend::detail
{
  Result<CoreFoundationPtr<::CFStringRef>> coreFoundationString(std::string_view const utf8)
  {
    if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<::CFIndex>::max()))
    {
      return makeError(Error::Code::ValueTooLarge, "UTF-8 text is too large for Core Foundation");
    }

    auto const bytes = std::vector<::UInt8>{utf8.begin(), utf8.end()};
    auto const length = static_cast<::CFIndex>(utf8.size());
    auto const* value =
      ::CFStringCreateWithBytes(::kCFAllocatorDefault, bytes.data(), length, ::kCFStringEncodingUTF8, 0U);

    if (value == nullptr)
    {
      return makeError(Error::Code::InvalidInput, "Core Foundation rejected invalid UTF-8 text");
    }

    return CoreFoundationPtr<::CFStringRef>{value};
  }

  Result<std::string> utf8String(::CFStringRef const value)
  {
    if (value == nullptr)
    {
      return makeError(Error::Code::InvalidInput, "Cannot convert a null Core Foundation string");
    }

    auto const length = ::CFStringGetLength(value);
    ::CFIndex byteCount = 0;
    auto const convertedCharacters =
      ::CFStringGetBytes(value, ::CFRangeMake(0, length), ::kCFStringEncodingUTF8, 0, 0U, nullptr, 0, &byteCount);

    if (convertedCharacters != length || byteCount < 0)
    {
      return makeError(Error::Code::InvalidInput, "Core Foundation string cannot be represented as UTF-8");
    }

    auto bytes = std::vector<::UInt8>(static_cast<std::size_t>(byteCount));

    if (byteCount == 0)
    {
      return std::string{};
    }

    ::CFIndex bytesWritten = 0;
    auto const writtenCharacters = ::CFStringGetBytes(
      value, ::CFRangeMake(0, length), ::kCFStringEncodingUTF8, 0, 0U, bytes.data(), byteCount, &bytesWritten);

    if (writtenCharacters != length || bytesWritten != byteCount)
    {
      return makeError(Error::Code::InvalidInput, "Core Foundation string cannot be represented as UTF-8");
    }

    return std::string{bytes.begin(), bytes.end()};
  }
} // namespace ao::audio::backend::detail
