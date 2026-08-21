// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao::rt
{
  struct ImportReport;
}

namespace ao::gtk
{
  enum class GtkTextId : std::uint8_t
  {
    MenuFile,
    OpenLibrary,
    ScanLibrary,
    ImportLibraryData,
    ExportLibraryData,
    Quit,
    MenuEdit,
    Preferences,
    MenuView,
    EditLayout,
    SavePanelSizesAsLayoutDefaults,
    ResetRuntimeLayoutState,
    MenuHelp,
    About,
    ApplicationMenu,
    PlaybackShowPlayingList,
    PlaybackToggleMute,
    PlaybackShowCurrentAlbum,
    PlaybackNowPlayingCoverArt,
    LibraryQuickFilterPlaceholder,
    LibraryClearFilter,
    LibraryCreateListFromFilter,
    ListNew,
    ListNewPlaylist,
    ListEdit,
    ListDelete,
    ListDeleteSubtree,
    SmartListNewTitle,
    SmartListEditTitle,
    SmartListNewPlaylistTitle,
    CommonCancel,
    CommonCreate,
    CommonSave,
    CommonClose,
    SmartListNamePlaceholder,
    SmartListName,
    SmartListDescriptionPlaceholder,
    SmartListDescription,
    SmartListMembershipTagPlaceholder,
    SmartListMembershipTag,
    SmartListInheritedFilter,
    SmartListFilterExpressionPlaceholder,
    SmartListLocalFilter,
    SmartListEffectiveFilter,
    SmartListMembership,
    SmartListAutoPresentation,
    SmartListPresentation,
    SmartListPreview,
    SmartListWaitingForFilter,
    SmartListTrack,
    SmartListInvalidSource,
    SmartListChooseMembershipTag,
    LibraryOpenMusicLibrary,
    LibrarySelectExportMode,
    LibraryChooseBackupContents,
    LibraryExportModeDelta,
    LibraryExportModeMetadata,
    LibraryExportModeFull,
    LibraryExportModeListOnly,
    LibraryInclude,
    LibraryNext,
    LibraryExportYaml,
    LibraryYamlFiles,
    LibraryImportYaml,
    LibraryConfirmRestore,
    LibraryRestoreLibrary,
    LibraryRestoreLists,
    LibraryRestoreScopeLibrary,
    LibraryRestoreScopeLists,
    LibraryFileSelectionFailed,
    LibraryCouldNotSelectMusicFolder,
    LibraryCouldNotSelectExportFile,
    LibraryCouldNotSelectBackup,
    ListDeleteQuestionTitle,
    ListDeleteSubtreeTitle,
    ListDeleteAction,
    ListDeleteAllAction,
    ListUnableToDeleteTitle,
    ListOtherComputedMembership,
    ListNoEditablePlaylists,
    ListCreatePlaylist,
    ListManageLists,
    ListAddToPlaylist,
    ListRemoveComputedUnavailable,
    ListMoveUp,
    ListMoveDown,
    ListMoveToTop,
    ListMoveToBottom,
    ListResetOrder,
    ListForgetHiddenPositions,
    ListManualOrder,
    ListSelectTracksForOrder,
    ListMoveUpAction,
    ListMoveDownAction,
    ListMoveToTopAction,
    ListMoveToBottomAction,
    ListResetOrderAction,
    Count,
  };

  /** GTK-local copy resolved once from the process catalog. */
  class GtkTextCatalog final
  {
  public:
    explicit GtkTextCatalog(i18n::MessageCatalog const& catalog);

    std::string const& text(GtkTextId id) const noexcept;
    std::string deleteListQuestion(std::string_view name) const;
    std::string deleteSubtreeQuestion(std::size_t count, std::string_view entries) const;
    std::string removeMembershipTagQuestion(std::string_view tag, std::size_t count) const;
    std::string membershipTagReferencesWarning(std::string_view tag, std::string_view references) const;
    std::string removeFromCurrentList(std::string_view name, std::string_view tag) const;
    std::string libraryRestoreConfirmation(rt::ImportReport const& report) const;
    std::string fileSelectionError(std::string_view operation, std::string_view message) const;

  private:
    i18n::MessageCatalog _catalog;
    std::array<std::string, static_cast<std::size_t>(GtkTextId::Count)> _text;
  };
} // namespace ao::gtk
