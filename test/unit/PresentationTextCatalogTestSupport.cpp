// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/PresentationTextCatalogTestSupport.h"

#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <stdexcept>
#include <string_view>
#include <utility>

namespace ao::test
{
  i18n::MessageCatalog messageCatalog(std::string_view const locale)
  {
    auto catalogRes = i18n::MessageCatalog::create(locale);

    if (!catalogRes)
    {
      throw std::runtime_error{catalogRes.error().message};
    }

    return std::move(*catalogRes);
  }

  i18n::MessageCatalog const& englishMessageCatalog()
  {
    static auto const catalog = messageCatalog("en");
    return catalog;
  }

  uimodel::PresentationTextCatalog const& englishPresentationTextCatalog()
  {
    static auto const catalog = uimodel::PresentationTextCatalog{englishMessageCatalog()};
    return catalog;
  }

  uimodel::PresentationTextCatalog presentationTextCatalog(std::string_view const locale)
  {
    auto catalog = messageCatalog(locale);
    return uimodel::PresentationTextCatalog{catalog};
  }
} // namespace ao::test
