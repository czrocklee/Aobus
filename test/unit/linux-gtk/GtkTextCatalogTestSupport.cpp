// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/linux-gtk/GtkTextCatalogTestSupport.h"

#include "i18n/GtkTextCatalog.h"
#include "test/unit/PresentationTextCatalogTestSupport.h"

#include <string_view>

namespace ao::gtk::test
{
  GtkTextCatalog const& englishGtkTextCatalog()
  {
    static auto const catalog = GtkTextCatalog{ao::test::englishMessageCatalog()};
    return catalog;
  }

  GtkTextCatalog gtkTextCatalog(std::string_view const locale)
  {
    auto catalog = ao::test::messageCatalog(locale);
    return GtkTextCatalog{catalog};
  }
} // namespace ao::gtk::test
