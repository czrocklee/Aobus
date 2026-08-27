// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>

#include <string>
#include <string_view>

namespace ao::gtk::layout::editor
{
  /** Resolves built-in descriptor vocabulary while preserving unknown extension text. */
  std::string layoutEditorVocabularyText(i18n::MessageCatalog const& textCatalog, std::string_view sourceText);
} // namespace ao::gtk::layout::editor
