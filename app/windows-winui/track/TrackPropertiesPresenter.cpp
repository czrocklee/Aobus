// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "track/TrackPropertiesPresenter.h"

#include "platform/StringResourceFormat.h"
#include "track/TrackPropertiesCoordinator.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/base.h>

#include <bitset>
#include <exception>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::winui
{
  struct TrackPropertiesPresenter::PresentationAttempt final
  {
    explicit PresentationAttempt(TrackPropertiesPresenter& owner) noexcept
      : presenter{&owner}
    {
      owner._attempt = this;
    }

    ~PresentationAttempt()
    {
      resetDialog();

      if (presenter != nullptr)
      {
        presenter->_attempt = nullptr;
      }
    }

    PresentationAttempt(PresentationAttempt const&) = delete;
    PresentationAttempt& operator=(PresentationAttempt const&) = delete;
    PresentationAttempt(PresentationAttempt&&) = delete;
    PresentationAttempt& operator=(PresentationAttempt&&) = delete;

    void resetDialog() noexcept
    {
      // Native dismissal can re-enter owner teardown. Clear the slot before deleting.
      std::exchange(dialogPtr, nullptr).reset();
    }

    void reportError(std::string_view const detail) const
    {
      if (presenter == nullptr || !presenter->_config.reportStatus)
      {
        return;
      }

      auto const reportStatus = presenter->_config.reportStatus;

      if (auto message = formatResource("winui_error", detail); presenter != nullptr)
      {
        reportStatus(std::move(message));
      }
    }

    // The presenter clears this before teardown can re-enter native code.
    TrackPropertiesPresenter* presenter;
    std::unique_ptr<TrackPropertiesCoordinator> dialogPtr;
  };

  TrackPropertiesPresenter::TrackPropertiesPresenter(TrackPropertiesPresenterConfig config)
    : _config{std::move(config)}
  {
  }

  TrackPropertiesPresenter::~TrackPropertiesPresenter()
  {
    if (_attempt != nullptr)
    {
      _attempt->presenter = nullptr;

      // The window can release its services before ShowAsync returns. Revoke the
      // coordinator's subscriptions now without deleting it on its own stack.
      if (_attempt->dialogPtr)
      {
        _attempt->dialogPtr->retire();
      }
    }

    std::exchange(_dialogPtr, nullptr).reset();
  }

  bool TrackPropertiesPresenter::isActive() const noexcept
  {
    return _attempt != nullptr || (_dialogPtr && _dialogPtr->isActive());
  }

  void TrackPropertiesPresenter::presentSelection(std::vector<TrackId> ids)
  {
    present(std::move(ids), std::nullopt);
  }

  void TrackPropertiesPresenter::editCredits(std::vector<TrackId> ids,
                                             std::bitset<library::kCreditKindCount> const scope)
  {
    present(std::move(ids), scope);
  }

  void TrackPropertiesPresenter::present(std::vector<TrackId> ids,
                                         std::optional<std::bitset<library::kCreditKindCount>> const optScope)
  {
    if (ids.empty() || isActive())
    {
      return;
    }

    // Native calls can re-enter before ShowAsync returns, including retiring this presenter.
    auto attempt = PresentationAttempt{*this};
    std::exchange(_dialogPtr, nullptr).reset();

    try
    {
      if (attempt.presenter == nullptr)
      {
        return;
      }

      auto const xamlRoot = attempt.presenter->_config.xamlRoot;
      auto root = xamlRoot ? xamlRoot() : winrt::Microsoft::UI::Xaml::XamlRoot{nullptr};

      if (attempt.presenter == nullptr)
      {
        return;
      }

      auto const& config = attempt.presenter->_config;
      attempt.dialogPtr = std::make_unique<TrackPropertiesCoordinator>(TrackPropertiesCoordinatorConfig{
        .xamlRoot = std::move(root),
        .asyncRuntime = config.asyncRuntime,
        .library = config.library,
        .workspace = config.workspace,
        .completion = config.completion,
        .textCatalog = config.textCatalog,
        .trackIds = std::move(ids),
        .optCreditsScope = optScope,
      });

      if (attempt.presenter == nullptr)
      {
        return;
      }

      auto const presentedRes = attempt.dialogPtr->present();

      // If ShowAsync re-entered teardown, only the stack pin remains. Dropping
      // it also releases any show operation returned after retirement.
      if (attempt.presenter == nullptr)
      {
        return;
      }

      if (!presentedRes)
      {
        attempt.reportError(presentedRes.error().message);
        return;
      }

      attempt.presenter->_dialogPtr = std::move(attempt.dialogPtr);
    }
    catch (winrt::hresult_error const& error)
    {
      attempt.resetDialog();

      if (attempt.presenter != nullptr)
      {
        attempt.reportError(winrt::to_string(error.message()));
      }
    }
    catch (...)
    {
      attempt.resetDialog();
      AO_FATAL_EXCEPTION(std::current_exception(), "WinUI track-properties presentation");
    }
  }
} // namespace ao::winui
