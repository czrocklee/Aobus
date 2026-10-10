// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "track/TrackCreditsEditorControl.h"

#include <ao/rt/completion/CompletionService.h>
#include <ao/winui/track/TrackPropertiesAdapter.h>

#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <tuple>
#include <utility>

namespace ao::winui
{
  namespace
  {
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;
    using i18n::MessageId;
    using winrt::Windows::Foundation::IInspectable;
    constexpr auto kKinds = std::array{library::CreditKind::Conductor,
                                       library::CreditKind::Ensemble,
                                       library::CreditKind::Soloist,
                                       library::CreditKind::Performer};
    constexpr double kSpacing = 8.0;

    bool isFocusedWithin(AutoSuggestBox const& input)
    {
      auto const root = input.XamlRoot();

      if (!root)
      {
        return false;
      }

      auto focused = Input::FocusManager::GetFocusedElement(root).try_as<DependencyObject>();

      while (focused)
      {
        if (focused == input)
        {
          return true;
        }

        focused = Media::VisualTreeHelper::GetParent(focused);
      }

      return false;
    }

    void populateKinds(ComboBox const& input, i18n::MessageCatalog const& catalog)
    {
      input.Header(winrt::box_value(winrt::to_hstring(i18n::requiredText(catalog, MessageId::TrackCreditKind))));

      for (auto kind : kKinds)
      {
        input.Items().Append(winrt::box_value(winrt::to_hstring(uimodel::trackCreditKindLabel(catalog, kind))));
      }
    }
  }

  TrackCreditsEditorControl::TrackCreditsEditorControl(uimodel::TrackCreditsEditorModel& model,
                                                       rt::CompletionService& completion,
                                                       i18n::MessageCatalog textCatalog,
                                                       std::function<void()> accept,
                                                       std::function<void()> cancel)
    : _model{model}
    , _completion{completion}
    , _textCatalog{std::move(textCatalog)}
    , _accept{std::move(accept)}
    , _cancel{std::move(cancel)}
  {
    _root.Spacing(kSpacing);
    _rows.Spacing(kSpacing);
    _scope.TextWrapping(TextWrapping::Wrap);
    _root.Children().Append(_scope);
    _mixed.Text(winrt::to_hstring(i18n::requiredText(_textCatalog, MessageId::TrackMultipleValues)));
    _mixed.TextWrapping(TextWrapping::Wrap);
    _root.Children().Append(_mixed);
    _replace = makeButton(MessageId::TrackCreditsReplaceScope,
                          [this]
                          {
                            _model.beginReplacement();
                            refresh();
                          });
    _root.Children().Append(_replace);
    _clear = makeButton(MessageId::TrackCreditsClear,
                        [this]
                        {
                          _model.clearScope();
                          refresh();
                        });
    _root.Children().Append(_clear);
    _root.Children().Append(_rows);
    populateKinds(_addKind, _textCatalog);
    _addKind.SelectedIndex(0);
    _root.Children().Append(_addKind);
    _add = makeButton(MessageId::TrackCreditsAdd,
                      [this]
                      {
                        if (!_model.canEdit())
                        {
                          return;
                        }

                        auto const selected = _addKind.SelectedIndex();

                        if (selected < 0 || selected >= static_cast<std::int32_t>(kKinds.size()))
                        {
                          return;
                        }

                        auto kind = kKinds[static_cast<std::size_t>(selected)];

                        if (_model.scope().count() == 1)
                        {
                          for (auto candidate : kKinds)
                          {
                            if (_model.scope().test(static_cast<std::size_t>(candidate)))
                            {
                              kind = candidate;
                            }
                          }
                        }

                        _model.addEntry(kind);
                        refresh();
                        focusDraftRow();
                      });
    _root.Children().Append(_add);
    _save = makeButton(MessageId::TrackCreditsCommit,
                       [this]
                       {
                         if (_model.canCommit())
                         {
                           _accept();
                         }
                         else
                         {
                           updateValidation();

                           if (auto const errors = _model.validationErrors(); !errors.empty())
                           {
                             _model.focusRow(errors.front().rowIndex);
                           }

                           focusDraftRow();
                         }
                       });
    _root.Children().Append(_save);
    _cancelButton = makeButton(MessageId::TrackCreditsCancel, [this] { _cancel(); });
    _root.Children().Append(_cancelButton);
    _keyRevoker =
      _root.KeyDown(winrt::auto_revoke,
                    [this](IInspectable const&, Input::KeyRoutedEventArgs const& args)
                    {
                      if (_enabled && _model.isEditing() && args.Key() == winrt::Windows::System::VirtualKey::Escape)
                      {
                        args.Handled(true);
                        _cancel();
                      }
                    });
    _loadedRevoker =
      _root.Loaded(winrt::auto_revoke, [this](IInspectable const&, RoutedEventArgs const&) { focusDraftRow(); });

    if (!_model.entries().empty())
    {
      _model.focusRow(0);
    }

    refresh();
  }

