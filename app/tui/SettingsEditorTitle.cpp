// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "SettingsEditor.h"
#include "Style.h"
#include "TextField.h"
#include <ao/i18n/MessageCatalog.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <cstdint>
#include <utility>

namespace ao::tui
{
  void SettingsEditor::beginTitleEditing()
  {
    _editingTitle = true;
    _titleInput.reset(_preferences.terminalTitleFormat);
    _titleError.clear();
    tryRefreshTitlePreview();
  }

  bool SettingsEditor::tryRefreshTitlePreview()
  {
    if (!_active || !_editingTitle || !_titleError.empty())
    {
      return false;
    }

    auto previewRes = _outputs.previewTerminalTitle(_titleInput.value());

    if (!previewRes)
    {
      _titleError = i18n::requiredFormat(
        _textCatalog, i18n::MessageId::TuiSettingsInvalidTitleFormat, {{"detail", previewRes.error().message}});
      _titlePreview.clear();
      return true;
    }

    auto preview = previewRes->value_or(std::string{i18n::requiredText(_textCatalog, i18n::MessageId::TuiSettingsOff)});
    auto const changed = preview != _titlePreview;
    _titlePreview = std::move(preview);
    return changed;
  }

  void SettingsEditor::handleTitleEditing(ftxui::Event const& event)
  {
    if (event == ftxui::Event::Escape)
    {
      _editingTitle = false;
      _titlePreview.clear();
      _titleError.clear();
    }
    else if (event == ftxui::Event::Return)
    {
      if (_titleError.empty())
      {
        auto candidate = _preferences;
        candidate.terminalTitleFormat = _titleInput.value();
        _editingTitle = false;
        _titlePreview.clear();
        applyPreferences(std::move(candidate));
      }
    }
    else if (_titleInput.tryApplyEvent(event))
    {
      _titleError.clear();
      tryRefreshTitlePreview();
    }
  }

  ftxui::Element SettingsEditor::renderTitleEditing(std::int32_t const columns) const
  {
    using namespace ftxui;
    using i18n::MessageId;
    auto rows = Elements{
      text(std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsTerminalTitle)}) | style::accent() | bold,
      paragraph(std::string{i18n::requiredText(_textCatalog, MessageId::TuiSettingsTerminalTitleHint)}) | dim,
      separator(),
      textFieldValue(_titleInput, &_titleTextBox) | reflect(_titleInputBox),
      separator(),
    };

    if (!_titleError.empty())
    {
      rows.push_back(paragraph(_titleError) | style::danger());
    }
    else
    {
      rows.push_back(
        paragraph(i18n::requiredFormat(_textCatalog, MessageId::TuiSettingsTitlePreview, {{"title", _titlePreview}})));
    }

    rows.push_back(filler());
    return style::panelBody(vbox(std::move(rows)) | size(WIDTH, EQUAL, columns)) | flex;
  }
} // namespace ao::tui
