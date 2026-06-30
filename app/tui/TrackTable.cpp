// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackTable.h"

#include "Model.h"
#include <ao/CoreIds.h>

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    ftxui::Element selectableRows(std::vector<std::string> const& labels,
                                  std::int32_t const selected,
                                  bool const active,
                                  std::string const& emptyText)
    {
      using namespace ftxui;

      if (labels.empty())
      {
        return vbox({text(emptyText) | dim}) | center;
      }

      auto rows = Elements{};
      rows.reserve(labels.size());

      for (std::size_t index = 0; index < labels.size(); ++index)
      {
        auto rowPtr = text(labels[index]) | flex;

        if (std::cmp_equal(index, selected))
        {
          rowPtr = rowPtr | inverted;

          if (active)
          {
            rowPtr = rowPtr | bold;
          }
        }

        rows.push_back(rowPtr);
      }

      return vbox(std::move(rows)) | focusPosition(0, std::max(0, selected)) | vscroll_indicator | frame | flex;
    }
  } // namespace

  ftxui::Element trackTableView(std::vector<TrackListItem> const& tracks,
                                std::int32_t const selected,
                                TrackId const playingTrackId)
  {
    using namespace ftxui;

    auto labels = std::vector<std::string>{};
    labels.reserve(tracks.size());

    for (auto const& track : tracks)
    {
      auto label = track.label;

      if (track.id == playingTrackId)
      {
        label.insert(0, "> ");
      }
      else
      {
        label.insert(0, "  ");
      }

      labels.push_back(std::move(label));
    }

    return vbox({
             text("#   Title  Artist  Album") | dim,
             selectableRows(labels, selected, true, "No tracks found. Run `aobus init` in this library first."),
           }) |
           flex;
  }

  ftxui::Element libraryChooserPane(std::vector<std::string> const& labels, std::int32_t const selected)
  {
    constexpr int kChooserColumns = 34;
    using namespace ftxui;

    return vbox({
             text("Lists") | bold,
             separator(),
             selectableRows(labels, selected, true, "No lists found"),
             separator(),
             text("Enter open  Esc close") | dim,
           }) |
           border | size(WIDTH, EQUAL, kChooserColumns);
  }
} // namespace ao::tui
