// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/UnicodeText.h>

#include <ao/Error.h>

#include <unicode/bytestream.h>
#include <unicode/casemap.h>
#include <unicode/normalizer2.h>
#include <unicode/stringoptions.h>
#include <unicode/stringpiece.h>
#include <unicode/ubrk.h>
#include <unicode/umachine.h>
#include <unicode/utext.h>
#include <unicode/utf8.h>
#include <unicode/utypes.h>

#include <cstddef>
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
      return makeError(errorCodeForIcu(status, fallback), std::format("{}: {}", operation, ::u_errorName(status)));
    }

    Result<icu::Normalizer2 const*> nfcNormalizer()
    {
      UErrorCode status = U_ZERO_ERROR;
      auto const* normalizer = icu::Normalizer2::getNFCInstance(status);

      if (U_FAILURE(status) != 0 || normalizer == nullptr)
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
      std::int32_t index = 0;

      while (index < length)
      {
        auto const byteOffset = index;
        UChar32 scalar = 0;
        // ICU's iterator macro owns byte-index advancement over contiguous input.
        // NOLINTNEXTLINE(bugprone-inc-dec-in-conditions,readability-simplify-subscript-expr)
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

      UErrorCode status = U_ZERO_ERROR;
      auto const source = icu::StringPiece{text.data(), static_cast<std::int32_t>(text.size())};
      auto const isNormalized = (*normalizerRes)->isNormalizedUTF8(source, status);

      if (U_FAILURE(status) != 0)
      {
        return makeIcuError("Could not check ICU NFC normalization", status);
      }

      if (isNormalized != 0)
      {
        return std::string{text};
      }

      auto result = std::string{};
      auto sink = icu::StringByteSink<std::string>{&result, length};
      (*normalizerRes)->normalizeUTF8(0, source, sink, nullptr, status);

      if (U_FAILURE(status) != 0)
      {
        return makeIcuError("Could not normalize UTF-8 text to NFC", status);
      }

      // ICU emits well-formed UTF-8 from validated UTF-8 input. Retain only
      // the facade's 32-bit operation limit instead of rescanning its output.
      if (auto const resultLengthRes = checkedIcuLength(result); !resultLengthRes)
      {
        return std::unexpected{resultLengthRes.error()};
      }

      return result;
    }

    class GraphemeBreakIterator final
    {
    public:
      GraphemeBreakIterator()
        : _iterator{::ubrk_open(UBRK_CHARACTER, "root", nullptr, 0, &_status)}
      {
      }

      GraphemeBreakIterator(GraphemeBreakIterator const&) = delete;
      GraphemeBreakIterator(GraphemeBreakIterator&&) = delete;
      GraphemeBreakIterator& operator=(GraphemeBreakIterator const&) = delete;
      GraphemeBreakIterator& operator=(GraphemeBreakIterator&&) = delete;

      ~GraphemeBreakIterator()
      {
        if (_iterator != nullptr)
        {
          ::ubrk_close(_iterator);
        }
      }

      UBreakIterator* get() const noexcept { return _iterator; }

      Result<> setText(std::string_view const text)
      {
        if (_iterator == nullptr || U_FAILURE(_status) != 0)
        {
          return makeIcuError("Could not initialize ICU grapheme segmentation", _status, Error::Code::InitFailed);
        }

        auto uText = UText UTEXT_INITIALIZER;
        UErrorCode status = U_ZERO_ERROR;
        auto* const openedText = ::utext_openUTF8(&uText, text.data(), static_cast<std::int32_t>(text.size()), &status);

        if (U_FAILURE(status) != 0 || openedText == nullptr)
        {
          ::utext_close(&uText);
          return makeIcuError("Could not open UTF-8 text for ICU grapheme segmentation", status);
        }

        ::ubrk_setUText(_iterator, &uText, &status);
        // ubrk_setUText shallow-clones the UText provider, so ICU permits closing
        // this wrapper immediately; the backing string_view remains alive below.
        ::utext_close(&uText);

        if (U_FAILURE(status) != 0)
        {
          return makeIcuError("Could not set ICU grapheme segmentation text", status);
        }

        return {};
      }

    private:
      UErrorCode _status = U_ZERO_ERROR;
      UBreakIterator* _iterator = nullptr;
    };

    enum class GraphemeDirection : std::uint8_t
    {
      Previous,
      Next,
    };

    // A UTF-8 continuation byte is the only kind that can sit inside a scalar.
    constexpr unsigned char kUtf8ContinuationMask = 0xC0U;
    constexpr unsigned char kUtf8ContinuationTag = 0x80U;

    // Rounds a byte offset back to the start of the scalar it points into. The
    // text is validated before this runs, so a continuation byte always has a
    // lead byte behind it and the walk cannot reach past the front.
    std::size_t utf8ScalarStart(std::string_view const text, std::size_t const offset) noexcept
    {
      if (offset >= text.size())
      {
        return offset;
      }

      auto start = offset;

      while (start > 0 && (static_cast<unsigned char>(text[start]) & kUtf8ContinuationMask) == kUtf8ContinuationTag)
      {
        --start;
      }

      return start;
    }

    Result<std::size_t> utf8GraphemeBoundary(std::string_view const text,
                                             std::size_t const offset,
                                             GraphemeDirection const direction)
    {
      auto const lengthRes = validatedUtf8Length(text);

      if (!lengthRes)
      {
        return std::unexpected{lengthRes.error()};
      }

      auto const length = static_cast<std::size_t>(*lengthRes);

      if (offset > length)
      {
        return makeError(Error::Code::InvalidInput,
                         std::format("Grapheme offset {} is past the end of {} UTF-8 bytes", offset, length));
      }

      // Both ends are answered without ICU: a search away from the text has no
      // boundary to find, and the terminating offset is itself one.
      if (direction == GraphemeDirection::Previous ? offset == 0 : offset == length)
      {
        return offset;
      }

      auto iterator = GraphemeBreakIterator{};

      if (auto const textRes = iterator.setText(text); !textRes)
      {
        return std::unexpected{textRes.error()};
      }

      // ICU addresses the text by code point, so an offset inside a scalar is
      // rounded down to that scalar's start before either search runs. Rounding
      // it here makes that explicit and, for a backward search, keeps the
      // rounded offset eligible: ICU would look strictly before it and step out
      // of the very cluster the offset sits in.
      auto const scalarStart = utf8ScalarStart(text, offset);
      auto const searchOffset = static_cast<std::int32_t>(scalarStart);

      if (direction == GraphemeDirection::Previous && scalarStart != offset &&
          ::ubrk_isBoundary(iterator.get(), searchOffset) != 0)
      {
        return static_cast<std::size_t>(searchOffset);
      }

      auto const boundary = direction == GraphemeDirection::Previous ? ::ubrk_preceding(iterator.get(), searchOffset)
                                                                     : ::ubrk_following(iterator.get(), searchOffset);

      // UBRK_DONE means the search ran off the text, which only the guarded
      // ends above can reach; treat it as staying at the corresponding edge.
      if (boundary == UBRK_DONE)
      {
        return direction == GraphemeDirection::Previous ? std::size_t{0} : length;
      }

      return static_cast<std::size_t>(boundary);
    }
  } // namespace

  Result<> validateUtf8(std::string_view const text)
  {
    if (auto const lengthRes = validatedUtf8Length(text); !lengthRes)
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

    UErrorCode status = U_ZERO_ERROR;
    auto const source = icu::StringPiece{text.data(), static_cast<std::int32_t>(text.size())};
    auto const isNormalized = (*normalizerRes)->isNormalizedUTF8(source, status);

    if (U_FAILURE(status) != 0)
    {
      return makeIcuError("Could not check ICU NFC normalization", status);
    }

    return isNormalized != 0;
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
    UErrorCode status = U_ZERO_ERROR;
    icu::CaseMap::utf8Fold(
      U_FOLD_CASE_DEFAULT, icu::StringPiece{normalizedRes->data(), *normalizedLengthRes}, sink, nullptr, status);

    if (U_FAILURE(status) != 0)
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

  Result<bool> isUtf8GraphemeBoundary(std::string_view const text, std::size_t const offset)
  {
    if (auto const lengthRes = validatedUtf8Length(text); !lengthRes)
    {
      return std::unexpected{lengthRes.error()};
    }

    if (offset > text.size())
    {
      return makeError(Error::Code::InvalidInput,
                       std::format("Grapheme offset {} is past the end of {} UTF-8 bytes", offset, text.size()));
    }

    if (offset == 0 || offset == text.size())
    {
      return true;
    }

    if (utf8ScalarStart(text, offset) != offset)
    {
      return false;
    }

    auto iterator = GraphemeBreakIterator{};

    if (auto const textRes = iterator.setText(text); !textRes)
    {
      return std::unexpected{textRes.error()};
    }

    return ::ubrk_isBoundary(iterator.get(), static_cast<std::int32_t>(offset)) != 0;
  }

  Result<std::size_t> previousUtf8GraphemeBoundary(std::string_view const text, std::size_t const offset)
  {
    return utf8GraphemeBoundary(text, offset, GraphemeDirection::Previous);
  }

  Result<std::size_t> nextUtf8GraphemeBoundary(std::string_view const text, std::size_t const offset)
  {
    return utf8GraphemeBoundary(text, offset, GraphemeDirection::Next);
  }
} // namespace ao::utility
