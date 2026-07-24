// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MprisArtUrlSession.h"

#include "common/MainContextCallbackScope.h"
#include <ao/CoreIds.h>

#include <memory>
#include <string>
#include <utility>

namespace ao::gtk::platform
{
  MprisArtUrlSession::MprisArtUrlSession(UrlRequester requestUrl, OnUrlChanged onUrlChanged)
    : _requestUrl{std::move(requestUrl)}, _onUrlChanged{std::move(onUrlChanged)}
  {
  }

  MprisArtUrlSession::~MprisArtUrlSession()
  {
    clear();
  }

  void MprisArtUrlSession::refresh(ResourceId const resourceId)
  {
    if (resourceId == _resourceId)
    {
      return;
    }

    invalidateRequest();
    _resourceId = resourceId;
    _url.clear();

    if (resourceId == kInvalidResourceId || !_requestUrl)
    {
      return;
    }

    _callbackScopePtr = std::make_unique<MainContextCallbackScope>();
    auto onReady = _callbackScopePtr->guard([this](std::string url) { handleUrlReady(std::move(url)); });

    try
    {
      if (auto request = _requestUrl(resourceId, std::move(onReady)); _callbackScopePtr)
      {
        _request = std::move(request);
      }
    }
    catch (...)
    {
      _callbackScopePtr.reset();
      throw;
    }
  }

  void MprisArtUrlSession::clear()
  {
    invalidateRequest();
    _resourceId = kInvalidResourceId;
    _url.clear();
  }

  std::string MprisArtUrlSession::urlFor(ResourceId const resourceId) const
  {
    return resourceId == _resourceId ? _url : std::string{};
  }

  void MprisArtUrlSession::invalidateRequest()
  {
    _callbackScopePtr.reset();
    _request.reset();
  }

  void MprisArtUrlSession::handleUrlReady(std::string url)
  {
    _url = std::move(url);
    invalidateRequest();

    if (_onUrlChanged)
    {
      _onUrlChanged();
    }
  }
} // namespace ao::gtk::platform
