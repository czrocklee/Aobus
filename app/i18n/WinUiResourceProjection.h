// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <array>
#include <string_view>

namespace ao::i18n::detail
{
  /** Names canonical messages whose WinUI projection uses positional formatting. */
  struct WinUiPositionalResource final
  {
    std::string_view messageId;
    std::string_view argumentName;
  };

  inline constexpr auto kWinUiPositionalResources = std::to_array<WinUiPositionalResource>({
    {.messageId = "winui_error", .argumentName = "detail"},
    {.messageId = "winui_save_settings_failed", .argumentName = "detail"},
    {.messageId = "winui_folder_picker_failed", .argumentName = "detail"},
    {.messageId = "winui_library_switch_failed", .argumentName = "detail"},
    {.messageId = "winui_theme_reload_failed", .argumentName = "detail"},
    {.messageId = "winui_theme_reloaded", .argumentName = "path"},
    {.messageId = "winui_smtc_unavailable", .argumentName = "detail"},
    {.messageId = "winui_shell_layout_failed", .argumentName = "detail"},
    {.messageId = "winui_library_ready_at", .argumentName = "root"},
    {.messageId = "winui_navigation_failed", .argumentName = "detail"},
    {.messageId = "winui_list_status", .argumentName = "list"},
    {.messageId = "winui_presentation_failed", .argumentName = "detail"},
    {.messageId = "winui_playback_failed", .argumentName = "detail"},
    {.messageId = "winui_column_resize_failed", .argumentName = "detail"},
    {.messageId = "winui_column_settings_failed", .argumentName = "detail"},
    {.messageId = "winui_column_move_failed", .argumentName = "detail"},
    {.messageId = "winui_column_visibility_failed", .argumentName = "detail"},
    {.messageId = "winui_column_not_sortable", .argumentName = "column"},
    {.messageId = "winui_sort_failed", .argumentName = "detail"},
    {.messageId = "track_fallback", .argumentName = "id"},
    {.messageId = "winui_theme_file_not_found", .argumentName = "path"},
    {.messageId = "winui_theme_load_failed", .argumentName = "detail"},
    {.messageId = "winui_startup_failure", .argumentName = "detail"},
  });

  /** Maps canonical messages to the property-qualified ids required by XAML x:Uid. */
  struct WinUiResourceAlias final
  {
    std::string_view messageId;
    std::string_view resourceId;
  };

  inline constexpr auto kWinUiResourceAliases = std::to_array<WinUiResourceAlias>({
    {.messageId = "winui_track_move_column_left", .resourceId = "winui_track_move_column_left_button.Text"},
    {.messageId = "winui_track_move_column_right", .resourceId = "winui_track_move_column_right_button.Text"},
  });

  /** English-only WinUI resources outside the shared presentation catalog. */
  struct WinUiEnglishResource final
  {
    std::string_view resourceId;
    std::string_view text;
  };

  inline constexpr auto kWinUiEnglishResources = std::to_array<WinUiEnglishResource>({
    {.resourceId = "AppTitleValue", .text = "Aobus"},
    {.resourceId = "SoulWindowTitle", .text = "Aobus Soul"},
    {.resourceId = "SortAscendingSuffix", .text = " ↑"},
    {.resourceId = "SortDescendingSuffix", .text = " ↓"},
    {.resourceId = "NoListActive", .text = "No Windows list is active"},
    {.resourceId = "NoResizableColumn", .text = "No resizable Windows track column is active"},
    {.resourceId = "ColumnHidden", .text = "The requested Windows track column is hidden"},
    {.resourceId = "NoMovableColumn", .text = "No movable Windows track column is active"},
    {.resourceId = "NoConfigurableColumn", .text = "No configurable Windows track column is active"},
    {.resourceId = "ColumnOutsidePresentation",
     .text = "The requested Windows track column is not part of this presentation"},
    {.resourceId = "OneVisibleColumnRequired", .text = "A track presentation must keep at least one visible column"},
    {.resourceId = "ColumnLayoutPolicyMissing", .text = "The requested track column has no layout policy"},
    {.resourceId = "NoPlayableTrackRow", .text = "No playable Windows track row is active"},
    {.resourceId = "NoTrackViewActive", .text = "No Windows track view is active"},
    {.resourceId = "UnknownPresentationFormat", .text = "Unknown track presentation '{0}'"},
    {.resourceId = "StartupHresultDetailFormat", .text = "{0} (HRESULT 0x{1:08X})"},
  });
} // namespace ao::i18n::detail