  TrackCreditsEditorControl::~TrackCreditsEditorControl() = default;

  void TrackCreditsEditorControl::refresh()
  {
    auto const scopeText = uimodel::trackCreditScopeLabel(_textCatalog, _model.scope());

    _scope.Text(
      winrt::to_hstring(i18n::requiredFormat(_textCatalog, MessageId::TrackCreditsScope, {{"scope", scopeText}})));
    _replace.Visibility(_model.canEdit() ? Visibility::Collapsed : Visibility::Visible);
    _mixed.Visibility(_model.isMixedReplacement() && !_model.canEdit() ? Visibility::Visible : Visibility::Collapsed);
    _addKind.Visibility(_model.scope().all() ? Visibility::Visible : Visibility::Collapsed);
    rebuildRows();
    updateEnabled();
    updateValidation();
  }

  void TrackCreditsEditorControl::setEnabled(bool const enabled)
  {
    _enabled = enabled;
    updateEnabled();

    if (!enabled)
    {
      for (auto const& name : _names)
      {
        name.IsSuggestionListOpen(false);
      }

      for (auto const& role : _roles)
      {
        role.IsSuggestionListOpen(false);
      }
    }
  }

  void TrackCreditsEditorControl::rebuildRows()
  {
    _building = true;
    _textRevokers.clear();
    _kindRevokers.clear();
    _rowButtonRevokers.clear();
    _rowLoadedRevokers.clear();
    _names.clear();
    _roles.clear();
    _rowControls.clear();
    _errors.clear();
    _rows.Children().Clear();

    for (std::size_t index = 0; index < _model.entries().size(); ++index)
    {
      appendRow(index);
    }

    _building = false;
  }

  void TrackCreditsEditorControl::updateEnabled()
  {
    auto const editing = _enabled && _model.isEditing();
    auto const canEdit = _enabled && _model.canEdit();
    _replace.IsEnabled(editing && !_model.canEdit());
    _clear.IsEnabled(editing);
    _cancelButton.IsEnabled(editing);
    _addKind.IsEnabled(canEdit);
    _add.IsEnabled(canEdit);
    _save.IsEnabled(_enabled && _model.canCommit());

    for (std::size_t index = 0; index < _rowControls.size(); ++index)
    {
      _names[index].IsEnabled(canEdit);
      _roles[index].IsEnabled(canEdit);
      auto const& row = _rowControls[index];

      if (row.kind)
      {
        row.kind.IsEnabled(canEdit);
      }

      auto const& entries = _model.entries();
      auto const hasEntry = index < entries.size();
      row.moveUp.IsEnabled(canEdit && hasEntry && index > 0 && entries[index - 1].kind == entries[index].kind);
      row.moveDown.IsEnabled(canEdit && hasEntry && index + 1 < entries.size() &&
                             entries[index + 1].kind == entries[index].kind);
      row.remove.IsEnabled(canEdit && hasEntry);
    }
  }

  void TrackCreditsEditorControl::updateValidation()
  {
    for (std::size_t index = 0; index < _rowControls.size(); ++index)
    {
      Automation::AutomationProperties::SetHelpText(_names[index], L"");
      Automation::AutomationProperties::SetHelpText(_roles[index], L"");

      if (_rowControls[index].kind)
      {
        Automation::AutomationProperties::SetHelpText(_rowControls[index].kind, L"");
      }
    }

    for (auto const& error : _errors)
    {
      error.Text(L"");
      error.Visibility(Visibility::Collapsed);
    }

    for (auto const& error : _model.validationErrors())
    {
      if (error.rowIndex < _errors.size())
      {
        auto const message = winrt::to_hstring(uimodel::formatTrackCreditValidationError(_textCatalog, error));
        _errors[error.rowIndex].Text(message);
        _errors[error.rowIndex].Visibility(Visibility::Visible);

        if (auto const input = validationControl(error); input)
        {
          Automation::AutomationProperties::SetHelpText(input, message);
        }
      }
    }

    _save.IsEnabled(_enabled && _model.canCommit());
  }

