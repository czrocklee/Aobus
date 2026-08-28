// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/presentation/PresentationText.h>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.h>

#include <functional>
#include <memory>
#include <string>

namespace ao::async
{
  class Runtime;
}

namespace ao::rt
{
  class LibraryJobs;
  class NotificationService;
}

namespace ao::winui
{
  struct LibraryTransferCoordinatorConfig final
  {
    std::function<winrt::Microsoft::UI::Xaml::XamlRoot()> xamlRoot;
    winrt::Microsoft::UI::WindowId windowId{};
    async::Runtime& asyncRuntime;
    rt::LibraryJobs& jobs;
    rt::NotificationService& notifications;
    i18n::MessageCatalog textCatalog;
    std::function<void(std::string)> reportStatus;
  };

  /** Window-owned WinUI YAML import/export dialogs, pickers, and tasks. */
  class LibraryTransferCoordinator final
  {
  public:
    explicit LibraryTransferCoordinator(LibraryTransferCoordinatorConfig config);
    ~LibraryTransferCoordinator();

    LibraryTransferCoordinator(LibraryTransferCoordinator const&) = delete;
    LibraryTransferCoordinator& operator=(LibraryTransferCoordinator const&) = delete;
    LibraryTransferCoordinator(LibraryTransferCoordinator&&) = delete;
    LibraryTransferCoordinator& operator=(LibraryTransferCoordinator&&) = delete;

    void importLibrary();
    void exportLibrary();
    bool active() const noexcept;
    void retire() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::winui
