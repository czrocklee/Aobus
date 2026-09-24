// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "i18n/GtkText.h"

#include "test/unit/MessageCatalogTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  TEST_CASE("GtkText - formats localized list questions with supplied content", "[gtk][unit][gtk-text][localization]")
  {
    auto const german = ao::test::messageCatalog("de-DE");
    CHECK(removeFromCurrentList(german, "Straße", "#straße") == "Aus Straße (#straße) entfernen");
    CHECK(deleteListQuestion(german, "Sommer").starts_with("\"Sommer\" löschen?"));

    auto const pseudo = ao::test::messageCatalog("qps-ploc");
    CHECK(deleteSubtreeQuestion(pseudo, 2, "• A\n• B\n").contains("• A\n• B\n"));
  }
} // namespace ao::gtk::test
