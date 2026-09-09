// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CommandCompletionState.h"
#include "Keymap.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/completion/CompletionResult.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::tui
{
  enum class Overlay : std::uint8_t
  {
    None,
    ListChooser,
    DetailPanel,
    QualityPanel,
    OutputDevices,
    PresentationPanel,
    Notifications,
    Help,
  };

  enum class ShellInputMode : std::uint8_t
  {
    None,
    QuickFilter,
    Command,
  };

  /**
   * @brief Whether @p overlay blocks interaction with the workspace beneath it.
   *
   * Separate from whether an overlay is on screen: Detail is a live inspector
   * that follows the track table while the user keeps browsing it, so it is
   * visible without being modal. Ask this when the question is "may the
   * workspace still be driven"; ask @ref ShellInteractionModel::overlay when
   * the question is "is another surface open".
   */
  bool isModalOverlay(Overlay overlay) noexcept;
  std::string_view overlayLabel(i18n::MessageCatalog const& textCatalog, Overlay overlay);
  /// The first binding that reaches an overlay's toggle after its fixed local protocol handles input.
  std::string_view overlayToggleShortcut(KeymapPlan const& keymapPlan, Overlay overlay);
  std::string overlayHint(i18n::MessageCatalog const& textCatalog, KeymapPlan const& keymapPlan, Overlay overlay);

  class ShellInteractionModel final
  {
  public:
    bool isInputActive() const noexcept;
    ShellInputMode inputMode() const noexcept;
    std::string const& inputDraft() const noexcept;
    bool isInputTouched() const noexcept;
    std::optional<rt::CompletionResult> const& commandCompletion() const noexcept;
    std::int32_t commandCompletionSelection() const noexcept;
    Overlay overlay() const noexcept;

    void beginInput(ShellInputMode mode, std::string draft = {});
    void appendInputText(std::string_view text);
    void backspaceInput();
    void closeInput();
    void setCommandCompletion(std::optional<rt::CompletionResult> optCompletion);
    bool tryMoveCommandCompletion(std::int32_t delta);
    bool tryMoveCommandCompletionByPage(std::int32_t delta);
    bool tryApplyCommandCompletion();
    void clearCommandCompletion();

    void openOverlay(Overlay overlay) noexcept;
    void closeOverlay() noexcept;

  private:
    ShellInputMode _inputMode = ShellInputMode::None;
    std::string _inputDraft{};
    bool _inputTouched = false;
    CommandCompletionState _completion{};
    Overlay _overlay = Overlay::None;
  };
} // namespace ao::tui
