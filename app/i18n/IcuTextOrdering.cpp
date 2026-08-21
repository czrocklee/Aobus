// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/i18n/IcuTextOrdering.h>

#include "IcuTextOrderingDetail.h"
#include <ao/Error.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>
#include <ao/utility/String.h>
#include <ao/utility/UnicodeText.h>

#include <unicode/coll.h>
#include <unicode/locid.h>
#include <unicode/stringpiece.h>
#include <unicode/ucol.h>
#include <unicode/uloc.h>
#include <unicode/unistr.h>
#include <unicode/utypes.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::i18n
{
  namespace
  {
    constexpr std::size_t kInitialSortKeyCapacity = 128;
    constexpr unsigned char kAsciiMax = 0x7fU;

    std::unexpected<Error> makeIcuError(std::string_view const operation,
                                        UErrorCode const status,
                                        Error::Code const fallback = Error::Code::InitFailed)
    {
      Error::Code const code = detail::collationErrorCode(status, fallback);
      return makeError(code, std::format("{}: {}", operation, ::u_errorName(status)));
    }

    Result<std::string> strictLocaleId(std::string_view const tag)
    {
      if (tag.empty() || tag.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
          tag.contains('\0'))
      {
        return makeError(Error::Code::InvalidInput, "The collation locale must be one complete BCP 47 tag");
      }

      auto localeId = std::array<char, ULOC_FULLNAME_CAPACITY>{};
      std::int32_t parsedLength = 0;
      UErrorCode status = U_ZERO_ERROR;
      auto const length = ::uloc_forLanguageTag(
        std::string{tag}.c_str(), localeId.data(), static_cast<std::int32_t>(localeId.size()), &parsedLength, &status);

      if (U_FAILURE(status) != 0 || std::cmp_not_equal(parsedLength, tag.size()))
      {
        return makeIcuError("Could not parse the collation locale", status, Error::Code::InvalidInput);
      }

      return std::string{localeId.data(), static_cast<std::size_t>(length)};
    }

    Result<> setAttribute(icu::Collator& collator,
                          UColAttribute const attribute,
                          UColAttributeValue const value,
                          std::string_view const name)
    {
      UErrorCode status = U_ZERO_ERROR;
      collator.setAttribute(attribute, value, status);

      if (U_FAILURE(status) != 0)
      {
        return makeIcuError(std::format("Could not configure ICU collation attribute {}", name), status);
      }

      return {};
    }

    Result<icu::UnicodeString> makeCollationInput(std::string_view const text)
    {
      bool isAscii = true;

      for (auto const ch : text)
      {
        if (static_cast<unsigned char>(ch) > kAsciiMax)
        {
          isAscii = false;
          break;
        }
      }

      if (isAscii)
      {
        if (text.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        {
          return makeError(Error::Code::ValueTooLarge, "Collation text exceeds ICU's 32-bit operation limit");
        }

        std::int32_t const length = static_cast<std::int32_t>(text.size());
        auto folded = icu::UnicodeString{};
        auto* const buffer = folded.getBuffer(length);

        if (buffer == nullptr)
        {
          return makeError(Error::Code::ResourceExhausted, "Could not allocate the ICU collation input");
        }

        for (std::int32_t index = 0; index < length; ++index)
        {
          buffer[index] = static_cast<char16_t>(
            static_cast<unsigned char>(utility::toAsciiLower(text[static_cast<std::size_t>(index)])));
        }

        folded.releaseBuffer(length);
        return folded;
      }

      auto foldedRes = utility::makeUtf8CaselessKey(text);

      if (!foldedRes)
      {
        return std::unexpected{foldedRes.error()};
      }

      auto folded =
        icu::UnicodeString::fromUTF8(icu::StringPiece{foldedRes->data(), static_cast<std::int32_t>(foldedRes->size())});

      if (folded.isBogus() != 0)
      {
        return makeError(Error::Code::ResourceExhausted, "Could not allocate the ICU collation input");
      }

      return folded;
    }

    class IcuTextOrderingPolicy final : public rt::TextOrderingPolicy
    {
    public:
      explicit IcuTextOrderingPolicy(std::unique_ptr<icu::Collator> collatorPtr)
        : _collatorPtr{std::move(collatorPtr)}
      {
      }

      Result<> makeSortKeyInto(std::string& output, std::string_view const admittedUtf8Text) const override
      {
        output.clear();

        if (admittedUtf8Text.empty())
        {
          return {};
        }

        auto foldedRes = makeCollationInput(admittedUtf8Text);

        if (!foldedRes)
        {
          return std::unexpected{foldedRes.error()};
        }

        std::int32_t requiredLength = 0;
        bool terminated = false;
        auto const writeKey = [&](std::size_t const requestedCapacity)
        {
          output.resize_and_overwrite(
            requestedCapacity,
            [&](char* const buffer, std::size_t const writableSize)
            {
              // ICU's byte API writes into the binary storage owned by std::string.
              // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
              auto* const byteBuffer = reinterpret_cast<std::uint8_t*>(buffer);
              requiredLength =
                _collatorPtr->getSortKey(*foldedRes, byteBuffer, static_cast<std::int32_t>(writableSize));

              if (requiredLength <= 0 || std::cmp_greater(requiredLength, writableSize))
              {
                return std::size_t{};
              }

              terminated = buffer[static_cast<std::size_t>(requiredLength - 1)] == '\0';
              return terminated ? static_cast<std::size_t>(requiredLength - 1) : std::size_t{};
            });
        };

        auto const maxIcuCapacity = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
        auto const reusableCapacity = output.capacity() <= maxIcuCapacity ? output.capacity() : std::size_t{};
        auto const initialCapacity = std::max(reusableCapacity, kInitialSortKeyCapacity);
        writeKey(initialCapacity);

        if (requiredLength <= 0)
        {
          output.clear();
          return makeError(Error::Code::InitFailed, "ICU returned an invalid collation-key length");
        }

        if (std::cmp_less_equal(requiredLength, initialCapacity))
        {
          if (!terminated)
          {
            output.clear();
            return makeError(Error::Code::CorruptData, "ICU violated its collation-key terminator contract");
          }

          return {};
        }

        auto const expectedLength = requiredLength;
        terminated = false;
        writeKey(static_cast<std::size_t>(expectedLength));

        if (requiredLength != expectedLength || !terminated)
        {
          output.clear();
          return makeError(Error::Code::CorruptData, "ICU violated its collation-key terminator contract");
        }

        return {};
      }

    private:
      std::unique_ptr<icu::Collator> _collatorPtr;
    };
  } // namespace

  namespace detail
  {
    Error::Code collationErrorCode(UErrorCode const status, Error::Code const successfulStatusFallback) noexcept
    {
      if (U_SUCCESS(status) != 0)
      {
        return successfulStatusFallback;
      }

      switch (status)
      {
        case U_MEMORY_ALLOCATION_ERROR: return Error::Code::ResourceExhausted;
        case U_BUFFER_OVERFLOW_ERROR: return Error::Code::ValueTooLarge;
        case U_ILLEGAL_ARGUMENT_ERROR: return Error::Code::InvalidInput;
        case U_MISSING_RESOURCE_ERROR:
        case U_FILE_ACCESS_ERROR: return Error::Code::NotFound;
        case U_INVALID_FORMAT_ERROR: return Error::Code::CorruptData;
        default: return Error::Code::InitFailed;
      }
    }
  } // namespace detail

  Result<std::unique_ptr<rt::TextOrderingPolicy>> createIcuTextOrderingPolicy(std::string_view const localeTag)
  {
    auto localeIdRes = strictLocaleId(localeTag);

    if (!localeIdRes)
    {
      return std::unexpected{localeIdRes.error()};
    }

    auto const locale = icu::Locale{localeIdRes->c_str()};
    UErrorCode status = U_ZERO_ERROR;
    auto collatorPtr = std::unique_ptr<icu::Collator>{icu::Collator::createInstance(locale, status)};

    if (U_FAILURE(status) != 0 || collatorPtr == nullptr)
    {
      return makeIcuError("Could not construct the ICU collator", status);
    }

    for (auto const& result : {
           setAttribute(*collatorPtr, UCOL_STRENGTH, UCOL_SECONDARY, "strength"),
           setAttribute(*collatorPtr, UCOL_ALTERNATE_HANDLING, UCOL_NON_IGNORABLE, "alternate-handling"),
           setAttribute(*collatorPtr, UCOL_CASE_LEVEL, UCOL_OFF, "case-level"),
           setAttribute(*collatorPtr, UCOL_NUMERIC_COLLATION, UCOL_OFF, "numeric-collation"),
         })
    {
      if (!result)
      {
        return std::unexpected{result.error()};
      }
    }

    return std::unique_ptr<rt::TextOrderingPolicy>{
      std::make_unique<IcuTextOrderingPolicy>(std::move(collatorPtr)),
    };
  }
} // namespace ao::i18n
