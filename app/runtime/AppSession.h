// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CommandBus.h"
#include "CorePrimitives.h"
#include "EventBus.h"
#include "Services.h"
#include "StateTypes.h"
#include "ViewRegistry.h"

#include <filesystem>
#include <memory>

namespace ao::app
{
  struct AppSessionDependencies final
  {
    std::shared_ptr<IControlExecutor> executor{};
    std::filesystem::path libraryRoot{};
  };

  class AppSession final
  {
  public:
    explicit AppSession(AppSessionDependencies dependencies);
    ~AppSession();

    AppSession(AppSession const&) = delete;
    AppSession& operator=(AppSession const&) = delete;
    AppSession(AppSession&&) = delete;
    AppSession& operator=(AppSession&&) = delete;

    IControlExecutor& executor() noexcept;

    CommandBus& commands() noexcept;
    EventBus& events() noexcept;

    IReadOnlyStore<PlaybackState>& playback() noexcept;
    IReadOnlyStore<FocusState>& focus() noexcept;
    IReadOnlyStore<NotificationFeedState>& notifications() noexcept;

    ViewRegistry& views() noexcept;
    LibraryQueryService& queries() noexcept;
    NotificationService& notificationService() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
