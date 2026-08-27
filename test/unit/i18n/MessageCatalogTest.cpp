// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/i18n/MessageCatalog.h>

#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
#include <gsl-lite/gsl-lite.hpp>
#include <unicode/uloc.h>
#include <unicode/utypes.h>
#endif

#include <array>
#include <atomic>
#include <barrier>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

namespace ao::i18n::test
{
  static_assert(std::constructible_from<MessageArgument, std::string_view, std::string_view>);
  static_assert(std::constructible_from<MessageArgument, std::string_view, std::int32_t>);
  static_assert(std::constructible_from<MessageArgument, std::string_view, std::size_t>);
  static_assert(std::constructible_from<MessageArgument, std::string_view, double>);
  static_assert(!std::constructible_from<MessageArgument, std::string_view, bool>);
  static_assert(!std::constructible_from<MessageArgument, std::string_view, MessageId>);

  TEST_CASE("MessageCatalog - admits canonical locale tags and applies explicit fallback", "[core][unit][catalog]")
  {
    auto englishRes = MessageCatalog::create("en-GB");
    REQUIRE(englishRes);
    CHECK(englishRes->requestedLocale() == "en-GB");
    auto englishMessageRes = englishRes->format(MessageId::PilotLibraryTitle);
    REQUIRE(englishMessageRes);
    CHECK(*englishMessageRes == ResolvedMessage{.text = "Music library", .locale = "en"});

    auto englishTextRes = englishRes->text(MessageId::PilotLibraryTitle);
    REQUIRE(englishTextRes);
    CHECK(*englishTextRes == "Music library");
    CHECK(requiredText(*englishRes, MessageId::PilotLibraryTitle) == "Music library");
    CHECK(requiredFormat(*englishRes, MessageId::PilotGreeting, {{"name", "Ada"}}) == "Welcome, Ada");

    for (auto const* const request : {"de-DE", "de-AT"})
    {
      auto germanRes = MessageCatalog::create(request);
      REQUIRE(germanRes);
      auto titleRes = germanRes->format(MessageId::PilotLibraryTitle);
      REQUIRE(titleRes);
      CHECK(*titleRes == ResolvedMessage{.text = "Musikbibliothek", .locale = "de"});

      auto fallbackRes = germanRes->format(MessageId::PilotEnglishFallback);
      REQUIRE(fallbackRes);
      CHECK(*fallbackRes == ResolvedMessage{.text = "English message fallback", .locale = "en"});
    }

    for (auto const* const request : {"zh-CN", "zh-Hans", "zh-Hans-CN", "zh"})
    {
      auto chineseRes = MessageCatalog::create(request);
      REQUIRE(chineseRes);
      auto titleRes = chineseRes->format(MessageId::PilotLibraryTitle);
      REQUIRE(titleRes);
      CHECK(titleRes->text == "音乐库");

      auto fallbackRes = chineseRes->format(MessageId::PilotEnglishFallback);
      REQUIRE(fallbackRes);
      CHECK(*fallbackRes == ResolvedMessage{.text = "English message fallback", .locale = "en"});

      auto simplifiedOnlyRes = chineseRes->format(MessageId::PilotSimplifiedOnlyProbe);
      REQUIRE(simplifiedOnlyRes);
      CHECK(*simplifiedOnlyRes == ResolvedMessage{.text = "仅简体中文探针", .locale = "zh-Hans"});
    }

    for (auto const* const request : {"zh-TW", "zh-HK", "zh-Hant", "zh-Hant-TW"})
    {
      auto traditionalRes = MessageCatalog::create(request);
      REQUIRE(traditionalRes);
      auto titleRes = traditionalRes->format(MessageId::PilotLibraryTitle);
      REQUIRE(titleRes);
      CHECK(titleRes->text == "音樂庫");

      auto fallbackRes = traditionalRes->format(MessageId::PilotEnglishFallback);
      REQUIRE(fallbackRes);
      CHECK(*fallbackRes == ResolvedMessage{.text = "English message fallback", .locale = "en"});

      auto simplifiedOnlyRes = traditionalRes->format(MessageId::PilotSimplifiedOnlyProbe);
      REQUIRE(simplifiedOnlyRes);
      CHECK(*simplifiedOnlyRes == ResolvedMessage{.text = "Simplified only probe", .locale = "en"});
    }

    for (auto const* const request : {"ja-JP", "ja"})
    {
      auto japaneseRes = MessageCatalog::create(request);
      REQUIRE(japaneseRes);
      auto titleRes = japaneseRes->format(MessageId::PilotLibraryTitle);
      REQUIRE(titleRes);
      CHECK(*titleRes == ResolvedMessage{.text = "音楽ライブラリ", .locale = "ja"});

      auto fallbackRes = japaneseRes->format(MessageId::PilotEnglishFallback);
      REQUIRE(fallbackRes);
      CHECK(*fallbackRes == ResolvedMessage{.text = "English message fallback", .locale = "en"});
    }

    for (auto const* const request : {"es-ES", "es"})
    {
      auto spanishRes = MessageCatalog::create(request);
      REQUIRE(spanishRes);
      auto titleRes = spanishRes->format(MessageId::PilotLibraryTitle);
      REQUIRE(titleRes);
      CHECK(*titleRes == ResolvedMessage{.text = "Biblioteca de música", .locale = "es"});

      auto fallbackRes = spanishRes->format(MessageId::PilotEnglishFallback);
      REQUIRE(fallbackRes);
      CHECK(*fallbackRes == ResolvedMessage{.text = "English message fallback", .locale = "en"});
    }

    for (auto const* const request : {"fr-FR", "fr"})
    {
      auto frenchRes = MessageCatalog::create(request);
      REQUIRE(frenchRes);
      auto titleRes = frenchRes->format(MessageId::PilotLibraryTitle);
      REQUIRE(titleRes);
      CHECK(*titleRes == ResolvedMessage{.text = "Bibliothèque musicale", .locale = "fr"});

      auto fallbackRes = frenchRes->format(MessageId::PilotEnglishFallback);
      REQUIRE(fallbackRes);
      CHECK(*fallbackRes == ResolvedMessage{.text = "English message fallback", .locale = "en"});
    }

    auto unsupportedRes = MessageCatalog::create("sv-SE");
    REQUIRE(unsupportedRes);
    auto unsupportedTitleRes = unsupportedRes->format(MessageId::PilotLibraryTitle);
    REQUIRE(unsupportedTitleRes);
    CHECK(*unsupportedTitleRes == ResolvedMessage{.text = "Music library", .locale = "en"});

    auto invalidRes = MessageCatalog::create("not a locale");
    REQUIRE_FALSE(invalidRes);
    CHECK(invalidRes.error().code == Error::Code::InvalidInput);
  }

