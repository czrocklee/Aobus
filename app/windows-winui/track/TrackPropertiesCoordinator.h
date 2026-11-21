// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/winui/CallbackAdmissionGate.h>
#include <ao/winui/track/TrackPropertiesAdapter.h>

// Asio's Win32 headers define GetCurrentTime(); XAML declares a method with that name.
#pragma push_macro("GetCurrentTime")
#include <unknwn.h>
#undef GetCurrentTime
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>
#pragma pop_macro("GetCurrentTime")

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ao::async
{
  class Runtime;
}

namespace ao::rt
{
  class CompletionService;
  class Library;
  class WorkspaceService;
  struct MetadataPatch;
}

namespace ao::winui
{
  class TrackCreditsEditorControl;
  struct TrackPropertiesCoordinatorConfig final
  {
    winrt::Microsoft::UI::Xaml::XamlRoot xamlRoot{nullptr};
    async::Runtime& asyncRuntime;
    rt::Library& library;
    rt::WorkspaceService& workspace;
    rt::CompletionService& completion;
    i18n::MessageCatalog textCatalog;
    std::vector<TrackId> trackIds;
    std::optional<std::bitset<library::kCreditKindCount>> optCreditsScope;
  };

  /** Native, window-owned Properties workflow for one captured track selection. */
  class TrackPropertiesCoordinator final
  {
  public:
    explicit TrackPropertiesCoordinator(TrackPropertiesCoordinatorConfig config);
    ~TrackPropertiesCoordinator();

    TrackPropertiesCoordinator(TrackPropertiesCoordinator const&) = delete;
    TrackPropertiesCoordinator& operator=(TrackPropertiesCoordinator const&) = delete;
    TrackPropertiesCoordinator(TrackPropertiesCoordinator&&) = delete;
    TrackPropertiesCoordinator& operator=(TrackPropertiesCoordinator&&) = delete;

    Result<> present();
    bool isActive() const noexcept { return _active; }
    /// Current native presentation; null before present() and after closure or retirement.
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog element() const { return _dialog; }
    void retire() noexcept;

  private:
    enum class InteractionState : std::uint8_t
    {
      Editing,
      Submitting,
      Closing,
    };

    enum class Addition : std::uint8_t
    {
      Tag,
      CustomMetadata,
    };

    struct AdditionRejection final
    {
      Addition kind;
      std::string message;
    };

    struct DraftFeedback final
    {
      TrackPropertiesRetryDraft retryDraft = TrackPropertiesRetryDraft::Blocked;
      std::optional<uimodel::TrackPropertiesFormEditorKind> optInvalidKind;
      bool showPendingHint = false;
    };

