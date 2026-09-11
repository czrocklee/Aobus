// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TrackPropertiesEditorTestSupport.h"

#include "RenderTestSupport.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "tui/TrackPropertiesEditor.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    rt::TrackFieldRawValue textRaw(std::string value)
    {
      return rt::TrackFieldRawValue{std::in_place_type<std::string>, std::move(value)};
    }

    rt::TrackFieldRawValue numberRaw(std::uint16_t const value)
    {
      return rt::TrackFieldRawValue{std::in_place_type<std::uint16_t>, value};
    }

    /**
     * @brief A preparation built the way the real one will be: every spec field
     *        is loaded from the first target, then merged from the rest.
     */
    TrackEditorPreparation makePreparation(std::vector<TrackFixture> const& tracks,
                                           std::vector<std::pair<std::string, std::size_t>> tagCounts,
                                           std::vector<std::string> tagSuggestions,
                                           i18n::MessageCatalog const& textCatalog)
    {
      auto const spec = uimodel::buildTrackPropertiesFormSpec(textCatalog);
      auto preparation = TrackEditorPreparation{
        .baseline = uimodel::TrackPropertiesFormModel{textCatalog},
        .tagCounts = std::move(tagCounts),
        .tagSuggestions = std::move(tagSuggestions),
      };

      for (auto const& row : spec.metadataRows)
      {
        preparation.baseline.addField(row.field, true);
      }

      for (auto const& row : spec.propertyRows)
      {
        preparation.baseline.addField(row.field, false);
      }

      auto rawFor = [](TrackFixture const& track, rt::TrackField const field)
      {
        switch (field)
        {
          case rt::TrackField::Title: return textRaw(track.title);
          case rt::TrackField::Album: return textRaw(track.album);
          case rt::TrackField::Year: return numberRaw(track.year);
          case rt::TrackField::Codec: return textRaw(track.codec);
          default: return rt::TrackFieldRawValue{};
        }
      };

      for (std::size_t index = 0; index < tracks.size(); ++index)
      {
        auto const& track = tracks[index];

        preparation.targets.push_back(TrackEditorTarget{
          .id = TrackId{static_cast<std::uint32_t>(index + 1)},
          .title = track.title,
          .path = "/music/" + track.title + ".flac",
        });

        for (auto const* rows : {&spec.metadataRows, &spec.propertyRows})
        {
          for (auto const& row : *rows)
          {
            if (index == 0)
            {
              preparation.baseline.loadFirstTrackField(row.field, rawFor(track, row.field));
            }
            else
            {
              preparation.baseline.tryMergeTrackField(row.field, rawFor(track, row.field));
            }
          }
        }
      }

      return preparation;
    }
  } // namespace

  TrackPropertiesEditor makeEditor(std::vector<TrackFixture> tracks,
                                   TrackPropertiesEditor::CompletionProvider completionProvider,
                                   std::vector<std::pair<std::string, std::size_t>> tagCounts,
                                   std::vector<std::string> tagSuggestions,
                                   std::string_view const locale,
                                   TrackEditorMode const mode)
  {
    auto const textCatalog = ao::test::messageCatalog(locale);
    return TrackPropertiesEditor{textCatalog,
                                 makePreparation(tracks, std::move(tagCounts), std::move(tagSuggestions), textCatalog),
                                 std::move(completionProvider),
                                 mode};
  }

  std::string frame(TrackPropertiesEditor const& editor)
  {
    return renderElement(editor.render(), kTerminalColumns, kTerminalRows).text;
  }

  /// FTXUI delivers Ctrl-chords as their C0 control characters.
  ftxui::Event applyEvent()
  {
    return ftxui::Event::Character(static_cast<char>(0x13));
  }

  ftxui::Event reloadEvent()
  {
    return ftxui::Event::Character(static_cast<char>(0x12));
  }

  ftxui::Event clearEvent()
  {
    return ftxui::Event::CtrlD;
  }

  ftxui::Event restoreEvent()
  {
    return ftxui::Event::Character(static_cast<char>(0x07));
  }

  ftxui::Event completeEvent()
  {
    return ftxui::Event::Character(static_cast<char>(0x0e));
  }

  void focusRow(TrackPropertiesEditor& editor, std::string_view const label)
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto const spec = uimodel::buildTrackPropertiesFormSpec(textCatalog);
    auto const target =
      std::ranges::find_if(spec.metadataRows, [label](auto const& row) { return row.label == label; });
    REQUIRE(target != spec.metadataRows.end());
    auto const targetIndex = static_cast<std::size_t>(target - spec.metadataRows.begin());

    for (std::size_t step = 0; step < spec.metadataRows.size(); ++step)
    {
      editor.tryHandleEvent(ftxui::Event::ArrowUp);
    }

    for (std::size_t step = 0; step < targetIndex; ++step)
    {
      editor.tryHandleEvent(ftxui::Event::ArrowDown);
    }
  }

  void selectTab(TrackPropertiesEditor& editor, TrackEditorTab const tab)
  {
    for (std::size_t i = 0; i < 4 && editor.tab() != tab; ++i)
    {
      editor.tryHandleEvent(ftxui::Event::Tab);
    }

    REQUIRE(editor.tab() == tab);
  }

  void typeText(TrackPropertiesEditor& editor, std::string_view const text)
  {
    editor.tryHandleEvent(ftxui::Event::Character(std::string{text}));
  }

  /// Whether @p line carries an inverted cell, which is how an input draws its caret.
  bool hasCaretOnLine(ftxui::Screen const& screen, std::int32_t const line)
  {
    for (std::int32_t column = 0; column < screen.dimx(); ++column)
    {
      if (screen.PixelAt(column, line).inverted)
      {
        return true;
      }
    }

    return false;
  }
} // namespace ao::tui::test