  TEST_CASE("MessageCatalog - exposes the internal root resource as English", "[core][regression][catalog]")
  {
    auto rootRes = MessageCatalog::create("root");
    REQUIRE(rootRes);
    CHECK(rootRes->requestedLocale() == "en");

    auto rootCountRes = rootRes->format(MessageId::PilotTrackCount, {{"count", std::size_t{1}}});
    REQUIRE(rootCountRes);
    CHECK(*rootCountRes == ResolvedMessage{.text = "1 track", .locale = "en"});

    auto undeterminedRes = MessageCatalog::create("und");
    REQUIRE(undeterminedRes);
    CHECK(undeterminedRes->requestedLocale() == "und");

    auto fallbackCountRes = undeterminedRes->format(MessageId::PilotTrackCount, {{"count", std::size_t{1}}});
    REQUIRE(fallbackCountRes);
    CHECK(*fallbackCountRes == ResolvedMessage{.text = "1 track", .locale = "en"});
  }

#ifndef _WIN32

  TEST_CASE("MessageCatalog - malformed operating-system locale falls back to English", "[core][regression][catalog]")
  {
    auto const originalLocale = std::string{::uloc_getDefault()};
    auto const restoreLocale = gsl_lite::finally(
      [&originalLocale]
      {
        auto restoreStatus = U_ZERO_ERROR;
        ::uloc_setDefault(originalLocale.c_str(), &restoreStatus);
      });

    auto status = U_ZERO_ERROR;
    ::uloc_setDefault("12345", &status);
    REQUIRE(U_SUCCESS(status));

    auto catalogRes = MessageCatalog::createForSystemLocale();
    REQUIRE(catalogRes);
    CHECK(catalogRes->requestedLocale() == "en");

    auto titleRes = catalogRes->format(MessageId::PilotLibraryTitle);
    REQUIRE(titleRes);
    CHECK(*titleRes == ResolvedMessage{.text = "Music library", .locale = "en"});
  }
#endif

