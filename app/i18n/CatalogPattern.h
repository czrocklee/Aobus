// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::i18n::detail
{
  enum class MessageArgumentKind : std::uint8_t
  {
    Value,
    Simple,
    Choice,
    Plural,
    Select,
    SelectOrdinal,
  };

  struct MessageArgumentSignature final
  {
    std::string name;
    MessageArgumentKind kind = MessageArgumentKind::Value;

    bool operator==(MessageArgumentSignature const&) const = default;
  };

  struct CatalogMessage final
  {
    std::string id;
    std::string pattern;

    bool operator==(CatalogMessage const&) const = default;
  };

  enum class MissingWinUiMessagePolicy : std::uint8_t
  {
    Reject,
    Omit,
  };

  Result<std::vector<MessageArgumentSignature>> messageArgumentSignature(std::string_view pattern);
  Result<> validateTranslationCatalog(std::span<CatalogMessage const> root,
                                      std::span<CatalogMessage const> translation);
  Result<std::string> pseudoLocalizePattern(std::string_view pattern);
  std::string unescapeIcuApostrophePairs(std::string_view pattern);
  Result<std::string> projectWinUiPositionalPattern(std::string_view pattern, std::string_view argumentName);
  Result<std::vector<CatalogMessage>> projectWinUiResources(std::span<CatalogMessage const> messages,
                                                            MissingWinUiMessagePolicy missingMessagePolicy);

  Result<std::vector<CatalogMessage>> loadCompiledCatalog(std::string_view resourceDirectory, std::string_view locale);
  std::string renderIcuResource(std::string_view locale, std::span<CatalogMessage const> messages);
  std::string renderResw(std::span<CatalogMessage const> messages);
} // namespace ao::i18n::detail
