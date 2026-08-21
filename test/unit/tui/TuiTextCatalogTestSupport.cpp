// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/tui/TuiTextCatalogTestSupport.h"

#include "test/unit/PresentationTextCatalogTestSupport.h"
#include "tui/TuiTextCatalog.h"

#include <string_view>

namespace ao::tui::test
{
  TuiTextCatalog const& englishTuiTextCatalog()
  {
    static auto const catalog = TuiTextCatalog{ao::test::englishMessageCatalog()};
    return catalog;
  }

  TuiTextCatalog tuiTextCatalog(std::string_view const locale)
  {
    auto catalog = ao::test::messageCatalog(locale);
    return TuiTextCatalog{catalog};
  }
} // namespace ao::tui::test