  TEST_CASE("MessageCatalog - formats reordered, plural, and select arguments", "[core][unit][catalog]")
  {
    auto catalogRes = MessageCatalog::create("de-DE");
    REQUIRE(catalogRes);

    auto const copyArguments = std::array{
      MessageArgument{"count", std::size_t{2}},
      MessageArgument{"destination", "Archiv"},
    };
    auto copyRes = catalogRes->format(MessageId::PilotCopySummary, copyArguments);
    REQUIRE(copyRes);
    CHECK(copyRes->text == "Nach Archiv wurden 2 Titel kopiert");

    auto const oneArgument = std::array{MessageArgument{"count", std::int32_t{1}}};
    auto oneRes = catalogRes->format(MessageId::PilotTrackCount, oneArgument);
    REQUIRE(oneRes);
    CHECK(oneRes->text == "1 Titel");

    auto manyRes = catalogRes->format(MessageId::PilotTrackCount, {{"count", std::size_t{2}}});
    REQUIRE(manyRes);
    CHECK(manyRes->text == "2 Titel");

    auto const stateArguments = std::array{MessageArgument{"state", "paused"}};
    auto stateRes = catalogRes->format(MessageId::PilotPlaybackState, stateArguments);
    REQUIRE(stateRes);
    CHECK(stateRes->text == "Pausiert");

    auto chineseCatalogRes = MessageCatalog::create("zh-CN");
    REQUIRE(chineseCatalogRes);
    auto zhOneRes = chineseCatalogRes->format(MessageId::PilotTrackCount, oneArgument);
    REQUIRE(zhOneRes);
    CHECK(zhOneRes->text == "1 首曲目");
    auto zhStateRes = chineseCatalogRes->format(MessageId::PilotPlaybackState, stateArguments);
    REQUIRE(zhStateRes);
    CHECK(zhStateRes->text == "已暂停");

    auto traditionalCatalogRes = MessageCatalog::create("zh-TW");
    REQUIRE(traditionalCatalogRes);
    auto zhHantOneRes = traditionalCatalogRes->format(MessageId::PilotTrackCount, oneArgument);
    REQUIRE(zhHantOneRes);
    CHECK(zhHantOneRes->text == "1 首曲目");
    auto zhHantStateRes = traditionalCatalogRes->format(MessageId::PilotPlaybackState, stateArguments);
    REQUIRE(zhHantStateRes);
    CHECK(zhHantStateRes->text == "已暫停");

    auto japaneseCatalogRes = MessageCatalog::create("ja-JP");
    REQUIRE(japaneseCatalogRes);
    auto jaOneRes = japaneseCatalogRes->format(MessageId::PilotTrackCount, oneArgument);
    REQUIRE(jaOneRes);
    CHECK(jaOneRes->text == "1 曲");
    auto jaStateRes = japaneseCatalogRes->format(MessageId::PilotPlaybackState, stateArguments);
    REQUIRE(jaStateRes);
    CHECK(jaStateRes->text == "一時停止");

    auto spanishCatalogRes = MessageCatalog::create("es-ES");
    REQUIRE(spanishCatalogRes);
    auto esOneRes = spanishCatalogRes->format(MessageId::PilotTrackCount, oneArgument);
    REQUIRE(esOneRes);
    CHECK(esOneRes->text == "1 pista");
    auto esManyRes = spanishCatalogRes->format(MessageId::PilotTrackCount, {{"count", std::size_t{2}}});
    REQUIRE(esManyRes);
    CHECK(esManyRes->text == "2 pistas");
    auto esStateRes = spanishCatalogRes->format(MessageId::PilotPlaybackState, stateArguments);
    REQUIRE(esStateRes);
    CHECK(esStateRes->text == "Pausado");

    auto frenchCatalogRes = MessageCatalog::create("fr-FR");
    REQUIRE(frenchCatalogRes);
    auto frOneRes = frenchCatalogRes->format(MessageId::PilotTrackCount, oneArgument);
    REQUIRE(frOneRes);
    CHECK(frOneRes->text == "1 morceau");
    auto frManyRes = frenchCatalogRes->format(MessageId::PilotTrackCount, {{"count", std::size_t{2}}});
    REQUIRE(frManyRes);
    CHECK(frManyRes->text == "2 morceaux");
    auto frStateRes = frenchCatalogRes->format(MessageId::PilotPlaybackState, stateArguments);
    REQUIRE(frStateRes);
    CHECK(frStateRes->text == "En pause");
  }

  TEST_CASE("MessageCatalog - pseudo output expands literals but preserves argument values", "[core][unit][catalog]")
  {
    auto catalogRes = MessageCatalog::create("qps-ploc");
    REQUIRE(catalogRes);
    CHECK(catalogRes->requestedLocale() == "qps-ploc");

    auto const arguments = std::array{MessageArgument{"application", "Aobus"}};
    auto messageRes = catalogRes->format(MessageId::PilotPseudoProbe, arguments);
    REQUIRE(messageRes);
    CHECK(messageRes->locale == "qps-ploc");
    CHECK(messageRes->text.starts_with("[!! "));
    CHECK(messageRes->text.ends_with(" !!]"));
    CHECK(messageRes->text.contains("Aobus"));
    CHECK(messageRes->text.size() > std::string_view{"Open Aobus music library"}.size());
  }