    struct FieldEditor final
    {
      TrackPropertyRowProjection row;
      winrt::Microsoft::UI::Xaml::Controls::TextBox textBox{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox suggestBox{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::CheckBox clearCheck{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::CheckBox::Checked_revoker checkedRevoker{};
      winrt::Microsoft::UI::Xaml::Controls::CheckBox::Unchecked_revoker uncheckedRevoker{};
      winrt::Microsoft::UI::Xaml::Controls::TextBox::TextChanged_revoker textChangedRevoker{};
      winrt::Microsoft::UI::Xaml::Controls::TextBox::LostFocus_revoker lostFocusRevoker{};
      winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox::TextChanged_revoker suggestionChangedRevoker{};

      bool isClearRequested() const { return clearCheck && clearCheck.IsChecked().GetBoolean(); }
      void setEnabled(bool enabled) const;
    };

    struct CustomEditor final
    {
      std::string key;
      std::optional<std::string> optOriginalValue;
      winrt::Microsoft::UI::Xaml::Controls::Grid panel{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::TextBox value{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::Button remove{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::TextBox::TextChanged_revoker valueChangedRevoker{};
      winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker deleteClickRevoker{};
      bool existed = false;
      bool editable = false;
      bool deleted = false;
    };

    Result<> prepareSession();
    void buildDialog();
    void buildDialogContent();
    void buildCreditsSection(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& content);
    void refreshCreditsPreview();
    void beginCreditsEdit(std::bitset<library::kCreditKindCount> scope);
    void finishCreditsEdit(bool accept);
    void requestDiscard(bool reload);
    void cancelDiscard();
    void confirmDiscard();
    void buildMetadataSection(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& content);
    void buildTechnicalSection(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& content);
    void buildTagsSection(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& content);
    void buildCustomMetadataSection(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& content);
    void appendFieldEditor(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& content,
                           uimodel::TrackPropertiesFormRow const& row);
    void appendCustomEditor(rt::CustomMetadataItem const& item);
    void appendNewCustomEditor(std::string key, std::string value);
    void rebuildTagRows();
    void addTag();
    void addCustomMetadata();
    void deleteCustomMetadata(std::size_t index);
    bool acceptsFieldGesture() const;
    void handleClearToggled(winrt::Microsoft::UI::Xaml::Controls::CheckBox const& source);
    void restoreWithdrawnInput(winrt::Microsoft::UI::Xaml::Controls::TextBox const& source);
    void refreshMetadataSuggestions(FieldEditor const& editor);
    std::string editorText(FieldEditor const& editor) const;
    void refreshTagSuggestions();
    void refreshCustomKeySuggestions();
    std::optional<uimodel::TrackPropertiesFormEditorKind> synchronizeFieldEdits(
      uimodel::TrackPropertiesFormModel& form) const;
    bool hasAuxiliaryChanges() const;
    bool hasUnstagedTagInput() const;
    bool hasUnstagedCustomInput() const;
    bool hasUnstagedInput() const;
    bool needsDiscardDecision() const;
    rt::MetadataPatch buildMetadataPatch() const;
    void setInteractionState(InteractionState state);
    void updateEditorEnabled();
    DraftFeedback synchronizeDraftFeedback(std::string_view actionError, std::optional<Addition> optRejectedAddition);
    winrt::hstring dialogFeedbackText(DraftFeedback const& draft,
                                      bool bindingCurrent,
                                      std::string_view actionError) const;
    TrackPropertiesRetryDraft updateDialogFeedback(std::string_view actionError = {},
                                                   std::optional<Addition> optRejectedAddition = {});
    void clearAdditionRejection(Addition kind);
    void handleSessionInvalidated();
    void handleSaveClicked(winrt::Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const& args);
    void finishSave(Result<uimodel::TrackPropertiesSubmitResult> res);
    void handleClosed();

    static async::Task<void> runSaveWorkflowAsync(async::Runtime* runtime,
                                                  TrackPropertiesCoordinator* owner,
                                                  CallbackAdmissionGate::Token token,
                                                  async::Task<Result<uimodel::TrackPropertiesSubmitResult>> submission,
                                                  std::stop_token stopToken);

    winrt::Microsoft::UI::Xaml::XamlRoot _xamlRoot{nullptr};
    async::Runtime& _asyncRuntime;
    rt::Library& _library;
    rt::WorkspaceService& _workspace;
    rt::CompletionService& _completion;
    i18n::MessageCatalog _textCatalog;
    std::vector<TrackId> _trackIds;
    std::optional<std::bitset<library::kCreditKindCount>> _optCreditsScope;
    uimodel::TrackPropertiesFormModel _formModel;
    uimodel::TrackPropertiesFormSpec _formSpec;
    rt::TrackDetailSnapshot _snapshot;
    std::optional<uimodel::TrackAuthoringSession> _optSession;
    async::Subscription _sessionInvalidatedSub;
    async::LifetimeScope _tasks;
    CallbackAdmissionGate _ownerCallbackGate;

    winrt::Microsoft::UI::Xaml::Controls::ContentDialog _dialog{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _errorText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _unstagedInputHint{nullptr};
    std::optional<std::string> _optFrozenError;
    std::optional<AdditionRejection> _optAdditionRejection;
    TrackPropertiesRetryFeedback _retryFeedback = TrackPropertiesRetryFeedback::None;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel _creditsPreview{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel _creditsHost{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel _discardPrompt{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button _reloadButton{nullptr};
    std::unique_ptr<TrackCreditsEditorControl> _creditsControlPtr;
    std::vector<winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker> _creditClickRevokers;
    std::vector<winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker> _discardClickRevokers;
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog::Closing_revoker _closingRevoker{};
    bool _reloadAfterDiscard = false;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel _tagRows{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox _tagInput{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button _tagAddButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel _customRows{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox _customKeyInput{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox _customValueInput{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button _customAddButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog::PrimaryButtonClick_revoker _primaryClickRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog::Closed_revoker _closedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox::TextChanged_revoker _tagTextChangedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox::QuerySubmitted_revoker _tagSubmittedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _tagAddClickRevoker{};
    std::vector<winrt::Microsoft::UI::Xaml::Controls::Button> _tagRemoveButtons;
    std::vector<winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker> _tagRemoveClickRevokers;
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox::TextChanged_revoker _customKeyChangedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::TextBox::TextChanged_revoker _customValueChangedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _customAddClickRevoker{};
    winrt::Windows::Foundation::IAsyncOperation<winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult>
      _showOperation{nullptr};

    std::vector<FieldEditor> _fieldEditors;
    std::vector<CustomEditor> _customEditors;
    std::vector<std::string> _originalTags;
    std::vector<std::string> _currentTags;
    bool _building = false;
    InteractionState _interactionState = InteractionState::Editing;
    bool _sessionInvalid = false;
    bool _active = false;
  };
} // namespace ao::winui
