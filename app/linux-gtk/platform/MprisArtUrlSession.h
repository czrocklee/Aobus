// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/utility/ScopedRegistration.h>

#include <functional>
#include <memory>
#include <string>

namespace ao::gtk
{
  class MainContextCallbackScope;
}

namespace ao::gtk::platform
{
  /**
   * Owns the active MPRIS art URL request and its completion lifetime.
   *
   * The session, requester, cancellation callback, and completion callback must
   * remain confined to the same GLib main context.
   */
  class [[nodiscard]] MprisArtUrlSession final
  {
  public:
    using OnUrlReady = std::function<void(std::string)>;
    using UrlRequester = std::function<utility::ScopedRegistration(ResourceId, OnUrlReady)>;
    using OnUrlChanged = std::function<void()>;

    MprisArtUrlSession(UrlRequester requestUrl, OnUrlChanged onUrlChanged);
    ~MprisArtUrlSession();

    MprisArtUrlSession(MprisArtUrlSession const&) = delete;
    MprisArtUrlSession& operator=(MprisArtUrlSession const&) = delete;
    MprisArtUrlSession(MprisArtUrlSession&&) = delete;
    MprisArtUrlSession& operator=(MprisArtUrlSession&&) = delete;

    void refresh(ResourceId resourceId);
    void clear();
    std::string urlFor(ResourceId resourceId) const;

  private:
    void invalidateRequest();
    void handleUrlReady(std::string url);

    UrlRequester _requestUrl;
    OnUrlChanged _onUrlChanged;
    utility::ScopedRegistration _request;
    std::unique_ptr<MainContextCallbackScope> _callbackScopePtr;
    ResourceId _resourceId = kInvalidResourceId;
    std::string _url;
  };
} // namespace ao::gtk::platform