  TEST_CASE("MessageCatalog - final frontend slice resolves localized wrappers and accessibility copy",
            "[core][unit][catalog]")
  {
    auto englishRes = MessageCatalog::create("en");
    REQUIRE(englishRes);
    auto selectedTitleRes = englishRes->format(MessageId::GtkTrackPropertiesSelectedTitle, {{"count", 2}});
    REQUIRE(selectedTitleRes);
    CHECK(selectedTitleRes->text == "Properties — 2 tracks selected");

    auto catalogRes = MessageCatalog::create("de-DE");
    REQUIRE(catalogRes);

    auto editTextRes = catalogRes->text(MessageId::GtkEditValue);
    REQUIRE(editTextRes);
    CHECK(*editTextRes == "Wert bearbeiten");

    auto confirmationRes = catalogRes->format(MessageId::GtkSaveLayoutDefaultsMessage, {{"preset", "modern"}});
    REQUIRE(confirmationRes);
    CHECK(confirmationRes->text.contains("'modern'"));
    CHECK(confirmationRes->text.contains("Bereichsgrößen"));

    auto startupRes = catalogRes->format(MessageId::GtkStartupOpenLibraryFailed, {{"detail", "E42"}});
    REQUIRE(startupRes);
    CHECK(startupRes->text == "Aobus konnte die Bibliothek nicht öffnen: E42");

    auto moveColumnRes = catalogRes->text(MessageId::WinUiTrackMoveColumnLeft);
    REQUIRE(moveColumnRes);
    CHECK(*moveColumnRes == "Nach links verschieben");
  }

  TEST_CASE("MessageCatalog - rejects missing, duplicate, unexpected, and mistyped arguments", "[core][unit][catalog]")
  {
    auto catalogRes = MessageCatalog::create("en");
    REQUIRE(catalogRes);

    auto missingRes = catalogRes->format(MessageId::PilotGreeting);
    REQUIRE_FALSE(missingRes);
    CHECK(missingRes.error().code == Error::Code::InvalidInput);

    auto const duplicate = std::array{
      MessageArgument{"name", "A"},
      MessageArgument{"name", "B"},
    };
    auto duplicateRes = catalogRes->format(MessageId::PilotGreeting, duplicate);
    REQUIRE_FALSE(duplicateRes);

    auto const unexpected = std::array{MessageArgument{"other", "A"}};
    auto unexpectedRes = catalogRes->format(MessageId::PilotGreeting, unexpected);
    REQUIRE_FALSE(unexpectedRes);

    auto const wrongType = std::array{MessageArgument{"count", "two"}};
    auto wrongTypeRes = catalogRes->format(MessageId::PilotTrackCount, wrongType);
    REQUIRE_FALSE(wrongTypeRes);

    auto unknownRes = catalogRes->format(MessageId::Count);
    REQUIRE_FALSE(unknownRes);
    CHECK(unknownRes.error().code == Error::Code::NotFound);

    auto parameterizedTextRes = catalogRes->text(MessageId::PilotGreeting);
    REQUIRE_FALSE(parameterizedTextRes);
    CHECK(parameterizedTextRes.error().code == Error::Code::InvalidInput);

    auto oversizedRes =
      catalogRes->format(MessageId::PilotTrackCount, {{"count", std::numeric_limits<std::uint64_t>::max()}});
    REQUIRE_FALSE(oversizedRes);
    CHECK(oversizedRes.error().code == Error::Code::InvalidInput);
  }

  TEST_CASE("MessageCatalog - one published catalog formats concurrently", "[core][unit][catalog][concurrency]")
  {
    auto catalogRes = MessageCatalog::create("de-DE");
    REQUIRE(catalogRes);
    auto const catalog = *catalogRes;

    constexpr std::size_t kThreadCount = 8;
    auto start = std::barrier{static_cast<std::ptrdiff_t>(kThreadCount)};
    auto succeeded = std::atomic{true};
    auto threads = std::vector<std::jthread>{};
    threads.reserve(kThreadCount);

    for (std::size_t threadIndex = 0; threadIndex < kThreadCount; ++threadIndex)
    {
      threads.emplace_back(
        [catalog, &start, &succeeded, threadIndex]
        {
          start.arrive_and_wait();

          for (std::size_t iteration = 0; iteration < 200; ++iteration)
          {
            auto const arguments = std::array{MessageArgument{"count", ((threadIndex + iteration) % 3) + 1}};
            auto messageRes = catalog.format(MessageId::PilotTrackCount, arguments);

            if (!messageRes || messageRes->locale != "de" || !messageRes->text.contains("Titel"))
            {
              succeeded.store(false, std::memory_order_relaxed);
              return;
            }
          }
        });
    }

    threads.clear();
    CHECK(succeeded.load(std::memory_order_relaxed));
  }
} // namespace ao::i18n::test
