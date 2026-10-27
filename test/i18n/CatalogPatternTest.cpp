// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "i18n/CatalogPattern.h"

#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace ao::i18n::detail::test
{
  TEST_CASE("CatalogPattern - validates named argument signatures and required branches", "[core][unit][catalog]")
  {
    auto signatureRes = messageArgumentSignature(
      "{state, select, playing {{count, plural, one {# track} other {# tracks}}} other {Stopped}}");
    REQUIRE(signatureRes);
    CHECK(*signatureRes == std::vector<MessageArgumentSignature>{
                             {.name = "count", .kind = MessageArgumentKind::Plural},
                             {.name = "state", .kind = MessageArgumentKind::Select},
                           });

    auto numberedRes = messageArgumentSignature("Hello {0}");
    REQUIRE_FALSE(numberedRes);
    CHECK(numberedRes.error().code == Error::Code::FormatRejected);

    auto missingOtherRes = messageArgumentSignature("{count, plural, one {One}}");
    REQUIRE_FALSE(missingOtherRes);
    CHECK(missingOtherRes.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("CatalogPattern - translation ids and argument kinds match English", "[core][unit][catalog]")
  {
    auto const root = std::array{
      CatalogMessage{.id = "plain", .pattern = "Plain"},
      CatalogMessage{.id = "count", .pattern = "{count, plural, one {One} other {Many}}"},
    };
    auto const valid = std::array{
      CatalogMessage{.id = "count", .pattern = "{count, plural, one {Eins} other {Mehrere}}"},
    };
    CHECK(validateTranslationCatalog(root, valid));

    auto const changed = std::array{
      CatalogMessage{.id = "count", .pattern = "{count, select, one {Eins} other {Mehrere}}"},
    };
    auto changedRes = validateTranslationCatalog(root, changed);
    REQUIRE_FALSE(changedRes);
    CHECK(changedRes.error().message.contains("different argument signature"));

    auto const unknown = std::array{CatalogMessage{.id = "extension", .pattern = "Text"}};
    auto unknownRes = validateTranslationCatalog(root, unknown);
    REQUIRE_FALSE(unknownRes);
    CHECK(unknownRes.error().message.contains("unknown message id"));
  }

  TEST_CASE("CatalogPattern - pseudo-localization preserves MessageFormat structure", "[core][unit][catalog]")
  {
    auto pseudoRes = pseudoLocalizePattern("Open {application}: {count, plural, one {# track} other {# tracks}}");
    REQUIRE(pseudoRes);
    CHECK(*pseudoRes == "[!! ÖÖpëëñ {application}: {count, plural, one {# trààçk} other {# trààçks}} !!]");

    auto const signatureRes = messageArgumentSignature(*pseudoRes);
    REQUIRE(signatureRes);
    CHECK(*signatureRes == std::vector<MessageArgumentSignature>{
                             {.name = "application", .kind = MessageArgumentKind::Value},
                             {.name = "count", .kind = MessageArgumentKind::Plural},
                           });

    auto const root = std::array{CatalogMessage{
      .id = "message", .pattern = "Open {application}: {count, plural, one {# track} other {# tracks}}"}};
    auto const pseudo = std::array{CatalogMessage{.id = "message", .pattern = *pseudoRes}};
    CHECK(validateTranslationCatalog(root, pseudo));
  }

  TEST_CASE("CatalogPattern - projects only the supported WinUI positional syntax", "[core][unit][catalog][winui]")
  {
    CHECK(unescapeIcuApostrophePairs("Owner''s {value}") == "Owner's {value}");

    auto projectedRes = projectWinUiPositionalPattern("Column ''{column}'' is unavailable", "column");
    REQUIRE(projectedRes);
    CHECK(*projectedRes == "Column '{0}' is unavailable");

    auto quotedSyntaxRes = projectWinUiPositionalPattern("Column '{column}' differs from {column}", "column");
    REQUIRE_FALSE(quotedSyntaxRes);
    CHECK(quotedSyntaxRes.error().message.contains("apostrophe-quoted syntax"));

    auto pluralRes = projectWinUiPositionalPattern("{count, plural, one {One} other {Many}}", "count");
    REQUIRE_FALSE(pluralRes);
    CHECK(pluralRes.error().message.contains("exactly one plain named argument"));
  }

  TEST_CASE("CatalogPattern - partial WinUI projections omit untranslated governed messages",
            "[core][unit][catalog][winui]")
  {
    auto const translated = std::array{
      CatalogMessage{.id = "winui_error", .pattern = "Fehler: {detail}"},
      CatalogMessage{.id = "winui_track_move_column_left", .pattern = "Nach links"},
    };

    auto projectedRes = projectWinUiResources(translated, MissingWinUiMessagePolicy::Omit);
    REQUIRE(projectedRes);

    // Compare the complete inventory without imposing an output-order contract.
    std::ranges::sort(*projectedRes, {}, &CatalogMessage::id);
    CHECK(*projectedRes == std::vector<CatalogMessage>{
                             {.id = "winui_error", .pattern = "Fehler: {0}"},
                             {.id = "winui_track_move_column_left", .pattern = "Nach links"},
                             {.id = "winui_track_move_column_left_button.Text", .pattern = "Nach links"},
                           });

    auto completeProjectionRes = projectWinUiResources(translated, MissingWinUiMessagePolicy::Reject);
    REQUIRE_FALSE(completeProjectionRes);
    CHECK(completeProjectionRes.error().message.contains("references unknown message id"));
  }

  TEST_CASE("CatalogPattern - RESW output is stable, sorted, and escaped", "[core][unit][catalog]")
  {
    auto const messages = std::array{
      CatalogMessage{.id = "z_last", .pattern = "A & B"},
      CatalogMessage{.id = "a_first", .pattern = "Use <value> and \"quotes\""},
    };

    auto const first = renderResw(messages);
    auto const second = renderResw(messages);
    CHECK(first == second);
    CHECK(
      first ==
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<root>\n"
      "  <resheader name=\"resmimetype\"><value>text/microsoft-resx</value></resheader>\n"
      "  <resheader name=\"version\"><value>2.0</value></resheader>\n"
      "  <resheader name=\"reader\"><value>System.Resources.ResXResourceReader, "
      "System.Windows.Forms</value></resheader>\n"
      "  <resheader name=\"writer\"><value>System.Resources.ResXResourceWriter, "
      "System.Windows.Forms</value></resheader>\n"
      "  <data name=\"a_first\" xml:space=\"preserve\"><value>Use &lt;value&gt; and &quot;quotes&quot;</value></data>\n"
      "  <data name=\"z_last\" xml:space=\"preserve\"><value>A &amp; B</value></data>\n"
      "</root>\n");
  }

  TEST_CASE("CatalogPattern - ICU resource output is sorted and escaped", "[core][unit][catalog]")
  {
    auto const messages = std::array{
      CatalogMessage{.id = "z_last", .pattern = "A & B"},
      CatalogMessage{.id = "a_first", .pattern = "Use <value> and \"quotes\""},
    };

    auto const resource = renderIcuResource("qps_Ploc", messages);
    CHECK(resource == "qps_Ploc:table {\n"
                      "  messages:table {\n"
                      "    a_first { \"Use <value> and \\\"quotes\\\"\" }\n"
                      "    z_last { \"A & B\" }\n"
                      "  }\n"
                      "}\n");
  }
} // namespace ao::i18n::detail::test
