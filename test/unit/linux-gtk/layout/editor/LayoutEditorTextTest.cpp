// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "layout/editor/LayoutEditorText.h"

#include "test/unit/PresentationTextCatalogTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::layout::editor::test
{
  TEST_CASE("LayoutEditorText - localizes built-in vocabulary and preserves extensions", "[gtk][unit][localization]")
  {
    auto const german = ao::test::presentationTextCatalog("de-DE");
    CHECK(layoutEditorVocabularyText(german, "Split Pane") == "Geteilte Ansicht");
    CHECK(layoutEditorVocabularyText(german, "Initial Position (%)") == "Anfangsposition (%)");
    CHECK(layoutEditorVocabularyText(german, "horizontal") == "Horizontal");
    CHECK(layoutEditorVocabularyText(german, "jumpToAlbum") == "Zum Album springen");
    CHECK(layoutEditorVocabularyText(german, "") == "Keine");
    CHECK(layoutEditorVocabularyText(german, "Third-party Control") == "Third-party Control");

    auto const pseudo = ao::test::presentationTextCatalog("qps-ploc");
    auto const expanded = layoutEditorVocabularyText(pseudo, "Split Pane");
    CHECK(expanded.starts_with("[!! "));
    CHECK(expanded.ends_with(" !!]"));
  }
} // namespace ao::gtk::layout::editor::test
