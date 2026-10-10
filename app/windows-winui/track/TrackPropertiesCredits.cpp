// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "track/TrackCreditsEditorControl.h"
#include "track/TrackPropertiesCoordinator.h"
#include <ao/uimodel/library/detail/TrackCredits.h>

#include <array>
#include <string_view>
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

    Button textButton(std::string_view text)
    {
      auto button = Button{};
      auto label = TextBlock{};
      label.Text(winrt::to_hstring(text));
      label.TextWrapping(TextWrapping::Wrap);
      button.Content(label);
      button.HorizontalAlignment(HorizontalAlignment::Stretch);
      return button;
    }
  }

  void TrackPropertiesCoordinator::buildCreditsSection(StackPanel const& content)
  {
    _creditsPreview = StackPanel{};
    _creditsPreview.Spacing(8.0);
    _creditsHost = StackPanel{};
    content.Children().Append(_creditsPreview);
    content.Children().Append(_creditsHost);
    refreshCreditsPreview();

    _discardClickRevokers.clear();
    _reloadButton = textButton(i18n::requiredText(_textCatalog, MessageId::TuiEditorHintReload));
    _reloadButton.Visibility(Visibility::Collapsed);
    _discardClickRevokers.push_back(_reloadButton.Click(
      winrt::auto_revoke, [this](IInspectable const&, RoutedEventArgs const&) { requestDiscard(true); }));
    content.Children().Append(_reloadButton);
    _discardPrompt = StackPanel{};
    _discardPrompt.Spacing(8.0);
    _discardPrompt.Visibility(Visibility::Collapsed);
    auto question = TextBlock{};
    question.Text(winrt::to_hstring(i18n::requiredText(_textCatalog, MessageId::AppKitDiscardQuestion)));
    question.TextWrapping(TextWrapping::Wrap);
    _discardPrompt.Children().Append(question);
    auto discard = textButton(i18n::requiredText(_textCatalog, MessageId::AppKitDiscardChanges));
    _discardClickRevokers.push_back(
      discard.Click(winrt::auto_revoke, [this](IInspectable const&, RoutedEventArgs const&) { confirmDiscard(); }));
    _discardPrompt.Children().Append(discard);
    auto cancel = textButton(i18n::requiredText(_textCatalog, MessageId::TrackCreditsCancel));
    _discardClickRevokers.push_back(cancel.Click(winrt::auto_revoke,
                                                 [this](IInspectable const&, RoutedEventArgs const&)
                                                 {
                                                   _discardPrompt.Visibility(Visibility::Collapsed);
                                                   updateEditorEnabled();
                                                   updateSaveEnabled();
                                                 }));
    _discardPrompt.Children().Append(cancel);
    content.Children().Append(_discardPrompt);
  }

  void TrackPropertiesCoordinator::refreshCreditsPreview()
  {
    _creditClickRevokers.clear();
    _creditsPreview.Children().Clear();
    auto appendScope = [this](std::string_view label, std::bitset<library::kCreditKindCount> scope)
    {
      auto button = textButton(i18n::requiredFormat(_textCatalog, MessageId::TrackCreditsScope, {{"scope", label}}));
      _creditClickRevokers.push_back(button.Click(
        winrt::auto_revoke, [this, scope](IInspectable const&, RoutedEventArgs const&) { beginCreditsEdit(scope); }));
      _creditsPreview.Children().Append(button);
    };
    appendScope(i18n::requiredText(_textCatalog, MessageId::TrackCreditsAllKinds), uimodel::allTrackCreditKinds());
    auto const sections = _formModel.creditSections();
    auto const rows = uimodel::formatTrackCreditDisplayRows(_textCatalog, sections);

    for (auto const kind : kKinds)
    {
      appendScope(uimodel::trackCreditKindLabel(_textCatalog, kind), uimodel::trackCreditScope(kind));

      for (auto const& row : rows)
      {
        if (row.kind != kind)
        {
          continue;
        }

        auto name = TextBlock{};
        name.Text(winrt::to_hstring(row.name));
        name.TextWrapping(TextWrapping::Wrap);
        _creditsPreview.Children().Append(name);

        if (!row.role.empty())
        {
          auto role = TextBlock{};
          role.Text(winrt::to_hstring(row.role));
          role.TextWrapping(TextWrapping::Wrap);
          _creditsPreview.Children().Append(role);
        }
      }
    }
  }

  void TrackPropertiesCoordinator::beginCreditsEdit(std::bitset<library::kCreditKindCount> const scope)
  {
    if (_sessionInvalid || !_optSession || !_optSession->isCurrent() ||
        _interactionState != InteractionState::Editing || _formModel.creditsEditor().isEditing())
    {
      return;
    }

    // Retire any hidden previous child's native callbacks before reusing the model.
    _creditsControlPtr.reset();
    _creditsHost.Children().Clear();

    if (auto beginRes = _formModel.beginCreditsEdit(scope); !beginRes)
    {
      setError(beginRes.error().message);
      return;
    }

    _creditsControlPtr = std::make_unique<TrackCreditsEditorControl>(
      _formModel.creditsEditor(),
      _completion,
      _textCatalog,
      [this] { finishCreditsEdit(true); },
      [this] { finishCreditsEdit(false); });
    _creditsHost.Children().Append(_creditsControlPtr->element());
    _creditsHost.Visibility(Visibility::Visible);
    updateEditorEnabled();
    updateSaveEnabled();
  }

  void TrackPropertiesCoordinator::finishCreditsEdit(bool const accept)
  {
    if (accept)
    {
      if (_sessionInvalid || _interactionState != InteractionState::Editing)
      {
        return;
      }

      if (auto acceptedRes = _formModel.acceptCreditsEdit(); !acceptedRes)
      {
        setError(acceptedRes.error().message);
        return;
      }
    }
    else
    {
      _formModel.cancelCreditsEdit();
    }

    // Keep the control alive until its native event callback has returned.
    _creditsHost.Visibility(Visibility::Collapsed);
    refreshCreditsPreview();
    updateEditorEnabled();
    updateSaveEnabled();
    std::ignore = _creditsPreview.Children().GetAt(0).as<Button>().Focus(FocusState::Programmatic);
  }

  void TrackPropertiesCoordinator::requestDiscard(bool const reload)
  {
    if (_interactionState != InteractionState::Editing)
    {
      return;
    }

    _reloadAfterDiscard = reload;
    _discardPrompt.Visibility(Visibility::Visible);
    _dialog.IsPrimaryButtonEnabled(false);
    std::ignore = _discardPrompt.Children().GetAt(1).as<Button>().Focus(FocusState::Programmatic);
  }

  void TrackPropertiesCoordinator::confirmDiscard()
  {
    if (_interactionState != InteractionState::Editing)
    {
      return;
    }

    if (!_reloadAfterDiscard)
    {
      setInteractionState(InteractionState::Closing);
      _dialog.Hide();
      return;
    }

    // Only confirmed reload replaces the old binding and the complete parent draft.
    _creditsControlPtr.reset();
    _formModel.cancelCreditsEdit();
    auto preparedRes = prepareSession();

    if (!preparedRes)
    {
      _sessionInvalid = true;
      setError(preparedRes.error().message);
      updateEditorEnabled();
      return;
    }

    // Preserve the already-presented ContentDialog rather than nesting ShowAsync.
    _building = true;
    _fieldEditors.clear();
    _customEditors.clear();
    auto content = StackPanel{};
    content.Spacing(12.0);
    _errorText = TextBlock{};
    _errorText.TextWrapping(TextWrapping::Wrap);
    _errorText.Visibility(Visibility::Collapsed);
    content.Children().Append(_errorText);
    buildCreditsSection(content);
    buildMetadataSection(content);
    buildTagsSection(content);
    buildCustomMetadataSection(content);
    buildTechnicalSection(content);
    auto scroll = ScrollViewer{};
    scroll.MaxHeight(640.0);
    scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    scroll.Content(content);
    _dialog.Content(scroll);
    _building = false;
    updateEditorEnabled();
    updateSaveEnabled();
  }
} // namespace ao::winui
