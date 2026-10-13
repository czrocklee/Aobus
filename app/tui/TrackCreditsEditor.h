// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "CompletionPopup.h"
#include "TextFieldModel.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>
#include <ao/rt/completion/CompletionResult.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace ftxui
{
  struct Mouse;
} // namespace ftxui

namespace ao::uimodel
{
  class TrackCreditsEditorModel;
} // namespace ao::uimodel

namespace ao::tui
{
  /// Terminal controls only: the Properties form owns the child draft and its scope.
  class TrackCreditsEditor final
  {
  public:
    using CompletionProvider =
      std::function<std::optional<rt::CompletionResult>(library::CreditKind, bool, std::string_view, std::size_t)>;
    enum class Request : std::uint8_t
    {
      None,
      Accept,
      Cancel
    };

    explicit TrackCreditsEditor(i18n::MessageCatalog textCatalog, CompletionProvider completionProvider = {});
    void reset(uimodel::TrackCreditsEditorModel& model);
    Request handleEvent(ftxui::Event const& event, uimodel::TrackCreditsEditorModel& model);
    ftxui::Element render(uimodel::TrackCreditsEditorModel const& model) const;

  private:
    enum class Control : std::uint8_t
    {
      Name,
      Role,
      Kind,
      Add,
      Delete,
      MoveUp,
      MoveDown,
      Replace,
      Clear,
      Commit,
      Cancel,
      Count
    };
    void selectRow(uimodel::TrackCreditsEditorModel& model, std::size_t index);
    void complete(uimodel::TrackCreditsEditorModel const& model);
    Request handleMouse(ftxui::Mouse const& mouse, uimodel::TrackCreditsEditorModel& model);
    bool tryHandleCompletionEvent(ftxui::Event const& event, uimodel::TrackCreditsEditorModel& model);
    Request activate(uimodel::TrackCreditsEditorModel& model);
    library::CreditKind selectedKind(uimodel::TrackCreditsEditorModel const& model) const;

    i18n::MessageCatalog _textCatalog;
    CompletionProvider _completionProvider;
    Control _control = Control::Name;
    std::size_t _rowIndex = 0;
    TextFieldModel _name;
    TextFieldModel _role;
    std::optional<rt::CompletionResult> _optCompletion;
    CompletionPopupSelection _completionSelection;
    mutable std::vector<ftxui::Box> _candidateBoxes;
  };
} // namespace ao::tui
