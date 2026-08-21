// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <string>
#include <string_view>

namespace ao::uimodel
{
  class PresentationTextCatalog;
}

namespace ao::gtk::layout::editor
{
  /** Resolves built-in descriptor vocabulary while preserving unknown extension text. */
  std::string layoutEditorVocabularyText(uimodel::PresentationTextCatalog const& textCatalog,
                                         std::string_view sourceText);
} // namespace ao::gtk::layout::editor
