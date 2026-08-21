// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/i18n/MessageCatalog.h>

#include "CatalogPattern.h"
#include "EmbeddedCatalogData.h"
#include "MessageIds.h"
#include "SystemLocale.h"
#include <ao/Error.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/String.h>
#include <ao/utility/UnicodeText.h>

#include <unicode/fmtable.h>
#include <unicode/locid.h>
#include <unicode/msgfmt.h>
#include <unicode/stringpiece.h>
#include <unicode/udata.h>
#include <unicode/uloc.h>
#include <unicode/unistr.h>
#include <unicode/ures.h>
#include <unicode/utypes.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::i18n
{
  namespace
  {
    constexpr auto kCatalogPackageName = "aobus_messages";

    struct ResourceBundleDeleter final
    {
      void operator()(UResourceBundle* const bundle) const noexcept { ::ures_close(bundle); }
    };

    using ResourceBundlePtr = std::unique_ptr<UResourceBundle, ResourceBundleDeleter>;

    struct LocaleCandidate final
    {
      std::string resourceLocale;
      std::string publicLocale;
      std::string formatLocale;
    };

    struct LoadedBundle final
    {
      LocaleCandidate locale;
      ResourceBundlePtr bundlePtr;
      ResourceBundlePtr messagesPtr;
    };

    struct CompiledMessage final
    {
      std::string locale;
      std::vector<detail::MessageArgumentSignature> signature;
      std::unique_ptr<icu::MessageFormat> formatterPtr;
      std::optional<std::string> optText;
    };

    struct RegistrationState final
    {
      std::once_flag once;
      std::optional<Error> optError;
    };

    RegistrationState& registrationState()
    {
      static auto state = RegistrationState{};
      return state;
    }

    std::unexpected<Error> makeIcuError(std::string_view const operation,
                                        UErrorCode const status,
                                        Error::Code const fallback = Error::Code::InitFailed)
    {
      auto const code = status == U_MEMORY_ALLOCATION_ERROR ? Error::Code::ResourceExhausted : fallback;
      return makeError(code, std::format("{}: {}", operation, ::u_errorName(status)));
    }

    bool asciiCaseEqual(std::string_view const left, std::string_view const right) noexcept
    {
      if (left.size() != right.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < left.size(); ++index)
      {
        if (utility::toAsciiLower(left[index]) != utility::toAsciiLower(right[index]))
        {
          return false;
        }
      }

      return true;
    }

    Result<> registerEmbeddedCatalogData()
    {
      auto& state = registrationState();
      std::call_once(
        state.once,
        [&state]
        {
          auto const data = detail::embeddedCatalogData();

          if (data.empty() || !utility::bytes::isAligned(data.data(), 16))
          {
            state.optError =
              Error{.code = Error::Code::CorruptData, .message = "Embedded ICU catalog data is empty or misaligned"};
            return;
          }

          UErrorCode status = U_ZERO_ERROR;
          ::udata_setAppData(kCatalogPackageName, data.data(), &status);

          if (status != U_ZERO_ERROR)
          {
            state.optError = Error{
              .code = status == U_MEMORY_ALLOCATION_ERROR ? Error::Code::ResourceExhausted : Error::Code::InitFailed,
              .message = std::format("Could not register embedded ICU catalog data: {}", ::u_errorName(status)),
            };
          }
        });

      if (state.optError)
      {
        return std::unexpected{*state.optError};
      }

      return {};
    }

    Result<std::string> localeIdFromLanguageTag(std::string_view const tag)
    {
      if (tag.empty())
      {
        return makeError(Error::Code::InvalidInput, "A locale tag cannot be empty");
      }

      if (tag.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) || tag.contains('\0'))
      {
        return makeError(Error::Code::InvalidInput, "The locale tag is not a complete BCP 47 value");
      }

      auto localeId = std::array<char, ULOC_FULLNAME_CAPACITY>{};
      std::int32_t parsedLength = 0;
      UErrorCode status = U_ZERO_ERROR;
      auto const length = ::uloc_forLanguageTag(
        std::string{tag}.c_str(), localeId.data(), static_cast<std::int32_t>(localeId.size()), &parsedLength, &status);

      if (U_FAILURE(status) != 0 || std::cmp_not_equal(parsedLength, tag.size()))
      {
        return makeError(
          Error::Code::InvalidInput, std::format("Invalid BCP 47 locale tag '{}': {}", tag, ::u_errorName(status)));
      }

      return std::string{localeId.data(), static_cast<std::size_t>(length)};
    }

    Result<std::string> languageTagFromLocaleId(std::string_view const localeId)
    {
      auto output = std::array<char, ULOC_FULLNAME_CAPACITY>{};
      UErrorCode status = U_ZERO_ERROR;
      auto const length = ::uloc_toLanguageTag(
        std::string{localeId}.c_str(), output.data(), static_cast<std::int32_t>(output.size()), 1, &status);

      if (U_FAILURE(status) != 0)
      {
        return makeIcuError("Could not canonicalize locale tag", status, Error::Code::InvalidInput);
      }

      return std::string{output.data(), static_cast<std::size_t>(length)};
    }

    Result<std::string> parentLocaleId(std::string_view const localeId)
    {
      auto output = std::array<char, ULOC_FULLNAME_CAPACITY>{};
      UErrorCode status = U_ZERO_ERROR;
      auto const length = ::uloc_getParent(
        std::string{localeId}.c_str(), output.data(), static_cast<std::int32_t>(output.size()), &status);

      if (U_FAILURE(status) != 0)
      {
        return makeIcuError("Could not resolve locale parent", status, Error::Code::InvalidInput);
      }

      return std::string{output.data(), static_cast<std::size_t>(length)};
    }

    Result<std::pair<std::string, std::vector<LocaleCandidate>>> localeCandidates(std::string_view const requestedTag)
    {
      auto localeIdRes = localeIdFromLanguageTag(requestedTag);

      if (!localeIdRes)
      {
        return std::unexpected{localeIdRes.error()};
      }

      auto canonicalTagRes = languageTagFromLocaleId(*localeIdRes);

      if (!canonicalTagRes)
      {
        return std::unexpected{canonicalTagRes.error()};
      }

      if (*localeIdRes == "root")
      {
        return std::pair{std::string{"en"},
                         std::vector<LocaleCandidate>{
                           {.resourceLocale = "root", .publicLocale = "en", .formatLocale = "en"},
                         }};
      }

      if (asciiCaseEqual(*canonicalTagRes, "qps-ploc"))
      {
        return std::pair{std::string{"qps-ploc"},
                         std::vector<LocaleCandidate>{
                           {.resourceLocale = "qps_Ploc", .publicLocale = "qps-ploc", .formatLocale = "en"},
                           {.resourceLocale = "root", .publicLocale = "en", .formatLocale = "en"},
                         }};
      }

      auto candidates = std::vector<LocaleCandidate>{};
      auto current = *localeIdRes;

      while (!current.empty())
      {
        auto tagRes = languageTagFromLocaleId(current);

        if (!tagRes)
        {
          return std::unexpected{tagRes.error()};
        }

        candidates.push_back({.resourceLocale = current, .publicLocale = *tagRes, .formatLocale = std::move(*tagRes)});

        auto parentRes = parentLocaleId(current);

        if (!parentRes)
        {
          return std::unexpected{parentRes.error()};
        }

        current = std::move(*parentRes);
      }

      candidates.push_back({.resourceLocale = "root", .publicLocale = "en", .formatLocale = "en"});
      return std::pair{std::move(*canonicalTagRes), std::move(candidates)};
    }

    Result<std::vector<LoadedBundle>> loadBundles(std::span<LocaleCandidate const> const candidates)
    {
      auto result = std::vector<LoadedBundle>{};

      for (auto const& candidate : candidates)
      {
        UErrorCode status = U_ZERO_ERROR;
        auto bundlePtr =
          ResourceBundlePtr{::ures_openDirect(kCatalogPackageName, candidate.resourceLocale.c_str(), &status)};

        if (status == U_MISSING_RESOURCE_ERROR)
        {
          continue;
        }

        if (U_FAILURE(status) != 0 || bundlePtr == nullptr)
        {
          return makeIcuError("Could not open embedded catalog bundle", status, Error::Code::CorruptData);
        }

        status = U_ZERO_ERROR;
        auto messagesPtr = ResourceBundlePtr{::ures_getByKey(bundlePtr.get(), "messages", nullptr, &status)};

        if (U_FAILURE(status) != 0 || messagesPtr == nullptr)
        {
          return makeIcuError("Embedded catalog bundle has no messages table", status, Error::Code::CorruptData);
        }

        result.push_back(
          {.locale = candidate, .bundlePtr = std::move(bundlePtr), .messagesPtr = std::move(messagesPtr)});
      }

      if (result.empty() || result.back().locale.resourceLocale != "root")
      {
        return makeError(Error::Code::CorruptData, "The embedded catalog has no English root bundle");
      }

      return result;
    }

    Result<std::optional<std::string>> patternFromBundle(LoadedBundle const& bundle, std::string_view const key)
    {
      UErrorCode status = U_ZERO_ERROR;
      std::int32_t length = 0;
      auto const* value = ::ures_getStringByKey(bundle.messagesPtr.get(), std::string{key}.c_str(), &length, &status);

      if (status == U_MISSING_RESOURCE_ERROR)
      {
        return std::nullopt;
      }

      if (U_FAILURE(status) != 0 || value == nullptr)
      {
        return makeIcuError("Could not read embedded catalog message", status, Error::Code::CorruptData);
      }

      auto pattern = std::string{};
      icu::UnicodeString{value, length}.toUTF8String(pattern);
      return pattern;
    }

    bool argumentKindAccepts(detail::MessageArgumentKind const kind, MessageArgumentValue const& value) noexcept
    {
      switch (kind)
      {
        case detail::MessageArgumentKind::Choice:
        case detail::MessageArgumentKind::Plural:
        case detail::MessageArgumentKind::SelectOrdinal:
          return std::holds_alternative<std::int64_t>(value) || std::holds_alternative<std::uint64_t>(value) ||
                 std::holds_alternative<double>(value);
        case detail::MessageArgumentKind::Select: return std::holds_alternative<std::string_view>(value);
        case detail::MessageArgumentKind::Value:
        case detail::MessageArgumentKind::Simple: return true;
      }

      return false;
    }
  } // namespace

  struct MessageCatalog::Impl final
  {
    std::string requestedLocale;
    std::array<CompiledMessage, detail::kMessageDefinitions.size()> messages;
    mutable std::array<std::mutex, detail::kMessageDefinitions.size()> formatterMutexes;
  };

  MessageCatalog::MessageCatalog(std::shared_ptr<Impl const> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  MessageCatalog::~MessageCatalog() = default;

  Result<MessageCatalog> MessageCatalog::create(std::string_view const localeTag)
  {
    if (auto registrationRes = registerEmbeddedCatalogData(); !registrationRes)
    {
      return std::unexpected{registrationRes.error()};
    }

    auto localeRes = localeCandidates(localeTag);

    if (!localeRes)
    {
      return std::unexpected{localeRes.error()};
    }

    auto bundlesRes = loadBundles(localeRes->second);

    if (!bundlesRes)
    {
      return std::unexpected{bundlesRes.error()};
    }

    auto implPtr = std::make_shared<Impl>();
    implPtr->requestedLocale = std::move(localeRes->first);

    for (auto const& definition : detail::kMessageDefinitions)
    {
      auto optSelectedPattern = std::optional<std::string>{};
      auto const* selectedBundle = static_cast<LoadedBundle const*>(nullptr);

      for (auto const& bundle : *bundlesRes)
      {
        auto patternRes = patternFromBundle(bundle, definition.key);

        if (!patternRes)
        {
          return std::unexpected{patternRes.error()};
        }

        if (*patternRes)
        {
          optSelectedPattern = std::move(**patternRes);
          selectedBundle = &bundle;
          break;
        }
      }

      if (!optSelectedPattern || selectedBundle == nullptr)
      {
        return makeError(
          Error::Code::CorruptData, std::format("English root catalog is missing message '{}'", definition.key));
      }

      auto signatureRes = detail::messageArgumentSignature(*optSelectedPattern);

      if (!signatureRes)
      {
        return makeError(signatureRes.error().code,
                         std::format("Embedded message '{}': {}", definition.key, signatureRes.error().message));
      }

      UErrorCode status = U_ZERO_ERROR;
      auto formatterPtr = std::make_unique<icu::MessageFormat>(
        icu::UnicodeString::fromUTF8(
          icu::StringPiece{optSelectedPattern->data(), static_cast<std::int32_t>(optSelectedPattern->size())}),
        icu::Locale::createFromName(selectedBundle->locale.formatLocale.c_str()),
        status);

      if (U_FAILURE(status) != 0)
      {
        return makeIcuError("Could not construct embedded MessageFormat", status, Error::Code::CorruptData);
      }

      auto optText = std::optional<std::string>{};

      if (signatureRes->empty())
      {
        status = U_ZERO_ERROR;
        auto output = icu::UnicodeString{};
        auto const names = std::array<icu::UnicodeString, 0>{};
        auto const values = std::array<icu::Formattable, 0>{};
        formatterPtr->format(names.data(), values.data(), 0, output, status);

        if (U_FAILURE(status) != 0)
        {
          return makeIcuError("Could not resolve embedded fixed message", status, Error::Code::CorruptData);
        }

        optText.emplace();
        output.toUTF8String(*optText);
      }

      implPtr->messages[detail::messageIndex(definition.id)] = CompiledMessage{
        .locale = selectedBundle->locale.publicLocale,
        .signature = std::move(*signatureRes),
        .formatterPtr = std::move(formatterPtr),
        .optText = std::move(optText),
      };
    }

    return MessageCatalog{std::move(implPtr)};
  }

  Result<MessageCatalog> MessageCatalog::createForSystemLocale()
  {
    auto systemLocaleRes = detail::systemLocaleTag();

    if (!systemLocaleRes)
    {
      return create("en");
    }

    return create(*systemLocaleRes);
  }

  std::string_view MessageCatalog::requestedLocale() const noexcept
  {
    return _implPtr->requestedLocale;
  }

  Result<std::string_view> MessageCatalog::text(MessageId const id) const
  {
    auto const index = detail::messageIndex(id);

    if (index >= _implPtr->messages.size())
    {
      return makeError(Error::Code::NotFound, "Unknown localization message id");
    }

    auto const& message = _implPtr->messages[index];

    if (!message.optText)
    {
      return makeError(Error::Code::InvalidInput, "Localized message requires arguments");
    }

    return std::string_view{*message.optText};
  }

  Result<ResolvedMessage> MessageCatalog::format(MessageId const id,
                                                 std::span<MessageArgument const> const arguments) const
  {
    auto const index = detail::messageIndex(id);

    if (index >= _implPtr->messages.size())
    {
      return makeError(Error::Code::NotFound, "Unknown localization message id");
    }

    auto const& message = _implPtr->messages[index];

    if (arguments.size() != message.signature.size())
    {
      return makeError(
        Error::Code::InvalidInput,
        std::format("Message expects {} arguments but received {}", message.signature.size(), arguments.size()));
    }

    for (std::size_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex)
    {
      auto const& argument = arguments[argumentIndex];

      if (auto validRes = utility::validateUtf8(argument.name); !validRes)
      {
        return makeError(Error::Code::InvalidInput, "Message argument name is not valid UTF-8");
      }

      if (auto const* text = std::get_if<std::string_view>(&argument.value); text != nullptr)
      {
        if (auto validRes = utility::validateUtf8(*text); !validRes)
        {
          return makeError(
            Error::Code::InvalidInput, std::format("Message argument '{}' is not valid UTF-8", argument.name));
        }
      }

      if (std::ranges::find(arguments.first(argumentIndex), argument.name, &MessageArgument::name) !=
          arguments.first(argumentIndex).end())
      {
        return makeError(
          Error::Code::InvalidInput, std::format("Message argument '{}' appears more than once", argument.name));
      }

      auto const expected =
        std::ranges::find(message.signature, argument.name, &detail::MessageArgumentSignature::name);

      if (expected == message.signature.end())
      {
        return makeError(
          Error::Code::InvalidInput, std::format("Message does not accept argument '{}'", argument.name));
      }

      if (!argumentKindAccepts(expected->kind, argument.value))
      {
        return makeError(
          Error::Code::InvalidInput, std::format("Message argument '{}' has the wrong value kind", argument.name));
      }

      if (auto const* unsignedValue = std::get_if<std::uint64_t>(&argument.value);
          unsignedValue != nullptr &&
          *unsignedValue > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      {
        return makeError(Error::Code::InvalidInput,
                         std::format("Message argument '{}' exceeds the supported integer range", argument.name));
      }
    }

    auto names = std::vector<icu::UnicodeString>{};
    auto values = std::vector<icu::Formattable>{};
    names.reserve(arguments.size());
    values.reserve(arguments.size());

    for (auto const& argument : arguments)
    {
      names.push_back(icu::UnicodeString::fromUTF8(
        icu::StringPiece{argument.name.data(), static_cast<std::int32_t>(argument.name.size())}));
      std::visit(
        [&values](auto const& value)
        {
          using Value = std::remove_cvref_t<decltype(value)>;

          if constexpr (std::same_as<Value, std::string_view>)
          {
            values.emplace_back(
              icu::UnicodeString::fromUTF8(icu::StringPiece{value.data(), static_cast<std::int32_t>(value.size())}));
          }
          else if constexpr (std::same_as<Value, std::uint64_t>)
          {
            values.emplace_back(static_cast<std::int64_t>(value));
          }
          else
          {
            values.emplace_back(value);
          }
        },
        argument.value);
    }

    auto output = icu::UnicodeString{};
    UErrorCode status = U_ZERO_ERROR;
    auto lock = std::scoped_lock{_implPtr->formatterMutexes[index]};
    message.formatterPtr->format(
      names.data(), values.data(), static_cast<std::int32_t>(arguments.size()), output, status);

    if (U_FAILURE(status) != 0)
    {
      return makeIcuError("Could not format localized message", status, Error::Code::InvalidInput);
    }

    auto text = std::string{};
    output.toUTF8String(text);
    return ResolvedMessage{.text = std::move(text), .locale = message.locale};
  }

  Result<ResolvedMessage> MessageCatalog::format(MessageId const id,
                                                 std::initializer_list<MessageArgument> const arguments) const
  {
    return format(id, std::span{arguments.begin(), arguments.size()});
  }
} // namespace ao::i18n
