// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/library/detail/TrackCredits.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <cstddef>
#include <functional>
#include <vector>

namespace ao::rt
{
  class CompletionService;
}

namespace ao::winui
{
  // Borrows the Properties draft and completion service until its owner retires.
  // Accept only stages the parent draft; this control never submits authoring work.
  // Dispatcher-confined. The owner keeps this facade alive until every native callback returns,
  // including accept/cancel; retaining a XAML sender does not retain this C++ owner.
  class TrackCreditsEditorControl final
  {
  public:
    TrackCreditsEditorControl(uimodel::TrackCreditsEditorModel& model,
                              rt::CompletionService& completion,
                              i18n::MessageCatalog textCatalog,
                              std::function<void()> accept,
                              std::function<void()> cancel);
    ~TrackCreditsEditorControl();
    TrackCreditsEditorControl(TrackCreditsEditorControl const&) = delete;
    TrackCreditsEditorControl& operator=(TrackCreditsEditorControl const&) = delete;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel element() const { return _root; }
    void refresh();
    void setEnabled(bool enabled);

  private:
    void rebuildRows();
    void updateEnabled();
    void updateValidation();
    winrt::Microsoft::UI::Xaml::Controls::Control validationControl(
      uimodel::TrackCreditValidationError const& error) const;
    void appendRow(std::size_t index);
    void focusDraftRow();
    void suggest(winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const& input, std::size_t index, bool role);
    winrt::Microsoft::UI::Xaml::Controls::Button makeButton(i18n::MessageId id, std::function<void()> action);

    struct RowControls final
    {
      winrt::Microsoft::UI::Xaml::Controls::ComboBox kind{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::Button moveUp{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::Button moveDown{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::Button remove{nullptr};
    };

    uimodel::TrackCreditsEditorModel& _model;
    rt::CompletionService& _completion;
    i18n::MessageCatalog _textCatalog;
    std::function<void()> _accept;
    std::function<void()> _cancel;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel _root{};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel _rows{};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _scope{};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _mixed{};
    winrt::Microsoft::UI::Xaml::Controls::Button _replace{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button _clear{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button _add{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button _save{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button _cancelButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ComboBox _addKind{};
    std::vector<RowControls> _rowControls;
    std::vector<winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox> _names;
    std::vector<winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox> _roles;
    std::vector<winrt::Microsoft::UI::Xaml::FrameworkElement::Loaded_revoker> _rowLoadedRevokers;
    std::vector<winrt::Microsoft::UI::Xaml::Controls::TextBlock> _errors;
    std::vector<winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker> _buttonRevokers;
    std::vector<winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker> _rowButtonRevokers;
    std::vector<winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox::TextChanged_revoker> _textRevokers;
    std::vector<winrt::Microsoft::UI::Xaml::Controls::ComboBox::SelectionChanged_revoker> _kindRevokers;
    winrt::Microsoft::UI::Xaml::UIElement::KeyDown_revoker _keyRevoker{};
    winrt::Microsoft::UI::Xaml::FrameworkElement::Loaded_revoker _loadedRevoker{};
    bool _building = false;
    bool _enabled = true;
  };
} // namespace ao::winui
