// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreFoundationString.h"

#include "CoreFoundationOwnership.h"

#include <ao/Error.h>

#include <CoreFoundation/CoreFoundation.h>

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

namespace ao::audio::backend::detail
{
  Result<CoreFoundationPtr<::CFStringRef>> coreFoundationString(std::string_view const utf8)
  {
    if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<::CFIndex>::max()))
    {
      return makeError(Error::Code::ValueTooLarge, "UTF-8 text is too large for Core Foundation");
    }

    auto const* bytes = reinterpret_cast<::UInt8 const*>(utf8.data());
    auto const length = static_cast<::CFIndex>(utf8.size());
    auto* value = ::CFStringCreateWithBytes(
      ::kCFAllocatorDefault, bytes, length, ::kCFStringEncodingUTF8, false);
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
    auto const convertedCharacters = ::CFStringGetBytes(value,
                                                        ::CFRangeMake(0, length),
                                                        ::kCFStringEncodingUTF8,
                                                        0,
                                                        false,
                                                        nullptr,
                                                        0,
                                                        &byteCount);
    if (convertedCharacters != length || byteCount < 0)
    {
      return makeError(Error::Code::InvalidInput, "Core Foundation string cannot be represented as UTF-8");
    }

    auto result = std::string(static_cast<std::size_t>(byteCount), '\0');
    if (byteCount == 0)
    {
      return result;
    }

    ::CFIndex bytesWritten = 0;
    auto const writtenCharacters = ::CFStringGetBytes(value,
                                                      ::CFRangeMake(0, length),
                                                      ::kCFStringEncodingUTF8,
                                                      0,
                                                      false,
                                                      reinterpret_cast<::UInt8*>(result.data()),
                                                      byteCount,
                                                      &bytesWritten);
    if (writtenCharacters != length || bytesWritten != byteCount)
    {
      return makeError(Error::Code::InvalidInput, "Core Foundation string cannot be represented as UTF-8");
    }

    return result;
  }
} // namespace ao::audio::backend::detail
