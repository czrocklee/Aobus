// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/winui/track/TrackPropertiesAdapter.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
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
}

namespace ao::uimodel
{
  class TrackAuthoringSession;
  struct TrackPropertiesFormRow;
}

namespace ao::winui
{
  struct TrackPropertiesCoordinatorConfig final
  {
    winrt::Microsoft::UI::Xaml::XamlRoot xamlRoot{nullptr};
    async::Runtime& asyncRuntime;
    rt::Library& library;
    rt::WorkspaceService& workspace;
    rt::CompletionService& completion;
    i18n::MessageCatalog textCatalog;
    std::vector<TrackId> trackIds;
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
    bool active() const noexcept { return _active; }
    void retire() noexcept;

  private:
    struct FieldEditor final
    {
      rt::TrackField field = rt::TrackField::Title;
      TrackPropertyControlKind controlKind = TrackPropertyControlKind::Text;
      winrt::Microsoft::UI::Xaml::Controls::TextBox textBox{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox suggestBox{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::TextBox::TextChanged_revoker textChangedRevoker{};
      winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox::TextChanged_revoker suggestionChangedRevoker{};
      bool enabled = false;
    };

    struct CustomEditor final
    {
      std::string key;
      std::optional<std::string> optOriginalValue;
      winrt::Microsoft::UI::Xaml::Controls::Grid panel{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::TextBox value{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::TextBox::TextChanged_revoker valueChangedRevoker{};
      winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker deleteClickRevoker{};
      bool existed = false;
      bool editable = false;
      bool deleted = false;
    };

    Result<> prepareSession();
    void buildDialog();
    void buildFieldModel();
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
    void refreshMetadataSuggestions(FieldEditor const& editor);
    std::string editorText(FieldEditor const& editor) const;
    void refreshTagSuggestions();
    void refreshCustomKeySuggestions();
    bool synchronizeFieldEdits();
    bool hasPendingChanges() const;
    rt::MetadataPatch buildMetadataPatch() const;
    std::vector<std::string> tagsToAdd() const;
    std::vector<std::string> tagsToRemove() const;
    void updateSaveEnabled();
    void setError(std::string text);
    void clearError();
    void handleSessionInvalidated();
    void handleSaveClicked(winrt::Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const& args);
    void finishSave(Result<TrackPropertiesCommitState> result);
    void handleClosed();

    static async::Task<Result<TrackPropertiesCommitState>> submitChanges(
      std::shared_ptr<uimodel::TrackAuthoringSession> sessionPtr,
      rt::TrackPropertiesPatch patch);
    static async::Task<void> runSaveWorkflow(async::Runtime* runtime,
                                             TrackPropertiesCoordinator* owner,
                                             std::weak_ptr<std::monostate> lifetimePtr,
                                             async::Task<Result<TrackPropertiesCommitState>> submission,
                                             std::stop_token stopToken);

    winrt::Microsoft::UI::Xaml::XamlRoot _xamlRoot{nullptr};
    async::Runtime& _asyncRuntime;
    rt::Library& _library;
    rt::WorkspaceService& _workspace;
    rt::CompletionService& _completion;
    i18n::MessageCatalog _textCatalog;
    std::vector<TrackId> _trackIds;
    uimodel::TrackPropertiesFormModel _formModel;
    rt::TrackDetailSnapshot _snapshot;
    std::shared_ptr<uimodel::TrackAuthoringSession> _sessionPtr;
    async::Subscription _sessionInvalidatedSub;
    async::LifetimeScope _tasks;
    std::shared_ptr<std::monostate> _callbackLifetimePtr;

    winrt::Microsoft::UI::Xaml::Controls::ContentDialog _dialog{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _errorText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel _tagRows{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox _tagInput{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel _customRows{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox _customKeyInput{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox _customValueInput{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog::PrimaryButtonClick_revoker _primaryClickRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog::Closed_revoker _closedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox::TextChanged_revoker _tagTextChangedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox::QuerySubmitted_revoker _tagSubmittedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _tagAddClickRevoker{};
    std::vector<winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker> _tagRemoveClickRevokers;
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox::TextChanged_revoker _customKeyChangedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _customAddClickRevoker{};
    winrt::Windows::Foundation::IAsyncOperation<winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult>
      _showOperation{nullptr};

    std::vector<FieldEditor> _fieldEditors;
    std::vector<CustomEditor> _customEditors;
    std::vector<std::string> _originalTags;
    std::vector<std::string> _currentTags;
    bool _building = false;
    bool _saving = false;
    bool _sessionInvalid = false;
    bool _active = false;
  };
} // namespace ao::winui
