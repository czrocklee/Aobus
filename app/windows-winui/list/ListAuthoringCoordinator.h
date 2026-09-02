// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/uimodel/library/list/ListAuthoring.h>
#include <ao/uimodel/library/list/ListOrder.h>
#include <ao/uimodel/library/list/SmartListEditing.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/winui/CallbackAdmissionGate.h>
#include <ao/winui/list/ListAuthoringAdapter.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

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

    bool dialogActive() const noexcept { return _dialogActive; }
    void retire() noexcept;

  private:
    void presentEditor(ListId parentListId,
                       ListId editListId,
                       std::string name,
                       std::string description,
                       std::string expression);
    void buildEditorDialog();
    void scheduleEditorPreview();
    void updateEditorPreview();
    void handleEditorSave(winrt::Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const& args);
    void finishEditorSave(Result<ListId> result);

    void finishDeletePreview(ListId listId, bool includeDescendants, Result<rt::DeleteListSubtreeReply> result);
    void buildDeleteDialog(ListId listId, bool includeDescendants, rt::DeleteListSubtreeReply const& preview);
    void handleDeleteCommit(winrt::Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const& args);
    void finishDeleteCommit(Result<rt::DeleteListSubtreeReply> result);

    void finishMembership(Result<uimodel::ListMembershipEditResult> result);
    void finishOrder(ListOrderCommand command, Result<rt::AuthoringResult<rt::MoveListOrderReply>> result);
    void finishOrderReset(Result<rt::AuthoringResult<rt::ResetListOrderReply>> result);

    void beginDialogWorkflow();
    void showDialog();
    void setDialogError(std::string text);
    void handleDialogClosed();

    template<typename ResultType, typename Finish>
    static async::Task<void> finishOnCallbackExecutor(async::Runtime* runtime,
                                                      ListAuthoringCoordinator* owner,
                                                      CallbackAdmissionGate::Token token,
                                                      async::Task<ResultType> submission,
                                                      Finish finish,
                                                      std::stop_token stopToken)
    {
      auto result = co_await std::move(submission);
      co_await runtime->resumeOnCallbackExecutor(stopToken);

      if (token.admits())
      {
        std::invoke(std::move(finish), owner, std::move(result));
      }
    }

    std::function<winrt::Microsoft::UI::Xaml::XamlRoot()> _xamlRoot;
    async::Runtime& _asyncRuntime;
    rt::Library& _library;
    rt::ViewService& _views;
    rt::TrackSourceCache& _sources;
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
    winrt::Microsoft::UI::Xaml::Controls::TextBox::TextChanged_revoker _filterChangedRevoker{};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer::Tick_revoker _previewTickRevoker{};

    std::vector<std::string> _presentationIds;
    uimodel::SmartListEditorViewState _editorState;
    ListId _parentListId = kInvalidListId;
    ListId _editListId = kInvalidListId;
    ListId _deleteListId = kInvalidListId;
    std::string _inheritedExpression;
    std::string _mutationError;
    bool _deleteDescendants = false;
    bool _deleteContainsActive = false;
    bool _dialogActive = false;
    bool _submitting = false;
    bool _retired = false;
  };
} // namespace ao::winui
