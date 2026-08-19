// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/UnicodeText.h>

#include <unicode/bytestream.h>
#include <unicode/casemap.h>
#include <unicode/normalizer2.h>
#include <unicode/stringpiece.h>
#include <unicode/ubrk.h>
#include <unicode/utext.h>
#include <unicode/utf8.h>
#include <unicode/utypes.h>

#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <string>
#include <string_view>

namespace ao::utility
{
  namespace
  {
    Error::Code errorCodeForIcu(UErrorCode const status, Error::Code const fallback) noexcept
    {
      if (status == U_MEMORY_ALLOCATION_ERROR)
      {
        return Error::Code::ResourceExhausted;
      }
      return fallback;
    }

    std::unexpected<Error> makeIcuError(std::string_view const operation,
                                        UErrorCode const status,
                                        Error::Code const fallback = Error::Code::Generic)
    {
      return makeError(errorCodeForIcu(status, fallback), std::format("{}: {}", operation, u_errorName(status)));
    }

    Result<icu::Normalizer2 const*> nfcNormalizer()
    {
      auto status = UErrorCode{U_ZERO_ERROR};
      auto const* normalizer = icu::Normalizer2::getNFCInstance(status);
      if (U_FAILURE(status) || normalizer == nullptr)
      {
        return makeIcuError("Could not initialize ICU NFC normalization", status, Error::Code::InitFailed);
      }
      return normalizer;
    }

    Result<std::int32_t> checkedIcuLength(std::string_view const text)
    {
      if (text.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
      {
        return makeError(Error::Code::ValueTooLarge, "UTF-8 text exceeds ICU's 32-bit operation limit");
      }
      return static_cast<std::int32_t>(text.size());
    }

    Result<std::int32_t> validatedUtf8Length(std::string_view const text)
    {
      auto const lengthRes = checkedIcuLength(text);
      if (!lengthRes)
      {
        return std::unexpected{lengthRes.error()};
      }

      auto const length = *lengthRes;
      auto index = std::int32_t{0};
      while (index < length)
      {
        auto const byteOffset = index;
        auto scalar = UChar32{};
        U8_NEXT(text.data(), index, length, scalar);
        if (scalar < 0)
        {
          return makeError(Error::Code::InvalidInput, std::format("Invalid UTF-8 sequence at byte {}", byteOffset));
        }
      }
      return length;
    }

    Result<std::string> normalizeValidatedUtf8Nfc(std::string_view const text, std::int32_t const length)
    {
      if (length == 0)
      {
        return std::string{};
      }

      auto const normalizerRes = nfcNormalizer();
      if (!normalizerRes)
      {
        return std::unexpected{normalizerRes.error()};
      }

      auto status = UErrorCode{U_ZERO_ERROR};
      auto const source = icu::StringPiece{text.data(), length};
      auto const isNormalized = (*normalizerRes)->isNormalizedUTF8(source, status);
      if (U_FAILURE(status))
      {
        return makeIcuError("Could not check ICU NFC normalization", status);
      }
      if (isNormalized)
      {
        return std::string{text};
      }

      auto result = std::string{};
      auto sink = icu::StringByteSink<std::string>{&result, length};
      (*normalizerRes)->normalizeUTF8(0, source, sink, nullptr, status);
      if (U_FAILURE(status))
      {
        return makeIcuError("Could not normalize UTF-8 text to NFC", status);
      }
      // ICU emits well-formed UTF-8 from validated UTF-8 input. Retain only
      // the facade's 32-bit operation limit instead of rescanning its output.
      auto const resultLengthRes = checkedIcuLength(result);
      if (!resultLengthRes)
      {
        return std::unexpected{resultLengthRes.error()};
      }
      return result;
    }

    class GraphemeBreakIterator final
    {
    public:
      GraphemeBreakIterator() { _iteratorPtr = ubrk_open(UBRK_CHARACTER, "root", nullptr, 0, &_status); }

      GraphemeBreakIterator(GraphemeBreakIterator const&) = delete;
      GraphemeBreakIterator(GraphemeBreakIterator&&) = delete;
      GraphemeBreakIterator& operator=(GraphemeBreakIterator const&) = delete;
      GraphemeBreakIterator& operator=(GraphemeBreakIterator&&) = delete;

      ~GraphemeBreakIterator()
      {
        if (_iteratorPtr != nullptr)
        {
          ubrk_close(_iteratorPtr);
        }
      }

      UBreakIterator* get() const noexcept { return _iteratorPtr; }

      UErrorCode status() const noexcept { return _status; }

    private:
      UErrorCode _status = U_ZERO_ERROR;
      UBreakIterator* _iteratorPtr = nullptr;
    };
  } // namespace

