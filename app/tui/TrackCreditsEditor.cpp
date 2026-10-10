// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TrackCreditsEditor.h"

#include "CompletionPopup.h"
#include "MouseBindings.h"
#include "Style.h"
#include "TextField.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>
#include <ao/uimodel/library/detail/TrackCredits.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    // Keep the input's internal xframe caret, but publish only the selected
    // candidate to the enclosing yframe. focusPosition alone loses that point
    // when an intervening vbox aggregates its disabled outgoing focus.
    class CompletionFocusNode final : public ftxui::Node
    {
    public:
      CompletionFocusNode(ftxui::Element elementPtr, std::optional<std::int32_t> const optCandidateLine)
        : Node{{std::move(elementPtr)}}, _optCandidateLine{optCandidateLine}
      {
      }

      void ComputeRequirement() override
      {
        Node::ComputeRequirement();
        requirement_ = children_.front()->requirement();
        requirement_.focused = {};

        if (_optCandidateLine)
        {
          requirement_.focused.enabled = true;
          requirement_.focused.node = this;
          requirement_.focused.box = {.x_min = 0, .x_max = 0, .y_min = *_optCandidateLine, .y_max = *_optCandidateLine};
        }
      }

      void SetBox(ftxui::Box box) override
      {
        Node::SetBox(box);
        children_.front()->SetBox(box);
      }

    private:
      std::optional<std::int32_t> _optCandidateLine;
    };
  } // namespace

  TrackCreditsEditor::TrackCreditsEditor(i18n::MessageCatalog textCatalog, CompletionProvider completionProvider)
    : _textCatalog{std::move(textCatalog)}, _completionProvider{std::move(completionProvider)}
  {
  }

  void TrackCreditsEditor::reset(uimodel::TrackCreditsEditorModel& model)
  {
    _control = model.canEdit() ? Control::Name : Control::Replace;
    selectRow(model, 0);
  }

  void TrackCreditsEditor::selectRow(uimodel::TrackCreditsEditorModel& model, std::size_t const index)
  {
    _optCompletion.reset();
    _candidateBoxes.clear();
    _rowIndex = model.entries().empty() ? 0 : std::min(index, model.entries().size() - 1);
    model.focusRow(model.entries().empty() ? std::nullopt : std::optional<std::size_t>{_rowIndex});
    _name.reset(model.entries().empty() ? "" : model.entries()[_rowIndex].name);
    _role.reset(model.entries().empty() ? "" : model.entries()[_rowIndex].role);
  }

  library::CreditKind TrackCreditsEditor::selectedKind(uimodel::TrackCreditsEditorModel const& model) const
  {
    if (_rowIndex < model.entries().size())
    {
      return model.entries()[_rowIndex].kind;
    }

    if (model.scope().all())
    {
      return library::CreditKind::Performer;
    }

    for (std::size_t index = 0; index < library::kCreditKindCount; ++index)
    {
      if (model.scope().test(index))
      {
        return static_cast<library::CreditKind>(index);
      }
    }

    return library::CreditKind::Performer;
  }

  void TrackCreditsEditor::complete(uimodel::TrackCreditsEditorModel const& model)
  {
    if (!_completionProvider || !model.canEdit() || _rowIndex >= model.entries().size() ||
        (_control != Control::Name && _control != Control::Role))
    {
      return;
    }

    auto const& input = _control == Control::Name ? _name : _role;
    _optCompletion = _completionProvider(selectedKind(model), _control == Control::Role, input.value(), input.cursor());

    if (_optCompletion && _optCompletion->items.empty())
    {
      _optCompletion.reset();
    }

    _completionSelection.reset();
  }

  TrackCreditsEditor::Request TrackCreditsEditor::handleMouse(ftxui::Mouse const& mouse,
                                                              uimodel::TrackCreditsEditorModel& model)
  {
    if (_optCompletion && isLeftPress(mouse))
    {
      if (auto const optRow = mouseRowAt(_candidateBoxes, mouse); optRow)
      {
        _completionSelection.select(*optRow);
        return handleEvent(ftxui::Event::Return, model);
      }
    }

    return Request::None;
  }

  bool TrackCreditsEditor::tryHandleCompletionEvent(ftxui::Event const& event, uimodel::TrackCreditsEditorModel& model)
  {
    if (!_optCompletion)
    {
      return false;
    }

    if (_completionSelection.tryNavigate(event, _optCompletion->items.size()))
    {
      return true;
    }

    if (event == ftxui::Event::Escape)
    {
      _optCompletion.reset();
      return true;
    }

    if (event == ftxui::Event::Return)
    {
      auto& input = _control == Control::Name ? _name : _role;
      auto const& item = _optCompletion->items[_completionSelection.selectedCandidate()];

      if (input.tryReplaceRange(_optCompletion->replaceBegin, _optCompletion->replaceEnd, item.insertText))
      {
        if (_control == Control::Name)
        {
          model.updateName(_rowIndex, input.value());
        }
        else
        {
          model.updateRole(_rowIndex, input.value());
        }
      }

      _optCompletion.reset();
      return true;
    }

    _optCompletion.reset();
    return false;
  }

  TrackCreditsEditor::Request TrackCreditsEditor::handleEvent(ftxui::Event const& event,
                                                              uimodel::TrackCreditsEditorModel& model)
  {
    using ftxui::Event;

    if (event.is_mouse())
    {
      auto mouseEvent = event;
      return handleMouse(mouseEvent.mouse(), model);
    }

    _candidateBoxes.clear();

    if (tryHandleCompletionEvent(event, model))
    {
      return Request::None;
    }

    if (event == Event::Escape)
    {
      return Request::Cancel;
    }

    if (event == Event::CtrlS)
    {
      _control = Control::Commit;
      return activate(model);
    }

    if (event == Event::Tab || event == Event::TabReverse)
    {
      auto const count = static_cast<std::int32_t>(Control::Count);
      _control =
        static_cast<Control>((static_cast<std::int32_t>(_control) + (event == Event::Tab ? 1 : count - 1)) % count);
      return Request::None;
    }

    if (event == Event::ArrowUp || event == Event::ArrowDown)
    {
      auto const previousRowIndex = _rowIndex == 0 ? 0 : _rowIndex - 1;
      selectRow(model, event == Event::ArrowUp ? previousRowIndex : _rowIndex + 1);
      return Request::None;
    }

    if (event == Event::Return)
    {
      return activate(model);
    }

    if (event == Event::CtrlN)
    {
      complete(model);
      return Request::None;
    }

    if (model.canEdit() && _rowIndex < model.entries().size())
    {
      if (_control == Control::Name && _name.tryApplyEvent(event))
      {
        model.updateName(_rowIndex, _name.value());
      }
      else if (_control == Control::Role && _role.tryApplyEvent(event))
      {
        model.updateRole(_rowIndex, _role.value());
      }
    }

    return Request::None;
  }

  TrackCreditsEditor::Request TrackCreditsEditor::activate(uimodel::TrackCreditsEditorModel& model)
  {
    switch (_control)
    {
      case Control::Name:
      case Control::Role: break;
      case Control::Kind:
        if (model.scope().all())
        {
          auto const kind = static_cast<library::CreditKind>((static_cast<std::size_t>(selectedKind(model)) + 1) %
                                                             library::kCreditKindCount);
          model.changeKind(_rowIndex, kind);
        }

        break;
      case Control::Add:
        model.addEntry(selectedKind(model));
        _control = Control::Name;
        break;
      case Control::Delete: model.deleteEntry(_rowIndex); break;
      case Control::MoveUp:
        if (_rowIndex > 0)
        {
          model.moveEntry(_rowIndex, _rowIndex - 1);
        }

        break;
      case Control::MoveDown: model.moveEntry(_rowIndex, _rowIndex + 1); break;
      case Control::Replace:
        model.beginReplacement();
        _control = Control::Add;
        break;
      case Control::Clear: model.clearScope(); break;
      case Control::Commit:
        if (model.canCommit())
        {
          return Request::Accept;
        }

        if (auto const errors = model.validationErrors(); !errors.empty())
        {
          selectRow(model, errors.front().rowIndex);

          switch (errors.front().reason)
          {
            case uimodel::TrackCreditValidationReason::BlankName:
            case uimodel::TrackCreditValidationReason::InvalidNameText: _control = Control::Name; break;
            case uimodel::TrackCreditValidationReason::InvalidRoleText: _control = Control::Role; break;
            case uimodel::TrackCreditValidationReason::InvalidKind: _control = Control::Kind; break;
          }
        }

        return Request::None;
      case Control::Cancel: return Request::Cancel;
      case Control::Count: break;
    }

    selectRow(model, model.focusedRow().value_or(0));
    return Request::None;
  }

  ftxui::Element TrackCreditsEditor::render(uimodel::TrackCreditsEditorModel const& model) const
  {
    using namespace ftxui;
    using i18n::MessageId;
    auto const label = [&](MessageId const id) { return std::string{i18n::requiredText(_textCatalog, id)}; };
    auto const scope = uimodel::trackCreditScopeLabel(_textCatalog, model.scope());
    auto rows = Elements{};
    _candidateBoxes.assign(_optCompletion ? _optCompletion->items.size() : 0, kEmptyMouseBox);
    auto append = [&](Control const control, Element elementPtr)
    {
      auto rowPtr = hbox({text(_control == control ? "> " : "  "), std::move(elementPtr) | flex});

      if (_control == control && _optCompletion)
      {
        auto const candidateLine =
          1 + static_cast<std::int32_t>(_completionSelection.selectedCandidate() - _completionSelection.windowStart());
        rowPtr = vbox(
          {std::make_shared<CompletionFocusNode>(std::move(rowPtr), std::nullopt),
           std::make_shared<CompletionFocusNode>(
             renderCompletionPopup(_optCompletion->items, _completionSelection, 2, _candidateBoxes), candidateLine)});
      }
      else if (_control == control)
      {
        rowPtr = std::move(rowPtr) | focus;
      }

      rows.push_back(std::move(rowPtr));
    };
    append(Control::Name,
           vbox({paragraph(label(MessageId::TrackCreditName)),
                 textFieldValue(_name, nullptr, _control == Control::Name && model.canEdit())}));
    append(Control::Role,
           vbox({paragraph(label(MessageId::TrackCreditRole)),
                 textFieldValue(_role, nullptr, _control == Control::Role && model.canEdit())}));
    append(Control::Kind,
           paragraph(label(MessageId::TrackCreditKind) + ": " +
                     std::string{uimodel::trackCreditKindLabel(_textCatalog, selectedKind(model))}) |
             (model.scope().all() ? nothing : dim));
    constexpr auto kActions =
      std::to_array<std::pair<Control, MessageId>>({{Control::Add, MessageId::TrackCreditsAdd},
                                                    {Control::Delete, MessageId::TrackCreditDelete},
                                                    {Control::MoveUp, MessageId::TrackCreditMoveUp},
                                                    {Control::MoveDown, MessageId::TrackCreditMoveDown},
                                                    {Control::Replace, MessageId::TrackCreditsReplaceScope},
                                                    {Control::Clear, MessageId::TrackCreditsClear},
                                                    {Control::Commit, MessageId::TrackCreditsCommit},
                                                    {Control::Cancel, MessageId::TrackCreditsCancel}});

    for (auto const& [control, id] : kActions)
    {
      append(control, paragraph(label(id)) | (control == Control::Commit && !model.canCommit() ? dim : nothing));
    }

    auto bodyPtr = vbox(std::move(rows)) | vscroll_indicator | yframe | flex;
    auto header =
      Elements{paragraph(i18n::requiredFormat(_textCatalog, MessageId::TrackCreditsScope, {{"scope", scope}})) | bold,
               text(std::format("{} / {}", model.entries().empty() ? 0 : _rowIndex + 1, model.entries().size()))};

    if (!model.canEdit())
    {
      header.push_back(paragraph(label(MessageId::TrackMultipleValues)) | style::warning());
    }

    for (auto const& error : model.validationErrors())
    {
      if (error.rowIndex == _rowIndex)
      {
        header.push_back(paragraph(uimodel::formatTrackCreditValidationError(_textCatalog, error)) | style::danger());
      }
    }

    header.push_back(std::move(bodyPtr));
    header.push_back(paragraph("Tab / Shift-Tab · ↑ / ↓ · Enter · Ctrl-N · Ctrl-S · Esc") | dim);
    return vbox(std::move(header)) | flex;
  }
} // namespace ao::tui
