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
    CHECK(pseudoRes->starts_with("[!! "));
    CHECK(pseudoRes->ends_with(" !!]"));
    CHECK(pseudoRes->contains("{application}"));
    CHECK(pseudoRes->contains("{count, plural, one {"));
    CHECK(pseudoRes->contains(" other {"));
    CHECK(pseudoRes->contains('#'));
    CHECK(pseudoRes->contains("ÖÖ"));

    REQUIRE(messageArgumentSignature(*pseudoRes));
    CHECK(*messageArgumentSignature(*pseudoRes) ==
          *messageArgumentSignature("Open {application}: "
                                    "{count, plural, one {# track} other {# tracks}}"));

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

    auto const findMessage = [&projectedRes](std::string_view const id)
    { return std::ranges::find(*projectedRes, id, &CatalogMessage::id); };

    auto const positional = findMessage("winui_error");
    REQUIRE(positional != projectedRes->end());
    CHECK(positional->pattern == "Fehler: {0}");

    auto const alias = findMessage("winui_track_move_column_left_button.Text");
    REQUIRE(alias != projectedRes->end());
    CHECK(alias->pattern == "Nach links");

    CHECK(findMessage("winui_save_settings_failed") == projectedRes->end());
    CHECK(findMessage("winui_track_move_column_right_button.Text") == projectedRes->end());

    auto completeProjectionRes = projectWinUiResources(translated, MissingWinUiMessagePolicy::Reject);
    REQUIRE_FALSE(completeProjectionRes);
    CHECK(completeProjectionRes.error().message.contains("references unknown message id"));
  }

  TEST_CASE("CatalogPattern - native resource output is stable, sorted, and escaped", "[core][unit][catalog]")
  {
    auto const messages = std::array{
      CatalogMessage{.id = "z_last", .pattern = "A & B"},
      CatalogMessage{.id = "a_first", .pattern = "Use <value> and \"quotes\""},
    };

    auto const first = renderResw(messages);
    auto const second = renderResw(messages);
    CHECK(first == second);
    CHECK(first.find("a_first") < first.find("z_last"));
    CHECK(first.contains("Use &lt;value&gt; and &quot;quotes&quot;"));
    CHECK(first.contains("A &amp; B"));

    auto const resource = renderIcuResource("qps_Ploc", messages);
    CHECK(resource.find("a_first") < resource.find("z_last"));
    CHECK(resource.contains("\\\"quotes\\\""));
  }
} // namespace ao::i18n::detail::test
