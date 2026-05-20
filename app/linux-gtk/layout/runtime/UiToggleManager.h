// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <sigc++/signal.h>

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace ao::gtk::layout
{
  /**
   * @brief Manages boolean toggle states for UI elements like sidebars and revealers.
   *
   * This is a GTK-layer service that decouples UI visibility states from the core runtime.
   */
  class UiToggleManager final
  {
  public:
    UiToggleManager() = default;
    ~UiToggleManager() = default;

    UiToggleManager(UiToggleManager const&) = delete;
    UiToggleManager& operator=(UiToggleManager const&) = delete;
    UiToggleManager(UiToggleManager&&) = delete;
    UiToggleManager& operator=(UiToggleManager&&) = delete;

    /**
     * @brief Set the state of a toggle.
     *
     * If the key does not exist, it is created.
     */
    void setToggleState(std::string_view key, bool state);

    /**
     * @brief Get the current state of a toggle.
     *
     * Returns false if the key is unknown.
     */
    bool getToggleState(std::string_view key) const;

    /**
     * @brief Get a signal that is emitted when a specific toggle changes.
     */
    sigc::signal<void(bool)>& signalToggleChanged(std::string_view key);

    /**
     * @brief Get all toggle states (for persistence).
     */
    std::map<std::string, bool, std::less<>> const& allStates() const { return _states; }

    /**
     * @brief Load toggle states (for persistence).
     */
    void loadStates(std::map<std::string, bool, std::less<>> states);

  private:
    std::map<std::string, bool, std::less<>> _states;
    mutable std::map<std::string, sigc::signal<void(bool)>, std::less<>> _signals;
  };
} // namespace ao::gtk::layout