  Control TrackCreditsEditorControl::validationControl(uimodel::TrackCreditValidationError const& error) const
  {
    if (error.rowIndex >= _rowControls.size())
    {
      return nullptr;
    }

    switch (error.reason)
    {
      case uimodel::TrackCreditValidationReason::BlankName:
      case uimodel::TrackCreditValidationReason::InvalidNameText: return _names[error.rowIndex];
      case uimodel::TrackCreditValidationReason::InvalidRoleText: return _roles[error.rowIndex];
      case uimodel::TrackCreditValidationReason::InvalidKind: return _rowControls[error.rowIndex].kind;
    }

    return nullptr;
  }

  void TrackCreditsEditorControl::appendRow(std::size_t const index)
  {
    auto const entry = _model.entries()[index];
    auto panel = StackPanel{};
    panel.Spacing(kSpacing);
    auto name = AutoSuggestBox{};
    name.Header(winrt::box_value(winrt::to_hstring(i18n::requiredText(_textCatalog, MessageId::TrackCreditName))));
    name.Text(winrt::to_hstring(entry.name));
    name.UpdateTextOnSelect(true);
    _names.push_back(name);
    panel.Children().Append(name);
    auto role = AutoSuggestBox{};
    role.Header(winrt::box_value(winrt::to_hstring(i18n::requiredText(_textCatalog, MessageId::TrackCreditRole))));
    role.Text(winrt::to_hstring(entry.role));
    role.UpdateTextOnSelect(true);
    _roles.push_back(role);

    for (auto const& input : {name, role})
    {
      _rowLoadedRevokers.push_back(input.Loaded(winrt::auto_revoke,
                                                [this, index](IInspectable const& sender, RoutedEventArgs const&)
                                                {
                                                  if (!_building && index < _names.size() &&
                                                      (sender == _names[index] || sender == _roles[index]) &&
                                                      _model.focusedRow() == index)
                                                  {
                                                    focusDraftRow();
                                                  }
                                                }));
    }

    panel.Children().Append(role);
    _textRevokers.push_back(
      name.TextChanged(winrt::auto_revoke,
                       [this, index](AutoSuggestBox const& sender, AutoSuggestBoxTextChangedEventArgs const& args)
                       {
                         if (_building || !_enabled || !_model.canEdit() || index >= _names.size() ||
                             index >= _model.entries().size() || sender != _names[index])
                         {
                           return;
                         }

                         auto text = winrt::to_string(sender.Text());

                         // Deferred hydration must not claim a row merely by replaying its current value.
                         // Genuine property and suggestion edits still update the draft, not only UserInput.
                         if (text != _model.entries()[index].name)
                         {
                           _model.updateName(index, std::move(text));
                           updateValidation();
                         }

                         if (args.Reason() == AutoSuggestionBoxTextChangeReason::UserInput && isFocusedWithin(sender))
                         {
                           _model.focusRow(index);
                           suggest(sender, index, false);
                         }
                       }));
    _textRevokers.push_back(
      role.TextChanged(winrt::auto_revoke,
                       [this, index](AutoSuggestBox const& sender, AutoSuggestBoxTextChangedEventArgs const& args)
                       {
                         if (_building || !_enabled || !_model.canEdit() || index >= _roles.size() ||
                             index >= _model.entries().size() || sender != _roles[index])
                         {
                           return;
                         }

                         auto text = winrt::to_string(sender.Text());

                         if (text != _model.entries()[index].role)
                         {
                           _model.updateRole(index, std::move(text));
                           updateValidation();
                         }

                         if (args.Reason() == AutoSuggestionBoxTextChangeReason::UserInput && isFocusedWithin(sender))
                         {
                           _model.focusRow(index);
                           suggest(sender, index, true);
                         }
                       }));

    auto rowControls = RowControls{};

    if (_model.scope().all())
    {
      auto kind = ComboBox{};
      rowControls.kind = kind;
      populateKinds(kind, _textCatalog);
      kind.SelectedIndex(static_cast<std::int32_t>(entry.kind));
      panel.Children().Append(kind);
      _kindRevokers.push_back(
        kind.SelectionChanged(winrt::auto_revoke,
                              [this, index](IInspectable const& sender, SelectionChangedEventArgs const&)
                              {
                                if (!_building && _enabled && _model.canEdit() && index < _rowControls.size() &&
                                    index < _model.entries().size() && sender == _rowControls[index].kind)
                                {
                                  auto const selected = sender.as<ComboBox>().SelectedIndex();

                                  if (selected >= 0 && selected < static_cast<std::int32_t>(kKinds.size()) &&
                                      kKinds[static_cast<std::size_t>(selected)] != _model.entries()[index].kind)
                                  {
                                    _model.changeKind(index, kKinds[static_cast<std::size_t>(selected)]);
                                    refresh();
                                    focusDraftRow();
                                  }
                                }
                              }));
    }

    // Vertical commands remain reachable at narrow dialog widths and by Tab/Space.
    auto appendCommand = [this, &panel](MessageId const id, std::function<void()> action)
    {
      auto button = makeButton(id, std::move(action));
      _rowButtonRevokers.push_back(std::move(_buttonRevokers.back()));
      _buttonRevokers.pop_back();
      panel.Children().Append(button);
      return button;
    };
    rowControls.moveUp = appendCommand(MessageId::TrackCreditMoveUp,
                                       [this, index]
                                       {
                                         _model.moveEntry(index, index - 1);
                                         refresh();
                                         focusDraftRow();
                                       });
    rowControls.moveDown = appendCommand(MessageId::TrackCreditMoveDown,
                                         [this, index]
                                         {
                                           _model.moveEntry(index, index + 1);
                                           refresh();
                                           focusDraftRow();
                                         });
    rowControls.remove = appendCommand(MessageId::TrackCreditDelete,
                                       [this, index]
                                       {
                                         _model.deleteEntry(index);
                                         refresh();
                                         focusDraftRow();
                                       });
    _rowControls.push_back(std::move(rowControls));
    auto error = TextBlock{};
    error.TextWrapping(TextWrapping::Wrap);
    Automation::AutomationProperties::SetLiveSetting(error, Automation::Peers::AutomationLiveSetting::Polite);
    panel.Children().Append(error);
    _errors.push_back(error);
    _rows.Children().Append(panel);
  }

