// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <string_view>

namespace ao::test
{
  i18n::MessageCatalog messageCatalog(std::string_view locale);
  i18n::MessageCatalog const& englishMessageCatalog();
  uimodel::PresentationTextCatalog const& englishPresentationTextCatalog();
  uimodel::PresentationTextCatalog presentationTextCatalog(std::string_view locale);
} // namespace ao::test
