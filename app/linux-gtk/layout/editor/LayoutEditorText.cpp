// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LayoutEditorText.h"

#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <array>
#include <string>
#include <string_view>

namespace ao::gtk::layout::editor
{
  namespace
  {
    using i18n::MessageId;

    struct VocabularyEntry final
    {
      std::string_view source;
      MessageId messageId;
    };

    constexpr auto kVocabulary = std::to_array<VocabularyEntry>({
      {.source = "Containers", .messageId = MessageId::GtkLayoutCategoryContainers},
      {.source = "Decorators", .messageId = MessageId::GtkLayoutCategoryDecorators},
      {.source = "Tracks", .messageId = MessageId::GtkLayoutCategoryTracks},
      {.source = "Playback", .messageId = MessageId::GtkLayoutCategoryPlayback},
      {.source = "Status", .messageId = MessageId::GtkLayoutCategoryStatus},
      {.source = "Generic", .messageId = MessageId::GtkLayoutCategoryGeneric},
      {.source = "Application", .messageId = MessageId::GtkLayoutCategoryApplication},
      {.source = "Library", .messageId = MessageId::GtkLayoutCategoryLibrary},
      {.source = "Layout", .messageId = MessageId::GtkLayoutCategoryLayout},
      {.source = "Box", .messageId = MessageId::GtkLayoutComponentBox},
      {.source = "Split Pane", .messageId = MessageId::GtkLayoutComponentSplitPane},
      {.source = "Label", .messageId = MessageId::GtkLayoutComponentLabel},
      {.source = "Action Button", .messageId = MessageId::GtkLayoutComponentActionButton},
      {.source = "Menu Button", .messageId = MessageId::GtkLayoutComponentMenuButton},
      {.source = "Menu Bar", .messageId = MessageId::GtkLayoutComponentMenuBar},
      {.source = "Track Table", .messageId = MessageId::GtkLayoutComponentTrackTable},
      {.source = "Quick Filter", .messageId = MessageId::GtkLayoutComponentQuickFilter},
      {.source = "Presentation Button", .messageId = MessageId::GtkLayoutComponentPresentationButton},
      {.source = "Cover Art", .messageId = MessageId::GtkLayoutComponentCoverArt},
      {.source = "Transport Button", .messageId = MessageId::GtkLayoutComponentTransportButton},
      {.source = "Soul Button", .messageId = MessageId::GtkLayoutComponentSoulButton},
      {.source = "Seek Slider", .messageId = MessageId::GtkLayoutComponentSeekSlider},
      {.source = "Time Label", .messageId = MessageId::GtkLayoutComponentTimeLabel},
      {.source = "Volume Control", .messageId = MessageId::GtkLayoutComponentVolumeControl},
      {.source = "Output Device Selector", .messageId = MessageId::GtkLayoutComponentOutputDeviceSelector},
      {.source = "Activity Status", .messageId = MessageId::GtkLayoutComponentActivityStatus},
      {.source = "Track Count", .messageId = MessageId::GtkLayoutComponentTrackCount},
      {.source = "Selection Info", .messageId = MessageId::GtkLayoutComponentSelectionInfo},
      {.source = "Status Message", .messageId = MessageId::GtkLayoutComponentStatusMessage},
      {.source = "Now Playing Status", .messageId = MessageId::GtkLayoutComponentNowPlayingStatus},
      {.source = "Playback Details", .messageId = MessageId::GtkLayoutComponentPlaybackDetails},
      {.source = "Library Tree", .messageId = MessageId::GtkLayoutComponentLibraryTree},
      {.source = "Playback Cover Art", .messageId = MessageId::GtkLayoutComponentPlaybackCoverArt},
      {.source = "Soul Play/Pause Button", .messageId = MessageId::GtkLayoutComponentSoulPlayPause},
      {.source = "Quality Indicator", .messageId = MessageId::GtkLayoutComponentQualityIndicator},
      {.source = "Audio Pipeline Panel", .messageId = MessageId::GtkLayoutComponentAudioPipelinePanel},
      {.source = "Current Title Label", .messageId = MessageId::GtkLayoutComponentCurrentTitleLabel},
      {.source = "Current Artist Label", .messageId = MessageId::GtkLayoutComponentCurrentArtistLabel},
      {.source = "Separator", .messageId = MessageId::GtkLayoutComponentSeparator},
      {.source = "Absolute Canvas", .messageId = MessageId::GtkLayoutComponentAbsoluteCanvas},
      {.source = "Spacer", .messageId = MessageId::GtkLayoutComponentSpacer},
      {.source = "Scroll Window", .messageId = MessageId::GtkLayoutComponentScrollWindow},
      {.source = "Tabs", .messageId = MessageId::GtkLayoutComponentTabs},
      {.source = "Responsive Class", .messageId = MessageId::GtkLayoutComponentResponsiveClass},
      {.source = "Center Box", .messageId = MessageId::GtkLayoutComponentCenterBox},
      {.source = "Open Library Button", .messageId = MessageId::GtkLayoutComponentOpenLibraryButton},
      {.source = "Workspace with Detail", .messageId = MessageId::GtkLayoutComponentWorkspaceWithDetail},
      {.source = "Collapsible Split", .messageId = MessageId::GtkLayoutComponentCollapsibleSplit},
      {.source = "Detail Scope", .messageId = MessageId::GtkLayoutComponentDetailScope},
      {.source = "Field Grid", .messageId = MessageId::GtkLayoutComponentFieldGrid},
      {.source = "Tag Editor", .messageId = MessageId::GtkLayoutComponentTagEditor},
      {.source = "Selection Region", .messageId = MessageId::GtkLayoutComponentSelectionRegion},
      {.source = "Detail Undo Bar", .messageId = MessageId::GtkLayoutComponentDetailUndoBar},
      {.source = "Text", .messageId = MessageId::GtkLayoutPropertyText},
      {.source = "Orientation", .messageId = MessageId::GtkLayoutPropertyOrientation},
      {.source = "Spacing", .messageId = MessageId::GtkLayoutPropertySpacing},
      {.source = "Variant", .messageId = MessageId::GtkLayoutPropertyVariant},
      {.source = "Placeholder Style", .messageId = MessageId::GtkLayoutPropertyPlaceholderStyle},
      {.source = "Command", .messageId = MessageId::GtkLayoutPropertyCommand},
      {.source = "Stroke Width", .messageId = MessageId::GtkLayoutPropertyStrokeWidth},
      {.source = "Glyph Scale", .messageId = MessageId::GtkLayoutPropertyGlyphScale},
      {.source = "Mode", .messageId = MessageId::GtkLayoutPropertyMode},
      {.source = "Idle Behavior", .messageId = MessageId::GtkLayoutPropertyIdleBehavior},
      {.source = "Max Text Chars", .messageId = MessageId::GtkLayoutPropertyMaxTextChars},
      {.source = "Glyph", .messageId = MessageId::GtkLayoutPropertyGlyph},
      {.source = "Show Full Logo", .messageId = MessageId::GtkLayoutPropertyShowFullLogo},
      {.source = "Target Size", .messageId = MessageId::GtkLayoutPropertyTargetSize},
      {.source = "Force Square", .messageId = MessageId::GtkLayoutPropertyForceSquare},
      {.source = "Action", .messageId = MessageId::GtkLayoutPropertyAction},
      {.source = "Show Label", .messageId = MessageId::GtkLayoutPropertyShowLabel},
      {.source = "Size", .messageId = MessageId::GtkLayoutPropertySize},
      {.source = "Icon (Symbolic)", .messageId = MessageId::GtkLayoutPropertyIconSymbolic},
      {.source = "Style", .messageId = MessageId::GtkLayoutPropertyStyle},
      {.source = "Homogeneous", .messageId = MessageId::GtkLayoutPropertyHomogeneous},
      {.source = "Snap To Grid", .messageId = MessageId::GtkLayoutPropertySnapToGrid},
      {.source = "Grid Size", .messageId = MessageId::GtkLayoutPropertyGridSize},
      {.source = "H. Scroll Policy", .messageId = MessageId::GtkLayoutPropertyHorizontalScrollPolicy},
      {.source = "V. Scroll Policy", .messageId = MessageId::GtkLayoutPropertyVerticalScrollPolicy},
      {.source = "Min Content Width", .messageId = MessageId::GtkLayoutPropertyMinContentWidth},
      {.source = "Min Content Height", .messageId = MessageId::GtkLayoutPropertyMinContentHeight},
      {.source = "Propagate Nat. Width", .messageId = MessageId::GtkLayoutPropertyPropagateNaturalWidth},
      {.source = "Propagate Nat. Height", .messageId = MessageId::GtkLayoutPropertyPropagateNaturalHeight},
      {.source = "Tab Title", .messageId = MessageId::GtkLayoutPropertyTabTitle},
      {.source = "Tab Icon", .messageId = MessageId::GtkLayoutPropertyTabIcon},
      {.source = "Axis", .messageId = MessageId::GtkLayoutPropertyAxis},
      {.source = "Compact Max", .messageId = MessageId::GtkLayoutPropertyCompactMax},
      {.source = "Regular Max", .messageId = MessageId::GtkLayoutPropertyRegularMax},
      {.source = "Class Prefix", .messageId = MessageId::GtkLayoutPropertyClassPrefix},
      {.source = "Slot", .messageId = MessageId::GtkLayoutPropertySlot},
      {.source = "View Source", .messageId = MessageId::GtkLayoutPropertyViewSource},
      {.source = "Group Cover Placeholder", .messageId = MessageId::GtkLayoutPropertyGroupCoverPlaceholder},
      {.source = "Position", .messageId = MessageId::GtkLayoutPropertyPosition},
      {.source = "Initial Position (%)", .messageId = MessageId::GtkLayoutPropertyInitialPosition},
      {.source = "Resize Start", .messageId = MessageId::GtkLayoutPropertyResizeStart},
      {.source = "Shrink Start", .messageId = MessageId::GtkLayoutPropertyShrinkStart},
      {.source = "Resize End", .messageId = MessageId::GtkLayoutPropertyResizeEnd},
      {.source = "Shrink End", .messageId = MessageId::GtkLayoutPropertyShrinkEnd},
      {.source = "Collapse Side", .messageId = MessageId::GtkLayoutPropertyCollapseSide},
      {.source = "Initially Revealed", .messageId = MessageId::GtkLayoutPropertyInitiallyRevealed},
      {.source = "CSS Classes", .messageId = MessageId::GtkLayoutPropertyCssClasses},
      {.source = "Categories", .messageId = MessageId::GtkLayoutPropertyCategories},
      {.source = "Show When", .messageId = MessageId::GtkLayoutPropertyShowWhen},
      {.source = "Show Placeholder", .messageId = MessageId::GtkLayoutPropertyShowPlaceholder},
      {.source = "Expand Horizontal", .messageId = MessageId::GtkLayoutPropertyExpandHorizontal},
      {.source = "Expand Vertical", .messageId = MessageId::GtkLayoutPropertyExpandVertical},
      {.source = "Horizontal Align", .messageId = MessageId::GtkLayoutPropertyHorizontalAlign},
      {.source = "Vertical Align", .messageId = MessageId::GtkLayoutPropertyVerticalAlign},
      {.source = "Width Request", .messageId = MessageId::GtkLayoutPropertyWidthRequest},
      {.source = "Height Request", .messageId = MessageId::GtkLayoutPropertyHeightRequest},
      {.source = "X", .messageId = MessageId::GtkLayoutPropertyX},
      {.source = "Y", .messageId = MessageId::GtkLayoutPropertyY},
      {.source = "Width", .messageId = MessageId::GtkLayoutPropertyWidth},
      {.source = "Height", .messageId = MessageId::GtkLayoutPropertyHeight},
      {.source = "Z-Index", .messageId = MessageId::GtkLayoutPropertyZIndex},
      {.source = "ambient", .messageId = MessageId::GtkLayoutValueAmbient},
      {.source = "classicInline", .messageId = MessageId::GtkLayoutValueClassicInline},
      {.source = "hidden", .messageId = MessageId::GtkLayoutValueHidden},
      {.source = "reserve", .messageId = MessageId::GtkLayoutValueReserve},
      {.source = "small", .messageId = MessageId::GtkLayoutValueSmall},
      {.source = "normal", .messageId = MessageId::GtkLayoutValueNormal},
      {.source = "large", .messageId = MessageId::GtkLayoutValueLarge},
      {.source = "flat", .messageId = MessageId::GtkLayoutValueFlat},
      {.source = "raised", .messageId = MessageId::GtkLayoutValueRaised},
      {.source = "circular", .messageId = MessageId::GtkLayoutValueCircular},
      {.source = "suggested", .messageId = MessageId::GtkLayoutValueSuggested},
      {.source = "destructive", .messageId = MessageId::GtkLayoutValueDestructive},
      {.source = "monogram", .messageId = MessageId::GtkLayoutValueMonogram},
      {.source = "note", .messageId = MessageId::GtkLayoutValueNote},
      {.source = "vinyl", .messageId = MessageId::GtkLayoutValueVinyl},
      {.source = "equalizer", .messageId = MessageId::GtkLayoutValueEqualizer},
      {.source = "soul", .messageId = MessageId::GtkLayoutValueSoul},
      {.source = "sigil", .messageId = MessageId::GtkLayoutValueSigil},
      {.source = "seal", .messageId = MessageId::GtkLayoutValueSeal},
      {.source = "horizontal", .messageId = MessageId::GtkLayoutValueHorizontal},
      {.source = "vertical", .messageId = MessageId::GtkLayoutValueVertical},
      {.source = "jumpToAlbum", .messageId = MessageId::GtkLayoutValueJumpToAlbum},
      {.source = "inline", .messageId = MessageId::GtkLayoutValueInline},
      {.source = "compact", .messageId = MessageId::GtkLayoutValueCompact},
      {.source = "tooltip", .messageId = MessageId::GtkLayoutValueTooltip},
      {.source = "reveal", .messageId = MessageId::GtkLayoutValueReveal},
      {.source = "playPause", .messageId = MessageId::GtkLayoutValuePlayPause},
      {.source = "filterByField", .messageId = MessageId::GtkLayoutValueFilterByField},
      {.source = "start", .messageId = MessageId::GtkLayoutValueStart},
      {.source = "end", .messageId = MessageId::GtkLayoutValueEnd},
      {.source = "automatic", .messageId = MessageId::GtkLayoutValueAutomatic},
      {.source = "always", .messageId = MessageId::GtkLayoutValueAlways},
      {.source = "never", .messageId = MessageId::GtkLayoutValueNever},
      {.source = "width", .messageId = MessageId::GtkLayoutValueWidth},
      {.source = "height", .messageId = MessageId::GtkLayoutValueHeight},
      {.source = "fill", .messageId = MessageId::GtkLayoutValueFill},
      {.source = "center", .messageId = MessageId::GtkLayoutValueCenter},
    });
  } // namespace

  std::string layoutEditorVocabularyText(uimodel::PresentationTextCatalog const& textCatalog,
                                         std::string_view const sourceText)
  {
    if (sourceText.empty())
    {
      return std::string{textCatalog.text(MessageId::GtkLayoutNone)};
    }

    for (auto const& entry : kVocabulary)
    {
      if (entry.source == sourceText)
      {
        return std::string{textCatalog.text(entry.messageId)};
      }
    }

    return std::string{sourceText};
  }
} // namespace ao::gtk::layout::editor
