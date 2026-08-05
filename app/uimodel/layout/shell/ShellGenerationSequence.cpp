// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>

#include <ao/Error.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <utility>

namespace ao::uimodel
{
  bool isGenerationActive(std::weak_ptr<ShellGenerationGate> const& gatePtr) noexcept
  {
    auto const lockedGatePtr = gatePtr.lock();
    return lockedGatePtr != nullptr && lockedGatePtr->isActive();
  }

  std::shared_ptr<ShellGenerationGate> ShellGenerationSequence::stage()
  {
    auto gatePtr = std::make_shared<ShellGenerationGate>(static_cast<ShellGenerationId>(_nextId));
    ++_nextId;
    _stagedGatePtrs.push_back(gatePtr);
    return gatePtr;
  }

  std::shared_ptr<ShellGenerationGate> ShellGenerationSequence::takeStaged(ShellGenerationId const candidate)
  {
    auto const it = std::ranges::find(
      _stagedGatePtrs, candidate, [](std::shared_ptr<ShellGenerationGate> const& gatePtr) { return gatePtr->id(); });

    if (it == _stagedGatePtrs.end())
    {
      return nullptr;
    }

    auto gatePtr = std::move(*it);
    _stagedGatePtrs.erase(it);
    return gatePtr;
  }

  void ShellGenerationSequence::discard(ShellGenerationId const candidate)
  {
    // A staged candidate never opened its gate, so abandoning it only drops the
    // sequence's claim on it; the caller destroys whatever it already built.
    takeStaged(candidate);
  }

  Result<ShellGenerationId> ShellGenerationSequence::publish(ShellGenerationId const candidate,
                                                             std::move_only_function<Result<>()> attach)
  {
    auto candidateGatePtr = takeStaged(candidate);

    if (candidateGatePtr == nullptr)
    {
      return makeError(
        Error::Code::InvalidState,
        std::format("Shell generation {} is not staged for publication", static_cast<std::uint64_t>(candidate)));
    }

    auto previousGatePtr = std::move(_activeGatePtr);
    auto const previousId = _activeId;

    _activeGatePtr = std::move(candidateGatePtr);
    _activeId = candidate;
    _activeGatePtr->setActive(true);

    if (previousGatePtr != nullptr)
    {
      previousGatePtr->setActive(false);
    }

    auto const restorePrevious = [&]
    {
      _activeGatePtr->setActive(false);
      _activeGatePtr = std::move(previousGatePtr);
      _activeId = previousId;

      if (_activeGatePtr != nullptr)
      {
        _activeGatePtr->setActive(true);
      }
    };

    auto attachedRes = Result<>{};

    try
    {
      attachedRes = attach ? attach() : Result<>{};
    }
    catch (std::exception const& error)
    {
      restorePrevious();
      return makeError(
        Error::Code::InitFailed, std::format("Shell generation attachment threw an exception: {}", error.what()));
    }
    catch (...)
    {
      restorePrevious();
      return makeError(Error::Code::InitFailed, "Shell generation attachment threw an unknown exception");
    }

    if (!attachedRes)
    {
      restorePrevious();
      return std::unexpected{attachedRes.error()};
    }

    return previousId;
  }

  ShellGenerationId ShellGenerationSequence::retireActive() noexcept
  {
    auto const retiredId = _activeId;

    if (_activeGatePtr != nullptr)
    {
      _activeGatePtr->setActive(false);
      _activeGatePtr.reset();
    }

    _activeId = ShellGenerationId::None;
    return retiredId;
  }
} // namespace ao::uimodel
