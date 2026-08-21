// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CatalogPattern.h"

#include "WinUiResourceProjection.h"
#include <ao/Error.h>

#include <unicode/messagepattern.h>
#include <unicode/parseerr.h>
#include <unicode/stringpiece.h>
#include <unicode/umachine.h>
#include <unicode/unistr.h>
#include <unicode/ures.h>
#include <unicode/utf16.h>
#include <unicode/utypes.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::i18n::detail
{
  namespace
  {
    using MessagePatternPtr = std::unique_ptr<icu::MessagePattern>;
    using ResourceBundlePtr = std::unique_ptr<UResourceBundle, decltype(&::ures_close)>;

    constexpr auto kLatinCapitalLetterCWithCedilla = UChar32{0x00C7};
    constexpr auto kLatinSmallLetterCWithCedilla = UChar32{0x00E7};
    constexpr auto kLatinCapitalLetterNWithTilde = UChar32{0x00D1};
    constexpr auto kLatinSmallLetterNWithTilde = UChar32{0x00F1};

    std::unexpected<Error> makeIcuError(std::string_view const operation,
                                        UErrorCode const status,
                                        Error::Code const fallback = Error::Code::FormatRejected)
    {
      auto const code = status == U_MEMORY_ALLOCATION_ERROR ? Error::Code::ResourceExhausted : fallback;
      return makeError(code, std::format("{}: {}", operation, ::u_errorName(status)));
    }

    icu::UnicodeString fromUtf8(std::string_view const text)
    {
      return icu::UnicodeString::fromUTF8(icu::StringPiece{text.data(), static_cast<std::int32_t>(text.size())});
    }

    std::string toUtf8(icu::UnicodeString const& text)
    {
      auto result = std::string{};
      text.toUTF8String(result);
      return result;
    }

    Result<MessagePatternPtr> parsePattern(std::string_view const pattern)
    {
      if (pattern.size() > static_cast<std::size_t>(INT32_MAX))
      {
        return makeError(Error::Code::ValueTooLarge, "Message pattern exceeds ICU's 32-bit limit");
      }

      UErrorCode status = U_ZERO_ERROR;
      auto parseError = UParseError{};
      auto parsedPtr = std::make_unique<icu::MessagePattern>(fromUtf8(pattern), &parseError, status);

      if (U_FAILURE(status) != 0)
      {
        return makeError(
          Error::Code::FormatRejected,
          std::format(
            "Invalid MessageFormat pattern at UTF-16 offset {}: {}", parseError.offset, ::u_errorName(status)));
      }

      return parsedPtr;
    }

    MessageArgumentKind argumentKind(UMessagePatternArgType const type)
    {
      switch (type)
      {
        case UMSGPAT_ARG_TYPE_NONE: return MessageArgumentKind::Value;
        case UMSGPAT_ARG_TYPE_SIMPLE: return MessageArgumentKind::Simple;
        case UMSGPAT_ARG_TYPE_CHOICE: return MessageArgumentKind::Choice;
        case UMSGPAT_ARG_TYPE_PLURAL: return MessageArgumentKind::Plural;
        case UMSGPAT_ARG_TYPE_SELECT: return MessageArgumentKind::Select;
        case UMSGPAT_ARG_TYPE_SELECTORDINAL: return MessageArgumentKind::SelectOrdinal;
      }

      return MessageArgumentKind::Value;
    }

    bool requiresOtherSelector(UMessagePatternArgType const type) noexcept
    {
      return type == UMSGPAT_ARG_TYPE_PLURAL || type == UMSGPAT_ARG_TYPE_SELECT ||
             type == UMSGPAT_ARG_TYPE_SELECTORDINAL;
    }

    bool hasOtherSelector(icu::MessagePattern const& pattern, std::int32_t const startIndex)
    {
      auto const limitIndex = pattern.getLimitPartIndex(startIndex);

      for (std::int32_t index = startIndex + 1; index < limitIndex; ++index)
      {
        auto const type = pattern.getPartType(index);

        if (type == UMSGPAT_PART_TYPE_ARG_START)
        {
          index = pattern.getLimitPartIndex(index);
          continue;
        }

        if (type == UMSGPAT_PART_TYPE_ARG_SELECTOR &&
            pattern.partSubstringMatches(pattern.getPart(index), UNICODE_STRING_SIMPLE("other")) != 0)
        {
          return true;
        }
      }

      return false;
    }

    void appendPseudoLiteral(icu::UnicodeString const& source,
                             std::int32_t const begin,
                             std::int32_t const end,
                             icu::UnicodeString& output)
    {
      for (std::int32_t index = begin; index < end;)
      {
        auto const scalar = source.char32At(index);
        index += U16_LENGTH(scalar);

        switch (scalar)
        {
          case 'A': output.append(u"ÀÀ"); break;
          case 'a': output.append(u"àà"); break;
          case 'E': output.append(u"ËË"); break;
          case 'e': output.append(u"ëë"); break;
          case 'I': output.append(u"ÏÏ"); break;
          case 'i': output.append(u"ïï"); break;
          case 'O': output.append(u"ÖÖ"); break;
          case 'o': output.append(u"öö"); break;
          case 'U': output.append(u"ÜÜ"); break;
          case 'u': output.append(u"üü"); break;
          case 'C': output.append(kLatinCapitalLetterCWithCedilla); break;
          case 'c': output.append(kLatinSmallLetterCWithCedilla); break;
          case 'N': output.append(kLatinCapitalLetterNWithTilde); break;
          case 'n': output.append(kLatinSmallLetterNWithTilde); break;
          default: output.append(scalar); break;
        }
      }
    }

    void appendPseudoMessage(icu::MessagePattern const& parsed,
                             std::int32_t const startIndex,
                             icu::UnicodeString& output)
    {
      auto const& source = parsed.getPatternString();
      auto const limitIndex = parsed.getLimitPartIndex(startIndex);
      auto cursor = parsed.getPart(startIndex).getLimit();

      for (std::int32_t index = startIndex + 1; index < limitIndex; ++index)
      {
        auto const& part = parsed.getPart(index);
        auto const type = part.getType();

        if (type == UMSGPAT_PART_TYPE_ARG_START)
        {
          appendPseudoLiteral(source, cursor, part.getIndex(), output);
          auto const argumentLimitIndex = parsed.getLimitPartIndex(index);
          auto argumentCursor = part.getIndex();

          for (std::int32_t nestedIndex = index + 1; nestedIndex < argumentLimitIndex; ++nestedIndex)
          {
            auto const& nestedPart = parsed.getPart(nestedIndex);

            if (nestedPart.getType() != UMSGPAT_PART_TYPE_MSG_START)
            {
              continue;
            }

            output.append(source, argumentCursor, nestedPart.getLimit() - argumentCursor);
            appendPseudoMessage(parsed, nestedIndex, output);
            auto const nestedLimitIndex = parsed.getLimitPartIndex(nestedIndex);
            // appendPseudoMessage writes a message body, not its closing syntax.
            // Leave the cursor on MSG_LIMIT so the enclosing argument copies
            // that delimiter and the following selector verbatim.
            argumentCursor = parsed.getPart(nestedLimitIndex).getIndex();
            nestedIndex = nestedLimitIndex;
          }

          auto const& argumentLimit = parsed.getPart(argumentLimitIndex);
          output.append(source, argumentCursor, argumentLimit.getLimit() - argumentCursor);
          cursor = argumentLimit.getLimit();
          index = argumentLimitIndex;
          continue;
        }

        if (type == UMSGPAT_PART_TYPE_SKIP_SYNTAX || type == UMSGPAT_PART_TYPE_REPLACE_NUMBER)
        {
          appendPseudoLiteral(source, cursor, part.getIndex(), output);
          output.append(source, part.getIndex(), part.getLength());
          cursor = part.getLimit();
        }
      }

      appendPseudoLiteral(source, cursor, parsed.getPart(limitIndex).getIndex(), output);
    }

    std::string escapeResourceString(std::string_view const text)
    {
      auto escaped = std::string{};
      escaped.reserve(text.size());

      for (auto const character : text)
      {
        switch (character)
        {
          case '\\': escaped += "\\\\"; break;
          case '"': escaped += "\\\""; break;
          case '\n': escaped += "\\n"; break;
          case '\r': escaped += "\\r"; break;
          case '\t': escaped += "\\t"; break;
          default: escaped.push_back(character); break;
        }
      }

      return escaped;
    }

    std::string escapeXml(std::string_view const text)
    {
      auto escaped = std::string{};
      escaped.reserve(text.size());

      for (auto const character : text)
      {
        switch (character)
        {
          case '&': escaped += "&amp;"; break;
          case '<': escaped += "&lt;"; break;
          case '>': escaped += "&gt;"; break;
          case '"': escaped += "&quot;"; break;
          case '\'': escaped += "&apos;"; break;
          default: escaped.push_back(character); break;
        }
      }

      return escaped;
    }

    std::vector<CatalogMessage> sortedMessages(std::span<CatalogMessage const> const messages)
    {
      auto sorted = std::vector<CatalogMessage>{messages.begin(), messages.end()};
      std::ranges::sort(sorted, {}, &CatalogMessage::id);
      return sorted;
    }
  } // namespace

  Result<std::vector<MessageArgumentSignature>> messageArgumentSignature(std::string_view const pattern)
  {
    auto parsedRes = parsePattern(pattern);

    if (!parsedRes)
    {
      return std::unexpected{parsedRes.error()};
    }

    auto signature = std::vector<MessageArgumentSignature>{};
    auto const& parsed = **parsedRes;

    for (std::int32_t index = 0; index < parsed.countParts(); ++index)
    {
      auto const& part = parsed.getPart(index);

      if (part.getType() != UMSGPAT_PART_TYPE_ARG_START)
      {
        continue;
      }

      if (index + 1 >= parsed.countParts() || parsed.getPartType(index + 1) != UMSGPAT_PART_TYPE_ARG_NAME)
      {
        return makeError(Error::Code::FormatRejected, "Message patterns must use named arguments");
      }

      if (requiresOtherSelector(part.getArgType()) && !hasOtherSelector(parsed, index))
      {
        return makeError(Error::Code::FormatRejected,
                         std::format("Message argument '{}' requires an 'other' branch",
                                     toUtf8(parsed.getSubstring(parsed.getPart(index + 1)))));
      }

      auto candidate = MessageArgumentSignature{
        .name = toUtf8(parsed.getSubstring(parsed.getPart(index + 1))),
        .kind = argumentKind(part.getArgType()),
      };
      auto const existing = std::ranges::find(signature, candidate.name, &MessageArgumentSignature::name);

      if (existing == signature.end())
      {
        signature.push_back(std::move(candidate));
      }
      else if (existing->kind != candidate.kind)
      {
        return makeError(Error::Code::FormatRejected,
                         std::format("Message argument '{}' is used with inconsistent kinds", candidate.name));
      }
    }

    std::ranges::sort(signature, {}, &MessageArgumentSignature::name);
    return signature;
  }

  std::string unescapeIcuApostrophePairs(std::string_view const pattern)
  {
    auto result = std::string{};
    result.reserve(pattern.size());

    for (std::size_t index = 0; index < pattern.size(); ++index)
    {
      if (pattern[index] == '\'' && index + 1 < pattern.size() && pattern[index + 1] == '\'')
      {
        result.push_back('\'');
        ++index;
        continue;
      }

      result.push_back(pattern[index]);
    }

    return result;
  }

  Result<std::string> projectWinUiPositionalPattern(std::string_view const pattern, std::string_view const argumentName)
  {
    auto signatureRes = messageArgumentSignature(pattern);

    if (!signatureRes)
    {
      return std::unexpected{signatureRes.error()};
    }

    if (signatureRes->size() != 1 || signatureRes->front().name != argumentName ||
        signatureRes->front().kind != MessageArgumentKind::Value)
    {
      return makeError(
        Error::Code::FormatRejected, "A WinUI positional pattern must contain exactly one plain named argument");
    }

    auto const namedToken = "{" + std::string{argumentName} + "}";
    auto result = std::string{};
    result.reserve(pattern.size());
    bool replaced = false;

    for (std::size_t index = 0; index < pattern.size();)
    {
      if (pattern[index] == '\'')
      {
        if (index + 1 < pattern.size() && pattern[index + 1] == '\'')
        {
          result.push_back('\'');
          index += 2;
          continue;
        }

        if (index + 1 < pattern.size() &&
            (pattern[index + 1] == '{' || pattern[index + 1] == '}' || pattern[index + 1] == '#'))
        {
          return makeError(
            Error::Code::FormatRejected, "WinUI positional resources do not support ICU apostrophe-quoted syntax");
        }
      }

      if (pattern.substr(index).starts_with(namedToken))
      {
        result += "{0}";
        index += namedToken.size();
        replaced = true;
        continue;
      }

      result.push_back(pattern[index]);
      ++index;
    }

    if (!replaced)
    {
      return makeError(Error::Code::FormatRejected, "A WinUI positional resource contains no replaceable argument");
    }

    for (std::size_t index = 0; index < result.size(); ++index)
    {
      if (result[index] == '{' && result.substr(index).starts_with("{0}"))
      {
        index += 2;
        continue;
      }

      if (result[index] == '{' || result[index] == '}')
      {
        return makeError(
          Error::Code::FormatRejected, "A WinUI positional resource contains unsupported MessageFormat syntax");
      }
    }

    return result;
  }

  Result<std::vector<CatalogMessage>> projectWinUiResources(std::span<CatalogMessage const> const messages,
                                                            MissingWinUiMessagePolicy const missingMessagePolicy)
  {
    auto result = std::vector<CatalogMessage>{messages.begin(), messages.end()};
    result.reserve(result.size() + kWinUiResourceAliases.size());

    for (auto const& positional : kWinUiPositionalResources)
    {
      auto source = std::ranges::find(result, positional.messageId, &CatalogMessage::id);

      if (source == result.end())
      {
        if (missingMessagePolicy == MissingWinUiMessagePolicy::Omit)
        {
          continue;
        }

        return makeError(
          Error::Code::FormatRejected,
          "WinUI positional resource references unknown message id '" + std::string{positional.messageId} + "'");
      }

      auto projectedRes = projectWinUiPositionalPattern(source->pattern, positional.argumentName);

      if (!projectedRes)
      {
        return makeError(projectedRes.error().code,
                         "WinUI resource cannot positionalize message '" + std::string{positional.messageId} +
                           "': " + projectedRes.error().message);
      }

      source->pattern = std::move(*projectedRes);
    }

    for (auto& message : result)
    {
      message.pattern = unescapeIcuApostrophePairs(message.pattern);
    }

    for (auto const& alias : kWinUiResourceAliases)
    {
      auto const source = std::ranges::find(result, alias.messageId, &CatalogMessage::id);

      if (source == result.end())
      {
        if (missingMessagePolicy == MissingWinUiMessagePolicy::Omit)
        {
          continue;
        }

        return makeError(Error::Code::FormatRejected,
                         "WinUI resource alias references unknown message id '" + std::string{alias.messageId} + "'");
      }

      auto signatureRes = messageArgumentSignature(source->pattern);

      if (!signatureRes)
      {
        return std::unexpected{signatureRes.error()};
      }

      if (!signatureRes->empty())
      {
        return makeError(
          Error::Code::FormatRejected,
          "WinUI resource alias has an incompatible message signature for '" + std::string{alias.messageId} + "'");
      }

      if (std::ranges::contains(result, alias.resourceId, &CatalogMessage::id))
      {
        return makeError(Error::Code::FormatRejected,
                         "WinUI resource alias collides with message id '" + std::string{alias.resourceId} + "'");
      }

      result.push_back({.id = std::string{alias.resourceId}, .pattern = source->pattern});
    }

    return result;
  }

  Result<> validateTranslationCatalog(std::span<CatalogMessage const> const root,
                                      std::span<CatalogMessage const> const translation)
  {
    for (auto const& message : root)
    {
      auto const signatureRes = messageArgumentSignature(message.pattern);

      if (!signatureRes)
      {
        return makeError(
          signatureRes.error().code, std::format("Root message '{}': {}", message.id, signatureRes.error().message));
      }
    }

    for (auto const& translated : translation)
    {
      auto const rootEntry = std::ranges::find(root, translated.id, &CatalogMessage::id);

      if (rootEntry == root.end())
      {
        return makeError(
          Error::Code::FormatRejected, std::format("Translation defines unknown message id '{}'", translated.id));
      }

      auto const rootSignatureRes = messageArgumentSignature(rootEntry->pattern);

      if (!rootSignatureRes)
      {
        return std::unexpected{rootSignatureRes.error()};
      }

      auto const translatedSignatureRes = messageArgumentSignature(translated.pattern);

      if (!translatedSignatureRes)
      {
        return makeError(
          translatedSignatureRes.error().code,
          std::format("Translation message '{}': {}", translated.id, translatedSignatureRes.error().message));
      }

      if (*rootSignatureRes != *translatedSignatureRes)
      {
        return makeError(Error::Code::FormatRejected,
                         std::format("Translation message '{}' has a different argument signature", translated.id));
      }
    }

    return {};
  }

  Result<std::string> pseudoLocalizePattern(std::string_view const pattern)
  {
    auto parsedRes = parsePattern(pattern);

    if (!parsedRes)
    {
      return std::unexpected{parsedRes.error()};
    }

    auto output = icu::UnicodeString{u"[!! "};
    appendPseudoMessage(**parsedRes, 0, output);
    output.append(u" !!]");
    return toUtf8(output);
  }

  Result<std::vector<CatalogMessage>> loadCompiledCatalog(std::string_view const resourceDirectory,
                                                          std::string_view const locale)
  {
    UErrorCode status = U_ZERO_ERROR;
    auto bundlePtr = ResourceBundlePtr{
      ::ures_openDirect(std::string{resourceDirectory}.c_str(), std::string{locale}.c_str(), &status), &::ures_close};

    if (U_FAILURE(status) != 0 || bundlePtr == nullptr)
    {
      return makeIcuError("Could not open compiled catalog", status, Error::Code::IoError);
    }

    status = U_ZERO_ERROR;
    auto messagesPtr = ResourceBundlePtr{::ures_getByKey(bundlePtr.get(), "messages", nullptr, &status), &::ures_close};

    if (U_FAILURE(status) != 0 || messagesPtr == nullptr)
    {
      return makeIcuError("Compiled catalog has no messages table", status);
    }

    auto result = std::vector<CatalogMessage>{};
    ::ures_resetIterator(messagesPtr.get());

    while (::ures_hasNext(messagesPtr.get()) != 0)
    {
      std::int32_t length = 0;
      char const* key = nullptr;
      status = U_ZERO_ERROR;
      auto const* value = ::ures_getNextString(messagesPtr.get(), &length, &key, &status);

      if (U_FAILURE(status) != 0 || value == nullptr || key == nullptr)
      {
        return makeIcuError("Could not read compiled catalog message", status);
      }

      auto pattern = std::string{};
      icu::UnicodeString{value, length}.toUTF8String(pattern);
      result.push_back({.id = key, .pattern = std::move(pattern)});
    }

    std::ranges::sort(result, {}, &CatalogMessage::id);
    return result;
  }

  std::string renderIcuResource(std::string_view const locale, std::span<CatalogMessage const> const messages)
  {
    auto result = std::format("{}:table {{\n  messages:table {{\n", locale);

    for (auto const& message : sortedMessages(messages))
    {
      result += std::format("    {} {{ \"{}\" }}\n", message.id, escapeResourceString(message.pattern));
    }

    result += "  }\n}\n";
    return result;
  }

  std::string renderResw(std::span<CatalogMessage const> const messages)
  {
    auto result = std::string{"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                              "<root>\n"
                              "  <resheader name=\"resmimetype\"><value>text/microsoft-resx</value></resheader>\n"
                              "  <resheader name=\"version\"><value>2.0</value></resheader>\n"
                              "  <resheader name=\"reader\"><value>System.Resources.ResXResourceReader, "
                              "System.Windows.Forms</value></resheader>\n"
                              "  <resheader name=\"writer\"><value>System.Resources.ResXResourceWriter, "
                              "System.Windows.Forms</value></resheader>\n"};

    for (auto const& message : sortedMessages(messages))
    {
      result += std::format("  <data name=\"{}\" xml:space=\"preserve\"><value>{}</value></data>\n",
                            escapeXml(message.id),
                            escapeXml(message.pattern));
    }

    result += "</root>\n";
    return result;
  }
} // namespace ao::i18n::detail
