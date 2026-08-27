// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace ao::i18n
{
  enum class MessageId : std::uint16_t
  {
#define AO_I18N_MESSAGE(id, key) id,    // NOLINT(cppcoreguidelines-macro-usage)
#include <ao/i18n/MessageInventory.def> // NOLINT(aobus-include-convention)
#undef AO_I18N_MESSAGE
    Count,
  };

  using MessageArgumentValue = std::variant<std::string_view, std::int64_t, std::uint64_t, double>;

  struct MessageArgument final
  {
    std::string_view name;
    MessageArgumentValue value;

    MessageArgument(std::string_view argumentName, std::string_view argumentValue) noexcept
      : name{argumentName}, value{argumentValue}
    {
    }

    template<std::integral Value>
      requires(!std::same_as<std::remove_cv_t<Value>, bool> && sizeof(Value) <= sizeof(std::uint64_t))
    MessageArgument(std::string_view argumentName, Value argumentValue) noexcept
      : name{argumentName}, value{integerValue(argumentValue)}
    {
    }

    template<std::floating_point Value>
    MessageArgument(std::string_view argumentName, Value argumentValue) noexcept
      : name{argumentName}, value{static_cast<double>(argumentValue)}
    {
    }

  private:
    template<std::integral Value>
    static MessageArgumentValue integerValue(Value value) noexcept
    {
      if constexpr (std::is_signed_v<Value>)
      {
        return static_cast<std::int64_t>(value);
      }
      else
      {
        return static_cast<std::uint64_t>(value);
      }
    }
  };

  struct ResolvedMessage final
  {
    std::string text;
    std::string locale;

    bool operator==(ResolvedMessage const&) const = default;
  };

  /**
   * Immutable interactive message catalog selected at process startup.
   *
   * Construction admits one explicit locale and eagerly loads and parses every
   * pattern. Published instances share logically immutable patterns and
   * serialize access to each ICU formatter; argument conversion and output are
   * owned by one formatting call.
   */
  class MessageCatalog final
  {
  public:
    MessageCatalog(MessageCatalog const&) = default;
    MessageCatalog(MessageCatalog&&) noexcept = default;
    MessageCatalog& operator=(MessageCatalog const&) = default;
    MessageCatalog& operator=(MessageCatalog&&) noexcept = default;
    ~MessageCatalog();

    static Result<MessageCatalog> create(std::string_view localeTag);
    static Result<MessageCatalog> createForSystemLocale();

    std::string_view requestedLocale() const noexcept;
    Result<std::string_view> text(MessageId id) const;
    Result<ResolvedMessage> format(MessageId id, std::span<MessageArgument const> arguments = {}) const;
    Result<ResolvedMessage> format(MessageId id, std::initializer_list<MessageArgument> arguments) const;

  private:
    struct Impl;

    explicit MessageCatalog(std::shared_ptr<Impl const> implPtr);

    std::shared_ptr<Impl const> _implPtr;
  };

  std::string_view requiredText(MessageCatalog const& catalog, MessageId id);
  std::string requiredFormat(MessageCatalog const& catalog, MessageId id, std::span<MessageArgument const> arguments);
  std::string requiredFormat(MessageCatalog const& catalog,
                             MessageId id,
                             std::initializer_list<MessageArgument> arguments);
} // namespace ao::i18n
