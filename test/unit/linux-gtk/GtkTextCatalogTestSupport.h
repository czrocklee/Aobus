// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "i18n/GtkTextCatalog.h"

#include <string_view>

namespace ao::gtk::test
{
  GtkTextCatalog const& englishGtkTextCatalog();
  GtkTextCatalog gtkTextCatalog(std::string_view locale);
} // namespace ao::gtk::test