  void TrackCreditsEditorControl::focusDraftRow()
  {
    if (!_enabled || !_model.isEditing())
    {
      return;
    }

    auto const optIndex = _model.focusedRow();

    if (optIndex && *optIndex < _names.size())
    {
      for (auto const& error : _model.validationErrors())
      {
        if (error.rowIndex == *optIndex)
        {
          if (auto const input = validationControl(error); input)
          {
            std::ignore = input.Focus(FocusState::Programmatic);
            return;
          }
        }
      }

      std::ignore = _names[*optIndex].Focus(FocusState::Programmatic);
    }
    else if (_model.isEditing())
    {
      std::ignore = (_model.canEdit() ? _add : _replace).Focus(FocusState::Programmatic);
    }
  }

  void TrackCreditsEditorControl::suggest(AutoSuggestBox const& input, std::size_t const index, bool const role)
  {
    auto const vocabulary = role ? _completion.creditRoles() : _completion.creditNames(_model.entries()[index].kind);
    auto const suggestions = trackPropertyVocabularySuggestions(vocabulary, winrt::to_string(input.Text()), 12);
    auto items = winrt::single_threaded_observable_vector<IInspectable>();

    for (auto const& suggestion : suggestions)
    {
      items.Append(winrt::box_value(winrt::to_hstring(suggestion)));
    }

    input.ItemsSource(items);
    input.IsSuggestionListOpen(!suggestions.empty());
  }

  Button TrackCreditsEditorControl::makeButton(MessageId const id, std::function<void()> action)
  {
    auto button = Button{};
    auto text = TextBlock{};
    text.Text(winrt::to_hstring(i18n::requiredText(_textCatalog, id)));
    text.TextWrapping(TextWrapping::Wrap);
    button.Content(text);
    button.HorizontalAlignment(HorizontalAlignment::Stretch);
    _buttonRevokers.push_back(button.Click(
      winrt::auto_revoke,
      [this, action = std::move(action)](IInspectable const& sender, RoutedEventArgs const&)
      {
        auto const source = sender.as<Button>();
        auto const isCurrent =
          source == _replace || source == _clear || source == _add || source == _save || source == _cancelButton ||
          std::ranges::any_of(_rowControls,
                              [&source](auto const& row)
                              { return source == row.moveUp || source == row.moveDown || source == row.remove; });

        if (_enabled && _model.isEditing() && isCurrent && source.IsEnabled())
        {
          // A structural action revokes row handlers. Retain the callable independently of
          // the native delegate while it runs; the C++ owner must survive this callback.
          auto retainedAction = action;
          retainedAction();
        }
      }));
    return button;
  }
} // namespace ao::winui
