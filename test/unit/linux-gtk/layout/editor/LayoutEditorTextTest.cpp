// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "layout/editor/LayoutEditorText.h"

#include "test/unit/MessageCatalogTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::layout::editor::test
{
  TEST_CASE("LayoutEditorText - localizes built-in vocabulary and preserves extensions", "[gtk][unit][localization]")
  {
    auto const german = ao::test::messageCatalog("de-DE");
    CHECK(layoutEditorVocabularyText(german, "Split Pane") == "Geteilte Ansicht");
    CHECK(layoutEditorVocabularyText(german, "Initial Position (%)") == "Anfangsposition (%)");
    CHECK(layoutEditorVocabularyText(german, "horizontal") == "Horizontal");
    CHECK(layoutEditorVocabularyText(german, "jumpToAlbum") == "Zum Album springen");
    CHECK(layoutEditorVocabularyText(german, "") == "Keine");
    CHECK(layoutEditorVocabularyText(german, "Third-party Control") == "Third-party Control");

    CHECK(layoutEditorVocabularyText(ao::test::messageCatalog("es-ES"), "Split Pane") == "Panel dividido");
    CHECK(layoutEditorVocabularyText(ao::test::messageCatalog("fr-FR"), "Split Pane") == "Panneau divisé");
    CHECK(layoutEditorVocabularyText(ao::test::messageCatalog("ja-JP"), "Split Pane") == "分割ペイン");
    CHECK(layoutEditorVocabularyText(ao::test::messageCatalog("zh-CN"), "Split Pane") == "分割面板");
    CHECK(layoutEditorVocabularyText(ao::test::messageCatalog("zh-TW"), "Split Pane") == "分割面板");

    auto const pseudo = ao::test::messageCatalog("qps-ploc");
    auto const expanded = layoutEditorVocabularyText(pseudo, "Split Pane");
    CHECK(expanded.starts_with("[!! "));
    CHECK(expanded.ends_with(" !!]"));
  }
} // namespace ao::gtk::layout::editor::test