  Result<> validateUtf8(std::string_view const text)
  {
    auto const lengthRes = validatedUtf8Length(text);
    if (!lengthRes)
    {
      return std::unexpected{lengthRes.error()};
    }
    return {};
  }

  Result<bool> isUtf8Nfc(std::string_view const text)
  {
    auto const lengthRes = validatedUtf8Length(text);
    if (!lengthRes)
    {
      return std::unexpected{lengthRes.error()};
    }
    if (*lengthRes == 0)
    {
      return true;
    }

    auto const normalizerRes = nfcNormalizer();
    if (!normalizerRes)
    {
      return std::unexpected{normalizerRes.error()};
    }

    auto status = UErrorCode{U_ZERO_ERROR};
    auto const isNormalized = (*normalizerRes)->isNormalizedUTF8(icu::StringPiece{text.data(), *lengthRes}, status);
    if (U_FAILURE(status))
    {
      return makeIcuError("Could not check ICU NFC normalization", status);
    }
    return isNormalized;
  }

  Result<std::string> normalizeUtf8Nfc(std::string_view const text)
  {
    auto const lengthRes = validatedUtf8Length(text);
    if (!lengthRes)
    {
      return std::unexpected{lengthRes.error()};
    }
    return normalizeValidatedUtf8Nfc(text, *lengthRes);
  }

  Result<std::string> makeUtf8CaselessKey(std::string_view const text)
  {
    auto const lengthRes = validatedUtf8Length(text);
    if (!lengthRes)
    {
      return std::unexpected{lengthRes.error()};
    }
    if (*lengthRes == 0)
    {
      return std::string{};
    }

    auto normalizedRes = normalizeValidatedUtf8Nfc(text, *lengthRes);
    if (!normalizedRes)
    {
      return std::unexpected{normalizedRes.error()};
    }

    auto const normalizedLengthRes = checkedIcuLength(*normalizedRes);
    if (!normalizedLengthRes)
    {
      return std::unexpected{normalizedLengthRes.error()};
    }

    auto folded = std::string{};
    auto sink = icu::StringByteSink<std::string>{&folded, *normalizedLengthRes};
    auto status = UErrorCode{U_ZERO_ERROR};
    icu::CaseMap::utf8Fold(
      U_FOLD_CASE_DEFAULT, icu::StringPiece{normalizedRes->data(), *normalizedLengthRes}, sink, nullptr, status);
    if (U_FAILURE(status))
    {
      return makeIcuError("Could not case-fold UTF-8 text", status);
    }

    // CaseMap emits well-formed UTF-8 from the validated normalized input.
    auto const foldedLengthRes = checkedIcuLength(folded);
    if (!foldedLengthRes)
    {
      return std::unexpected{foldedLengthRes.error()};
    }
    return normalizeValidatedUtf8Nfc(folded, *foldedLengthRes);
  }

  Result<std::size_t> previousUtf8GraphemeBoundary(std::string_view const text)
  {
    auto const lengthRes = validatedUtf8Length(text);
    if (!lengthRes)
    {
      return std::unexpected{lengthRes.error()};
    }
    if (*lengthRes == 0)
    {
      return std::size_t{0};
    }

    auto iterator = GraphemeBreakIterator{};
    if (iterator.get() == nullptr || U_FAILURE(iterator.status()))
    {
      return makeIcuError("Could not initialize ICU grapheme segmentation", iterator.status(), Error::Code::InitFailed);
    }

    UText uText = UTEXT_INITIALIZER;
    auto status = UErrorCode{U_ZERO_ERROR};
    auto* const openedText = utext_openUTF8(&uText, text.data(), *lengthRes, &status);
    if (U_FAILURE(status) || openedText == nullptr)
    {
      utext_close(&uText);
      return makeIcuError("Could not open UTF-8 text for ICU grapheme segmentation", status);
    }

    ubrk_setUText(iterator.get(), &uText, &status);
    // ubrk_setUText shallow-clones the UText provider, so ICU permits closing
    // this wrapper immediately; the backing string_view remains alive below.
    utext_close(&uText);
    if (U_FAILURE(status))
    {
      return makeIcuError("Could not set ICU grapheme segmentation text", status);
    }

    auto const boundary = ubrk_preceding(iterator.get(), *lengthRes);
    if (boundary == UBRK_DONE)
    {
      return std::size_t{0};
    }
    return static_cast<std::size_t>(boundary);
  }
} // namespace ao::utility
