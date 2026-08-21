// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/i18n/IcuCompletionAliases.h>

#include <ao/Error.h>
#include <ao/rt/completion/CompletionAliasPolicy.h>
#include <ao/utility/String.h>
#include <ao/utility/UnicodeText.h>

#include <unicode/stringpiece.h>
#include <unicode/translit.h>
#include <unicode/uchar.h>
#include <unicode/umachine.h>
#include <unicode/unistr.h>
#include <unicode/uscript.h>
#include <unicode/utf8.h>
#include <unicode/utrans.h>
#include <unicode/utypes.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::i18n
{
  namespace
  {
    constexpr auto kKanaTransformId = std::string_view{"Any-Latin; Latin-ASCII; Lower"};
    constexpr auto kHanTransformId = std::string_view{"Han-Latin; Latin-ASCII; Lower"};
    constexpr unsigned char kAsciiMax = 0x7fU;

    struct TextShape final
    {
      std::vector<std::string_view> kanaRuns;
      bool hasHan = false;
      bool onlyKanaSeparators = true;
    };

    Error::Code transliterationErrorCode(UErrorCode const status) noexcept
    {
      switch (status)
      {
        case U_MEMORY_ALLOCATION_ERROR: return Error::Code::ResourceExhausted;
        case U_BUFFER_OVERFLOW_ERROR: return Error::Code::ValueTooLarge;
        case U_MISSING_RESOURCE_ERROR:
        case U_FILE_ACCESS_ERROR: return Error::Code::NotFound;
        case U_INVALID_FORMAT_ERROR: return Error::Code::CorruptData;
        default: return Error::Code::InitFailed;
      }
    }

    std::unexpected<Error> makeIcuError(std::string_view const operation, UErrorCode const status)
    {
      return makeError(transliterationErrorCode(status), std::format("{}: {}", operation, ::u_errorName(status)));
    }

    bool isKana(UChar32 const scalar) noexcept
    {
      return ::uscript_hasScript(scalar, USCRIPT_HIRAGANA) != 0 || ::uscript_hasScript(scalar, USCRIPT_KATAKANA) != 0;
    }

    bool isCombiningMark(UChar32 const scalar) noexcept
    {
      auto const category = static_cast<UCharCategory>(::u_charType(scalar));
      return category == U_NON_SPACING_MARK || category == U_COMBINING_SPACING_MARK || category == U_ENCLOSING_MARK;
    }

    bool isSeparator(UChar32 const scalar) noexcept
    {
      if (::u_isUWhiteSpace(scalar) != 0)
      {
        return true;
      }

      auto const category = static_cast<UCharCategory>(::u_charType(scalar));
      return category == U_DASH_PUNCTUATION || category == U_START_PUNCTUATION || category == U_END_PUNCTUATION ||
             category == U_CONNECTOR_PUNCTUATION || category == U_OTHER_PUNCTUATION ||
             category == U_INITIAL_PUNCTUATION || category == U_FINAL_PUNCTUATION;
    }

    TextShape analyzeText(std::string_view const text)
    {
      auto shape = TextShape{};
      std::int32_t index = 0;
      auto const length = static_cast<std::int32_t>(text.size());
      std::int32_t runBegin = -1;
      std::int32_t runEnd = -1;

      auto finishRun = [&]
      {
        if (runBegin >= 0)
        {
          shape.kanaRuns.push_back(
            text.substr(static_cast<std::size_t>(runBegin), static_cast<std::size_t>(runEnd - runBegin)));
          runBegin = -1;
          runEnd = -1;
        }
      };

      while (index < length)
      {
        auto const scalarBegin = index;
        UChar32 scalar = 0;
        // ICU's iterator macro owns byte-index advancement over contiguous input.
        // NOLINTNEXTLINE(bugprone-inc-dec-in-conditions,readability-simplify-subscript-expr)
        U8_NEXT(text.data(), index, length, scalar);

        if ((!isSeparator(scalar) && isKana(scalar)) || (runBegin >= 0 && isCombiningMark(scalar)))
        {
          if (runBegin < 0)
          {
            runBegin = scalarBegin;
          }

          runEnd = index;
          continue;
        }

        finishRun();
        shape.hasHan = shape.hasHan || ::uscript_hasScript(scalar, USCRIPT_HAN) != 0;
        shape.onlyKanaSeparators = shape.onlyKanaSeparators && isSeparator(scalar);
      }

      finishRun();
      return shape;
    }

    std::string compactAlias(std::string_view const text)
    {
      auto result = std::string{};
      result.reserve(text.size());

      for (auto const ch : text)
      {
        if (utility::isAsciiAlpha(ch))
        {
          result.push_back(utility::toAsciiLower(ch));
        }
        else if (utility::isAsciiDigit(ch))
        {
          result.push_back(ch);
        }
      }

      return result;
    }

    void appendUniqueAlias(std::vector<std::string>& output, std::string alias)
    {
      if (alias.size() < rt::kMinimumCompletionAliasLength || std::ranges::contains(output, alias))
      {
        return;
      }

      output.push_back(std::move(alias));
    }

    void removeCompactDirectSpelling(std::vector<std::string>& output, std::string_view const source)
    {
      if (auto const direct = compactAlias(source); !direct.empty())
      {
        std::erase(output, direct);
      }
    }

    Result<std::unique_ptr<icu::Transliterator>> createTransliterator(std::string_view const identifier)
    {
      UErrorCode status = U_ZERO_ERROR;
      auto const id =
        icu::UnicodeString::fromUTF8(icu::StringPiece{identifier.data(), static_cast<std::int32_t>(identifier.size())});
      auto transliteratorPtr = std::unique_ptr<icu::Transliterator>{
        icu::Transliterator::createInstance(id, UTRANS_FORWARD, status),
      };

      if (U_FAILURE(status) != 0 || transliteratorPtr == nullptr)
      {
        return makeIcuError(std::format("Could not construct ICU transform '{}'", identifier), status);
      }

      return transliteratorPtr;
    }

    Result<std::string> transliterate(icu::Transliterator const& transform, std::string_view const source)
    {
      auto text =
        icu::UnicodeString::fromUTF8(icu::StringPiece{source.data(), static_cast<std::int32_t>(source.size())});

      if (text.isBogus() != 0)
      {
        return makeError(Error::Code::ResourceExhausted, "Could not allocate ICU transliteration input");
      }

      transform.transliterate(text);

      if (text.isBogus() != 0)
      {
        return makeError(Error::Code::ResourceExhausted, "Could not allocate ICU transliteration output");
      }

      auto result = std::string{};
      text.toUTF8String(result);
      return result;
    }

    class IcuCompletionAliasPolicy final : public rt::CompletionAliasPolicy
    {
    public:
      Result<> makeAliasesInto(std::vector<std::string>& output, std::string_view const admittedUtf8Text) const override
      {
        output.clear();

        if (admittedUtf8Text.contains('\0'))
        {
          return makeError(Error::Code::InvalidInput, "Completion alias text contains an embedded NUL");
        }

        if (std::ranges::all_of(
              admittedUtf8Text, [](char const ch) { return static_cast<unsigned char>(ch) <= kAsciiMax; }))
        {
          return {};
        }

        auto const nfcRes = utility::isUtf8Nfc(admittedUtf8Text);

        if (!nfcRes)
        {
          return std::unexpected{nfcRes.error()};
        }

        if (!*nfcRes)
        {
          return makeError(Error::Code::InvalidInput, "Completion alias text must be admitted NFC text");
        }

        auto const shape = analyzeText(admittedUtf8Text);

        if (!shape.kanaRuns.empty())
        {
          auto transformRes = kanaTransform();

          if (!transformRes)
          {
            return std::unexpected{transformRes.error()};
          }

          auto combined = std::string{};

          for (auto const run : shape.kanaRuns)
          {
            auto aliasRes = transliterate(**transformRes, run);

            if (!aliasRes)
            {
              return std::unexpected{aliasRes.error()};
            }

            auto alias = compactAlias(*aliasRes);
            combined += alias;

            if (alias.size() >= rt::kMinimumCompletionAliasLength)
            {
              appendUniqueAlias(output, std::move(alias));
            }
          }

          if (shape.onlyKanaSeparators && shape.kanaRuns.size() > 1)
          {
            appendUniqueAlias(output, std::move(combined));
          }

          removeCompactDirectSpelling(output, admittedUtf8Text);

          return {};
        }

        if (!shape.hasHan)
        {
          return {};
        }

        auto transformRes = hanTransform();

        if (!transformRes)
        {
          return std::unexpected{transformRes.error()};
        }

        auto aliasRes = transliterate(**transformRes, admittedUtf8Text);

        if (!aliasRes)
        {
          return std::unexpected{aliasRes.error()};
        }

        appendUniqueAlias(output, compactAlias(*aliasRes));
        removeCompactDirectSpelling(output, admittedUtf8Text);
        return {};
      }

    private:
      Result<icu::Transliterator*> kanaTransform() const { return transform(_kanaTransformPtr, kKanaTransformId); }

      Result<icu::Transliterator*> hanTransform() const { return transform(_hanTransformPtr, kHanTransformId); }

      static Result<icu::Transliterator*> transform(std::unique_ptr<icu::Transliterator>& transformPtr,
                                                    std::string_view const identifier)
      {
        if (transformPtr == nullptr)
        {
          auto transformRes = createTransliterator(identifier);

          if (!transformRes)
          {
            return std::unexpected{transformRes.error()};
          }

          transformPtr = std::move(*transformRes);
        }

        return transformPtr.get();
      }

      mutable std::unique_ptr<icu::Transliterator> _kanaTransformPtr;
      mutable std::unique_ptr<icu::Transliterator> _hanTransformPtr;
    };
  } // namespace

  std::unique_ptr<rt::CompletionAliasPolicy> createIcuCompletionAliasPolicy()
  {
    return std::make_unique<IcuCompletionAliasPolicy>();
  }
} // namespace ao::i18n
