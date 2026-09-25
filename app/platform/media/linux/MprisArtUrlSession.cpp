// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MprisArtUrlSession.h"

#include <ao/CoreIds.h>

#include <memory>
#include <string>
#include <utility>

namespace ao::media
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

    _callbackStatePtr = std::make_shared<CallbackState>();
    // Keep only a weak local identity: synchronous completion must expire the
    // scope before destroying the request returned by the requester.
    auto const weakStatePtr = std::weak_ptr<CallbackState>{_callbackStatePtr};
    auto onReady = [weakStatePtr, this](std::string url)
    {
      if (!weakStatePtr.lock())
      {
        return;
      }

      handleUrlReady(std::move(url));
    };

    try
    {
      auto request = _requestUrl(resourceId, std::move(onReady));

      if (_callbackStatePtr && _callbackStatePtr == weakStatePtr.lock())
      {
        _request = std::move(request);
      }
    }
    catch (...)
    {
      if (_callbackStatePtr && _callbackStatePtr == weakStatePtr.lock())
      {
        _callbackStatePtr.reset();
      }

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
    _callbackStatePtr.reset();
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
} // namespace ao::media
