// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>

#include <functional>
#include <map>
#include <string>

namespace ao::rt
{
  class LibraryChanges;
}

namespace ao::uimodel
{
  /**
   * Owns the shared deletion lifecycle for per-List presentation preferences.
   */
  class ListPresentationPreferenceLifecycle final
  {
  public:
    ListPresentationPreferenceLifecycle(std::map<ListId, std::string>& presentations,
                                        rt::LibraryChanges const& changes,
                                        std::move_only_function<void(ListId) noexcept> onPreferenceRemoved);
    ~ListPresentationPreferenceLifecycle() = default;

    ListPresentationPreferenceLifecycle(ListPresentationPreferenceLifecycle const&) = delete;
    ListPresentationPreferenceLifecycle& operator=(ListPresentationPreferenceLifecycle const&) = delete;
    ListPresentationPreferenceLifecycle(ListPresentationPreferenceLifecycle&&) = delete;
    ListPresentationPreferenceLifecycle& operator=(ListPresentationPreferenceLifecycle&&) = delete;

  private:
    std::map<ListId, std::string>& _presentations;
    std::move_only_function<void(ListId) noexcept> _onPreferenceRemoved;
    async::Subscription _changesSubscription;
  };
} // namespace ao::uimodel
