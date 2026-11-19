// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>

// Asio's Win32 headers define GetCurrentTime(); XAML declares a method with that name.
#pragma push_macro("GetCurrentTime")
#include <unknwn.h>
#undef GetCurrentTime
#include <winrt/Microsoft.UI.Xaml.h>
#pragma pop_macro("GetCurrentTime")

#include <bitset>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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

namespace ao::winui
{
  class TrackPropertiesCoordinator;

  struct TrackPropertiesPresenterConfig final
  {
    std::function<winrt::Microsoft::UI::Xaml::XamlRoot()> xamlRoot;
    async::Runtime& asyncRuntime;
    rt::Library& library;
    rt::WorkspaceService& workspace;
    rt::CompletionService& completion;
    i18n::MessageCatalog textCatalog;
    std::function<void(std::string)> reportStatus;
  };

  /**
   * Window-scoped Track Properties presenter, confined to the window dispatcher.
   * Destroy the presenter before releasing its borrowed services.
   */
  class TrackPropertiesPresenter final
  {
  public:
    explicit TrackPropertiesPresenter(TrackPropertiesPresenterConfig config);
    ~TrackPropertiesPresenter();

    TrackPropertiesPresenter(TrackPropertiesPresenter const&) = delete;
    TrackPropertiesPresenter& operator=(TrackPropertiesPresenter const&) = delete;
    TrackPropertiesPresenter(TrackPropertiesPresenter&&) = delete;
    TrackPropertiesPresenter& operator=(TrackPropertiesPresenter&&) = delete;

    bool isActive() const noexcept;
    void presentSelection(std::vector<TrackId> ids);
    void editCredits(std::vector<TrackId> ids, std::bitset<library::kCreditKindCount> scope);

  private:
    struct PresentationAttempt;

    void present(std::vector<TrackId> ids, std::optional<std::bitset<library::kCreditKindCount>> optScope);

    TrackPropertiesPresenterConfig _config;
    std::unique_ptr<TrackPropertiesCoordinator> _dialogPtr;
    // Points into present() only while that call and this owner are both live.
    PresentationAttempt* _attempt = nullptr;
  };
} // namespace ao::winui
