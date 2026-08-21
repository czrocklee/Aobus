// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "i18n/GtkTextCatalog.h"

#include <ao/Contract.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    using i18n::MessageId;

    constexpr auto kMessageIds = std::to_array<MessageId>({
      MessageId::GtkShellMenuFile,
      MessageId::GtkShellOpenLibrary,
      MessageId::GtkShellScanLibrary,
      MessageId::GtkShellImportLibraryData,
      MessageId::GtkShellExportLibraryData,
      MessageId::GtkShellQuit,
      MessageId::GtkShellMenuEdit,
      MessageId::GtkShellPreferences,
      MessageId::GtkShellMenuView,
      MessageId::GtkShellEditLayout,
      MessageId::GtkShellSavePanelSizesAsLayoutDefaults,
      MessageId::GtkShellResetRuntimeLayoutState,
      MessageId::GtkShellMenuHelp,
      MessageId::GtkShellAbout,
      MessageId::GtkShellApplicationMenu,
      MessageId::GtkPlaybackShowPlayingList,
      MessageId::GtkPlaybackToggleMute,
      MessageId::GtkPlaybackShowCurrentAlbum,
      MessageId::GtkPlaybackNowPlayingCoverArt,
      MessageId::GtkLibraryQuickFilterPlaceholder,
      MessageId::GtkLibraryClearFilter,
      MessageId::GtkLibraryCreateListFromFilter,
      MessageId::GtkListNew,
      MessageId::GtkListNewPlaylist,
      MessageId::GtkListEdit,
      MessageId::GtkListDelete,
      MessageId::GtkListDeleteSubtree,
      MessageId::GtkSmartListNewTitle,
      MessageId::GtkSmartListEditTitle,
      MessageId::GtkSmartListNewPlaylistTitle,
      MessageId::GtkCommonCancel,
      MessageId::GtkCommonCreate,
      MessageId::GtkCommonSave,
      MessageId::GtkCommonClose,
      MessageId::GtkSmartListNamePlaceholder,
      MessageId::GtkSmartListName,
      MessageId::GtkSmartListDescriptionPlaceholder,
      MessageId::GtkSmartListDescription,
      MessageId::GtkSmartListMembershipTagPlaceholder,
      MessageId::GtkSmartListMembershipTag,
      MessageId::GtkSmartListInheritedFilter,
      MessageId::GtkSmartListFilterExpressionPlaceholder,
      MessageId::GtkSmartListLocalFilter,
      MessageId::GtkSmartListEffectiveFilter,
      MessageId::GtkSmartListMembership,
      MessageId::GtkSmartListAutoPresentation,
      MessageId::GtkSmartListPresentation,
      MessageId::GtkSmartListPreview,
      MessageId::GtkSmartListWaitingForFilter,
      MessageId::GtkSmartListTrack,
      MessageId::GtkSmartListInvalidSource,
      MessageId::GtkSmartListChooseMembershipTag,
      MessageId::GtkLibraryOpenMusicLibrary,
      MessageId::GtkLibrarySelectExportMode,
      MessageId::GtkLibraryChooseBackupContents,
      MessageId::GtkLibraryExportModeDelta,
      MessageId::GtkLibraryExportModeMetadata,
      MessageId::GtkLibraryExportModeFull,
      MessageId::GtkLibraryExportModeListOnly,
      MessageId::GtkLibraryInclude,
      MessageId::GtkLibraryNext,
      MessageId::GtkLibraryExportYaml,
      MessageId::GtkLibraryYamlFiles,
      MessageId::GtkLibraryImportYaml,
      MessageId::GtkLibraryConfirmRestore,
      MessageId::GtkLibraryRestoreLibrary,
      MessageId::GtkLibraryRestoreLists,
      MessageId::GtkLibraryRestoreScopeLibrary,
      MessageId::GtkLibraryRestoreScopeLists,
      MessageId::GtkLibraryFileSelectionFailed,
      MessageId::GtkLibraryCouldNotSelectMusicFolder,
      MessageId::GtkLibraryCouldNotSelectExportFile,
      MessageId::GtkLibraryCouldNotSelectBackup,
      MessageId::GtkListDeleteQuestionTitle,
      MessageId::GtkListDeleteSubtreeTitle,
      MessageId::GtkListDeleteAction,
      MessageId::GtkListDeleteAllAction,
      MessageId::GtkListUnableToDeleteTitle,
      MessageId::GtkListOtherComputedMembership,
      MessageId::GtkListNoEditablePlaylists,
      MessageId::GtkListCreatePlaylist,
      MessageId::GtkListManageLists,
      MessageId::GtkListAddToPlaylist,
      MessageId::GtkListRemoveComputedUnavailable,
      MessageId::GtkListMoveUp,
      MessageId::GtkListMoveDown,
      MessageId::GtkListMoveToTop,
      MessageId::GtkListMoveToBottom,
      MessageId::GtkListResetOrder,
      MessageId::GtkListForgetHiddenPositions,
      MessageId::GtkListManualOrder,
      MessageId::GtkListSelectTracksForOrder,
      MessageId::GtkListMoveUpAction,
      MessageId::GtkListMoveDownAction,
      MessageId::GtkListMoveToTopAction,
      MessageId::GtkListMoveToBottomAction,
      MessageId::GtkListResetOrderAction,
    });

    static_assert(kMessageIds.size() == static_cast<std::size_t>(GtkTextId::Count));

    std::string requiredMessage(i18n::MessageCatalog const& catalog,
                                i18n::MessageId const id,
                                std::initializer_list<i18n::MessageArgument> const arguments = {})
    {
      auto result = catalog.format(id, arguments);

      if (!result)
      {
        AO_FATAL("Could not format required GTK message: {}", result.error().message);
      }

      return std::move(result->text);
    }
  } // namespace

  GtkTextCatalog::GtkTextCatalog(i18n::MessageCatalog const& catalog)
    : _catalog{catalog}
  {
    for (std::size_t index = 0; index < _text.size(); ++index)
    {
      auto result = catalog.text(kMessageIds[index]);

      if (!result)
      {
        AO_FATAL("Could not resolve required GTK message: {}", result.error().message);
      }

      _text[index] = *result;
    }
  }

  std::string const& GtkTextCatalog::text(GtkTextId const id) const noexcept
  {
    static auto const kEmpty = std::string{};
    auto const index = static_cast<std::size_t>(id);
    return index < _text.size() ? _text[index] : kEmpty;
  }

  std::string GtkTextCatalog::deleteListQuestion(std::string_view const name) const
  {
    return requiredMessage(_catalog, MessageId::GtkListDeleteQuestion, {i18n::MessageArgument{"name", name}});
  }

  std::string GtkTextCatalog::deleteSubtreeQuestion(std::size_t const count, std::string_view const entries) const
  {
    return requiredMessage(_catalog,
                           MessageId::GtkListDeleteSubtreeQuestion,
                           {i18n::MessageArgument{"count", count}, i18n::MessageArgument{"entries", entries}});
  }

  std::string GtkTextCatalog::removeMembershipTagQuestion(std::string_view const tag, std::size_t const count) const
  {
    return requiredMessage(_catalog,
                           MessageId::GtkListRemoveTag,
                           {i18n::MessageArgument{"tag", tag}, i18n::MessageArgument{"count", count}});
  }

  std::string GtkTextCatalog::membershipTagReferencesWarning(std::string_view const tag,
                                                             std::string_view const references) const
  {
    return requiredMessage(_catalog,
                           MessageId::GtkListTagReferences,
                           {i18n::MessageArgument{"tag", tag}, i18n::MessageArgument{"references", references}});
  }

  std::string GtkTextCatalog::removeFromCurrentList(std::string_view const name, std::string_view const tag) const
  {
    return requiredMessage(_catalog,
                           MessageId::GtkListRemoveFromCurrent,
                           {i18n::MessageArgument{"name", name}, i18n::MessageArgument{"tag", tag}});
  }

  std::string GtkTextCatalog::libraryRestoreConfirmation(rt::ImportReport const& report) const
  {
    auto const scopeId = report.targetScope == rt::ImportTargetScope::Library ? GtkTextId::LibraryRestoreScopeLibrary
                                                                              : GtkTextId::LibraryRestoreScopeLists;
    return requiredMessage(_catalog,
                           MessageId::GtkLibraryRestoreConfirmation,
                           {i18n::MessageArgument{"scope", text(scopeId)},
                            i18n::MessageArgument{"version", report.payloadVersion},
                            i18n::MessageArgument{"mode", rt::exportModeName(report.payloadMode)},
                            i18n::MessageArgument{"tracksCreated", report.tracksCreated},
                            i18n::MessageArgument{"tracksUpdated", report.tracksUpdated},
                            i18n::MessageArgument{"tracksDeleted", report.tracksDeleted},
                            i18n::MessageArgument{"listsCreated", report.listsCreated},
                            i18n::MessageArgument{"listsDeleted", report.listsDeleted},
                            i18n::MessageArgument{"dangling", report.danglingReferencesIgnored}});
  }

  std::string GtkTextCatalog::fileSelectionError(std::string_view const operation, std::string_view const message) const
  {
    return requiredMessage(_catalog,
                           MessageId::GtkLibraryFileSelectionError,
                           {i18n::MessageArgument{"operation", operation}, i18n::MessageArgument{"message", message}});
  }
} // namespace ao::gtk
