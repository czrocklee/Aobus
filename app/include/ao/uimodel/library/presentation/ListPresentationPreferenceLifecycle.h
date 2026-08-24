// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>

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
                                        compat::MoveOnlyFunction<void(ListId)> onPreferenceRemoved);
    ~ListPresentationPreferenceLifecycle() = default;

    ListPresentationPreferenceLifecycle(ListPresentationPreferenceLifecycle const&) = delete;
    ListPresentationPreferenceLifecycle& operator=(ListPresentationPreferenceLifecycle const&) = delete;
    ListPresentationPreferenceLifecycle(ListPresentationPreferenceLifecycle&&) = delete;
    ListPresentationPreferenceLifecycle& operator=(ListPresentationPreferenceLifecycle&&) = delete;

  private:
    async::Subscription _changesSubscription;
  };
} // namespace ao::uimodel
