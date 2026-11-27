// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/uimodel/library/list/ListAuthoring.h>
#include <ao/uimodel/library/list/ListOrder.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/winui/CallbackAdmissionGate.h>
#include <ao/winui/list/ListAuthoringAdapter.h>

// Asio's Win32 headers define a macro that collides with XAML's method.
#pragma push_macro("GetCurrentTime")
#include <unknwn.h>
#undef GetCurrentTime
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#pragma pop_macro("GetCurrentTime")
#include <winrt/Windows.Foundation.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ao::async
{
  class Runtime;
}

namespace ao::rt
{
  class Library;
  class NotificationService;
  class TextOrderingPolicy;
  class TrackSourceCache;
  class ViewService;
}

namespace ao::uimodel
{
  class ListPresentations;
  class TrackPresentationCatalog;
}

namespace ao::winui
{
  class TrackListController;

  struct ListAuthoringCoordinatorConfig final
  {
    std::function<winrt::Microsoft::UI::Xaml::XamlRoot()> xamlRoot;
    async::Runtime& asyncRuntime;
    rt::Library& library;
    rt::ViewService& views;
    rt::TrackSourceCache& sources;
    rt::NotificationService& notifications;
    TrackListController& trackList;
    uimodel::TrackPresentationCatalog& presentationCatalog;
    uimodel::ListPresentations& listPresentations;
    rt::TextOrderingPolicy const* textOrderingPolicy = nullptr;
    i18n::MessageCatalog textCatalog;
    std::function<void(std::string)> reportStatus;
  };

  /** Window-owned native workflows for List CRUD, membership, and saved order. */
  class ListAuthoringCoordinator final
  {
  public:
    explicit ListAuthoringCoordinator(ListAuthoringCoordinatorConfig config);
    ~ListAuthoringCoordinator();

    ListAuthoringCoordinator(ListAuthoringCoordinator const&) = delete;
    ListAuthoringCoordinator& operator=(ListAuthoringCoordinator const&) = delete;
    ListAuthoringCoordinator(ListAuthoringCoordinator&&) = delete;
    ListAuthoringCoordinator& operator=(ListAuthoringCoordinator&&) = delete;

    void createList(ListId parentListId, std::string initialExpression = {});
    void editList(ListId listId);
    void deleteList(ListId listId, bool includeDescendants);

    std::vector<uimodel::WritableTagListTarget> membershipTargets() const;
    void editMembership(ListId listId, bool add);

    uimodel::ListOrderCapabilityState orderCapabilities() const;
    void applyOrder(ListOrderCommand command);

    bool isDialogActive() const noexcept { return _dialogActive; }
    void retire() noexcept;

  private:
    void presentEditor(ListId parentListId,
                       ListId editListId,
                       std::string name,
                       std::string description,
                       std::string expression);
    void buildEditorDialog();
    rt::ListDraft editorDraft() const;
    /// Refresh the preview and return whether the draft can be submitted.
    bool tryValidateEditorDraft(rt::ListDraft const& draft);
    void handleEditorSave(winrt::Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const& args);
    void finishEditorSave(Result<ListId> res,
                          std::string const& name,
                          std::string const& expression,
                          std::int32_t selected);

    void finishDeletePreview(ListId listId, bool includeDescendants, Result<rt::DeleteListSubtreeReply> res);
    void buildDeleteDialog(ListId listId, bool includeDescendants, rt::DeleteListSubtreeReply const& preview);
    void handleDeleteCommit(winrt::Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const& args);
    void finishDeleteCommit(Result<rt::DeleteListSubtreeReply> res);

    void finishMembership(Result<uimodel::ListMembershipEditResult> res);
    template<typename Reply>
    void finishOrder(Result<rt::AuthoringResult<Reply>> res);
    void reportFeedback(rt::NotificationSeverity severity, std::string text, rt::NotificationLifetime lifetime);
    void reportWorkflowError(Error const& error);

    void beginDialogWorkflow();
    void showDialog();
    void setDialogError(std::string text);
    void handleDialogClosed();

    template<typename ResultType, typename Finish>
    static async::Task<void> finishOnCallbackExecutorAsync(async::Runtime* runtime,
                                                           ListAuthoringCoordinator* owner,
                                                           CallbackAdmissionGate::Token token,
                                                           async::Task<ResultType> submission,
                                                           Finish finish,
                                                           std::stop_token stopToken)
    {
      auto res = co_await std::move(submission);
      co_await runtime->resumeOnCallbackExecutorAsync(stopToken);

      if (token.accepts())
      {
        std::invoke(std::move(finish), owner, std::move(res));
      }
    }

    std::function<winrt::Microsoft::UI::Xaml::XamlRoot()> _xamlRoot;
    async::Runtime& _asyncRuntime;
    rt::Library& _library;
    rt::ViewService& _views;
    rt::TrackSourceCache& _sources;
    rt::NotificationService& _notifications;
    TrackListController& _trackList;
    uimodel::TrackPresentationCatalog& _presentationCatalog;
    uimodel::ListPresentations& _listPresentations;
    rt::TextOrderingPolicy const* _textOrderingPolicy = nullptr;
    i18n::MessageCatalog _textCatalog;
    std::function<void(std::string)> _reportStatus;
    std::optional<async::LifetimeScope> _optDialogTasks;
    async::LifetimeScope _commandTasks;
    CallbackAdmissionGate _ownerCallbackGate;
    CallbackAdmissionGate _dialogGenerationGate;

    winrt::Microsoft::UI::Xaml::Controls::ContentDialog _dialog{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox _nameInput{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox _descriptionInput{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox _filterInput{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ComboBox _presentationInput{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _inheritedFilterText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _effectiveFilterText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _membershipText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _previewText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _errorText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::CheckBox _removeTagCheck{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer _previewTimer{nullptr};
    winrt::Windows::Foundation::IAsyncOperation<winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult>
      _showOperation{nullptr};

    winrt::Microsoft::UI::Xaml::Controls::ContentDialog::PrimaryButtonClick_revoker _primaryClickRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog::Closed_revoker _closedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::TextBox::TextChanged_revoker _nameChangedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::TextBox::TextChanged_revoker _descriptionChangedRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::TextBox::TextChanged_revoker _filterChangedRevoker{};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer::Tick_revoker _previewTickRevoker{};

    std::vector<std::optional<rt::TrackPresetId>> _presentationIds;
    ListId _parentListId = kInvalidListId;
    ListId _editListId = kInvalidListId;
    ListId _deleteListId = kInvalidListId;
    async::Subscription _previewChanges;
    std::optional<rt::TrackSourceLease> _optPreviewLease;
    std::string _mutationError;
    bool _deleteDescendants = false;
    bool _deleteContainsActive = false;
    bool _dialogActive = false;
    bool _submitting = false;
    bool _retired = false;
  };
} // namespace ao::winui
