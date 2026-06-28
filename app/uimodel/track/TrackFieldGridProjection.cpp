// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/uimodel/track/TrackFieldGridProjection.h>

#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel::track
{
  namespace
  {
    bool isSecondaryCompositeField(rt::TrackField const field)
    {
      return field == rt::TrackField::DiscTotal || field == rt::TrackField::TrackTotal ||
             field == rt::TrackField::MovementTotal;
    }

    std::optional<rt::TrackField> secondaryCompositeField(rt::TrackField const field)
    {
      switch (field)
      {
        case rt::TrackField::DiscNumber: return rt::TrackField::DiscTotal;
        case rt::TrackField::TrackNumber: return rt::TrackField::TrackTotal;
        case rt::TrackField::MovementNumber: return rt::TrackField::MovementTotal;
        default: return std::nullopt;
      }
    }

    void appendHeaderPart(std::string& summary, std::string_view const text)
    {
      if (text.empty())
      {
        return;
      }

      if (!summary.empty())
      {
        summary += " · ";
      }

      summary += text;
    }
  } // namespace

  TrackFieldGridProjection projectTrackFieldGrid(TrackFieldGridProjectionRequest const request)
  {
    auto projection = TrackFieldGridProjection{};

    for (auto const& def : rt::trackFieldDefinitions())
    {
      if (def.synthetic || def.category == rt::TrackFieldCategory::Tag || !def.presentable)
      {
        continue;
      }

      if (def.category == rt::TrackFieldCategory::Metadata)
      {
        if (!request.includeMetadata || isSecondaryCompositeField(def.field))
        {
          continue;
        }

        if (auto const optSecondary = secondaryCompositeField(def.field); optSecondary)
        {
          projection.compositeMetadataFields.push_back(
            TrackFieldGridCompositeFields{.primaryField = def.field, .secondaryField = *optSecondary});
        }
        else
        {
          projection.metadataFields.push_back(def.field);
        }
      }
      else if (def.category == rt::TrackFieldCategory::Technical && request.includeTechnical)
      {
        projection.technicalFields.push_back(def.field);
      }
    }

    return projection;
  }

  std::string formatTrackFieldGridMetadataHeader(std::string_view const titleText, std::string_view const artistText)
  {
    auto summary = std::string{};

    if (!titleText.empty())
    {
      summary += titleText;
    }

    if (!artistText.empty())
    {
      if (!summary.empty())
      {
        summary += " — ";
      }

      summary += artistText;
    }

    if (summary.empty())
    {
      return "Metadata";
    }

    return summary;
  }

  std::string formatTrackFieldGridTechnicalHeader(std::string_view const codecText,
                                                  std::string_view const sampleRateText,
                                                  std::string_view const bitDepthText)
  {
    auto summary = std::string{};
    appendHeaderPart(summary, codecText);
    appendHeaderPart(summary, sampleRateText);
    appendHeaderPart(summary, bitDepthText);

    if (summary.empty())
    {
      return "Audio Properties";
    }

    return summary;
  }
} // namespace ao::uimodel::track
