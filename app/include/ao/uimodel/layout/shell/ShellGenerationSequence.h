// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/compat/MoveOnlyFunction.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ao::uimodel
{
  /// Monotonic identity of one shell view generation.
  // Generation ids use the same 64-bit domain as the monotonic counter; narrowing
  // the enum would make the identity wrap while the sequence remains live.
  enum class ShellGenerationId : std::uint64_t // NOLINT(performance-enum-size)
  {
    None = 0,
  };

  /**
   * @brief Shared gate that decides whether one generation's callbacks may still run.
   *
   * A view generation owns every element, adapter, controller, subscription, and
   * view-local asynchronous operation built for its document. Those callbacks
   * capture the gate weakly rather than capturing the generation or a view
   * owner, so a completion that arrives after the generation was retired finds
   * a closed gate and does nothing.
   */
  class ShellGenerationGate final
  {
  public:
    explicit ShellGenerationGate(ShellGenerationId id) noexcept
      : _id{id}
    {
    }

    ShellGenerationId id() const noexcept { return _id; }

    /// Whether this generation is the live one. False while staged, and forever after retirement.
    bool isActive() const noexcept { return _active; }

  private:
    friend class ShellGenerationSequence;

    void setActive(bool const active) noexcept { _active = active; }

    ShellGenerationId _id = ShellGenerationId::None;
    bool _active = false;
  };

  /// Whether a callback holding @p gatePtr may still touch its view generation.
  bool isGenerationActive(std::weak_ptr<ShellGenerationGate> const& gatePtr) noexcept;

  /**
   * @brief Single-active-generation transition for a shell that rebuilds its whole view.
   *
   * A candidate is staged while the current generation stays live, so all
   * fallible construction and binding happens behind a closed gate. Publication
   * changes the active generation exactly once. If the caller's native
   * attachment reports failure, the previous generation and token are restored
   * before that value is returned. An unexpected exception is rethrown only
   * after the same rollback.
   *
   * The sequence does not own native trees. It reports which generation the
   * caller must now destroy; destruction order stays with the frontend.
   */
  class ShellGenerationSequence final
  {
  public:
    ShellGenerationId activeId() const noexcept { return _activeId; }

    /// Number of candidates staged but not yet published or discarded.
    std::size_t stagedCount() const noexcept { return _stagedGatePtrs.size(); }

    /// Begin an inactive candidate whose gate stays closed until it is published.
    std::shared_ptr<ShellGenerationGate> stage();

    /// Abandon a staged candidate. Its gate stays closed and its id is never published.
    void discard(ShellGenerationId candidate);

    /**
     * @brief Publish @p candidate as the single active generation.
     *
     * @param attach Native attachment run once the candidate holds the active
     *               token. It must contain no operation expected to fail.
     * @return The generation the caller must now retire, or `None` for the first
     *         publication. On failure the active generation is unchanged and
     *         @p candidate has been discarded. An unexpected attachment
     *         exception is rethrown after the same rollback.
     */
    Result<ShellGenerationId> publish(ShellGenerationId candidate, compat::MoveOnlyFunction<Result<>()> attach);

    /**
     * @brief Retire the live generation without publishing a replacement.
     *
     * Used at teardown, so callbacks that arrive while the shell is being
     * destroyed find a closed gate.
     *
     * @return The retired generation, or `None` when none was live.
     */
    ShellGenerationId retireActive() noexcept;

  private:
    std::shared_ptr<ShellGenerationGate> takeStaged(ShellGenerationId candidate);

    ShellGenerationId _activeId = ShellGenerationId::None;
    std::shared_ptr<ShellGenerationGate> _activeGatePtr{};
    std::vector<std::shared_ptr<ShellGenerationGate>> _stagedGatePtrs{};
    std::uint64_t _nextId = 1;
  };
} // namespace ao::uimodel
