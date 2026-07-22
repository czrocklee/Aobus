// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PreparedNextRegistry.h"

#include "runtime/playback/ProjectionAnchor.h"
#include <ao/Exception.h>
#include <ao/rt/PreparedPlayback.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <tuple>
#include <utility>

namespace ao::rt
{
  void PreparedNextRegistry::activate(PreparedNextToken const token, ProjectionAnchor anchor)
  {
    if (token.value == 0)
    {
      throwException<Exception>("Prepared-next commitment requires a valid token");
    }

    if (contains(token))
    {
      throwException<Exception>("Prepared-next token is already registered");
    }

    retireActive();
    _optActive.emplace(Commitment{.token = token, .anchor = std::move(anchor)});
  }

  bool PreparedNextRegistry::acknowledgeDisarm(PreparedNextToken const token)
  {
    if (_optActive && _optActive->token == token)
    {
      _optActive.reset();
      return true;
    }

    auto const it = std::ranges::find(_retired, token, &Commitment::token);

    if (it == _retired.end())
    {
      return false;
    }

    _retired.erase(it);
    return true;
  }

  void PreparedNextRegistry::invalidate(std::optional<PreparedNextToken> const optDisarmedToken)
  {
    if (optDisarmedToken)
    {
      std::ignore = acknowledgeDisarm(*optDisarmedToken);
    }

    retireActive();
  }

  void PreparedNextRegistry::applyBatch(TrackListProjectionDeltaBatch const& batch,
                                        std::size_t const projectionSize,
                                        ProjectionIndexResolver const& resolveIndex)
  {
    if (!_optActive && _retired.empty())
    {
      return;
    }

    if (!resolveIndex)
    {
      throwException<Exception>("Prepared-next registry requires a projection index resolver");
    }

    auto const apply = [&](Commitment& commitment)
    { commitment.anchor.applyBatch(batch, projectionSize, resolveIndex(commitment.anchor.trackId())); };

    if (_optActive)
    {
      apply(*_optActive);
    }

    for (auto& commitment : _retired)
    {
      apply(commitment);
    }
  }

  std::optional<ProjectionAnchor> PreparedNextRegistry::resolveWinner(PreparedNextToken const token)
  {
    auto const* commitment = find(token);

    if (commitment == nullptr)
    {
      return std::nullopt;
    }

    auto winner = commitment->anchor;
    clear();
    return winner;
  }

  void PreparedNextRegistry::clearCoveredByBarrier(PreparedCancellationBarrier const barrier) noexcept
  {
    if (_optActive && barrier.covers(_optActive->token))
    {
      _optActive.reset();
    }

    std::erase_if(_retired, [&](Commitment const& commitment) { return barrier.covers(commitment.token); });
  }

  void PreparedNextRegistry::clear() noexcept
  {
    _optActive.reset();
    _retired.clear();
  }

  std::optional<PreparedNextToken> PreparedNextRegistry::activeToken() const noexcept
  {
    if (!_optActive)
    {
      return std::nullopt;
    }

    return _optActive->token;
  }

  std::optional<ProjectionAnchor> PreparedNextRegistry::anchorFor(PreparedNextToken const token) const
  {
    if (auto const* commitment = find(token); commitment != nullptr)
    {
      return commitment->anchor;
    }

    return std::nullopt;
  }

  bool PreparedNextRegistry::contains(PreparedNextToken const token) const noexcept
  {
    return find(token) != nullptr;
  }

  bool PreparedNextRegistry::isRetired(PreparedNextToken const token) const noexcept
  {
    return std::ranges::find(_retired, token, &Commitment::token) != _retired.end();
  }

  std::size_t PreparedNextRegistry::size() const noexcept
  {
    return _retired.size() + static_cast<std::size_t>(_optActive.has_value());
  }

  PreparedNextRegistry::Commitment const* PreparedNextRegistry::find(PreparedNextToken const token) const noexcept
  {
    if (_optActive && _optActive->token == token)
    {
      return &*_optActive;
    }

    auto const it = std::ranges::find(_retired, token, &Commitment::token);
    return it != _retired.end() ? &*it : nullptr;
  }

  void PreparedNextRegistry::retireActive()
  {
    if (!_optActive)
    {
      return;
    }

    _retired.push_back(std::move(*_optActive));
    _optActive.reset();
  }
} // namespace ao::rt
