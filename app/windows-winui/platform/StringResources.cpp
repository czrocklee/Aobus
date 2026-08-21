// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "platform/StringResources.h"

#include <ao/Error.h>
#include <ao/utility/String.h>

#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace ao::winui
{
  namespace
  {
    namespace resources = winrt::Microsoft::Windows::ApplicationModel::Resources;

    struct ResourceLookupState final
    {
      std::string localeTag;
      resources::ResourceManager manager;
      resources::ResourceContext context;
      resources::ResourceMap map;
    };

    struct ResourceLookupHolder final
    {
      std::mutex mutex;
      std::shared_ptr<ResourceLookupState const> statePtr;
    };

    ResourceLookupHolder& resourceLookupHolder()
    {
      static auto holder = ResourceLookupHolder{};
      return holder;
    }

    std::shared_ptr<ResourceLookupState const> configuredState()
    {
      auto& holder = resourceLookupHolder();
      auto lock = std::scoped_lock{holder.mutex};
      return holder.statePtr;
    }

    winrt::hstring lookup(std::wstring_view const resourceId)
    {
      try
      {
        auto const statePtr = configuredState();

        if (!statePtr)
        {
          return {};
        }

        auto const candidate = statePtr->map.TryGetValue(winrt::hstring{resourceId}, statePtr->context);
        return candidate ? candidate.ValueAsString() : winrt::hstring{};
      }
      catch (winrt::hresult_error const&)
      {
        return {};
      }
    }
  } // namespace

  Result<> configureResourceLanguage(std::string_view const localeTag)
  {
    try
    {
      auto manager = resources::ResourceManager{};
      auto context = manager.CreateResourceContext();
      context.QualifierValues().Insert(L"Language", winrt::to_hstring(localeTag));
      auto map = manager.MainResourceMap().GetSubtree(L"Resources");

      if (!map)
      {
        return makeError(Error::Code::InitFailed, "WinUI resources contain no Resources subtree");
      }

      auto candidatePtr = std::make_shared<ResourceLookupState>(ResourceLookupState{
        .localeTag = std::string{localeTag},
        .manager = std::move(manager),
        .context = std::move(context),
        .map = std::move(map),
      });

      auto& holder = resourceLookupHolder();
      auto lock = std::scoped_lock{holder.mutex};

      if (holder.statePtr)
      {
        if (holder.statePtr->localeTag == localeTag)
        {
          return {};
        }

        return makeError(Error::Code::Conflict, "WinUI resource language was already configured");
      }

      holder.statePtr = std::move(candidatePtr);
      return {};
    }
    catch (winrt::hresult_error const& error)
    {
      return makeError(Error::Code::InitFailed,
                       "Could not configure the WinUI resource language: " + winrt::to_string(error.message()));
    }
  }

  void resetResourceLanguage()
  {
    auto& holder = resourceLookupHolder();
    auto lock = std::scoped_lock{holder.mutex};
    holder.statePtr.reset();
  }

  winrt::hstring resourceHstring(std::wstring_view const resourceId)
  {
    auto const value = lookup(resourceId);
    return value.empty() ? winrt::hstring{resourceId} : value;
  }

  std::string resourceString(std::string_view const resourceId)
  {
    return winrt::to_string(resourceHstring(winrt::to_hstring(resourceId)));
  }

  std::string resourceStringOr(std::string_view const resourceId, std::string_view const fallback)
  {
    auto const value = lookup(winrt::to_hstring(resourceId));
    return value.empty() ? std::string{fallback} : winrt::to_string(value);
  }

  std::string stableResourceString(std::string_view const prefix,
                                   std::string_view const stableId,
                                   std::string_view const fallback)
  {
    auto key = std::string{prefix};
    key.reserve(prefix.size() + stableId.size());

    for (auto const character : stableId)
    {
      key.push_back(utility::isAsciiAlphaNumeric(character) ? character : '_');
    }

    return resourceStringOr(key, fallback);
  }
} // namespace ao::winui
