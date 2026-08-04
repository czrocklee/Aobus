// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "platform/StringResources.h"

#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>

#include <cctype>
#include <string>
#include <string_view>

namespace ao::winui
{
  namespace
  {
    winrt::hstring lookup(std::wstring_view const resourceId)
    {
      try
      {
        auto const loader = winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader{};
        return loader.GetString(winrt::hstring{resourceId});
      }
      catch (winrt::hresult_error const&)
      {
        return {};
      }
    }
  } // namespace

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
      auto const byte = static_cast<unsigned char>(character);
      key.push_back(std::isalnum(byte) != 0 ? character : '_');
    }

    return resourceStringOr(key, fallback);
  }
} // namespace ao::winui
