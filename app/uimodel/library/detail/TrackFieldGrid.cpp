// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/detail/TrackFieldGrid.h>

#include <ao/rt/TrackField.h>

#include <array>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  namespace
  {
    constexpr auto kCompositeMetadataFields = std::to_array<TrackFieldGridCompositeFields>({
      {.primaryField = rt::TrackField::DiscNumber, .secondaryField = rt::TrackField::DiscTotal},
      {.primaryField = rt::TrackField::TrackNumber, .secondaryField = rt::TrackField::TrackTotal},
      {.primaryField = rt::TrackField::MovementNumber, .secondaryField = rt::TrackField::MovementTotal},
    });

    void appendMetadataField(TrackFieldGridSchema& schema, rt::TrackField field)
    {
      for (auto const& composite : kCompositeMetadataFields)
      {
        if (field == composite.primaryField)
        {
          schema.compositeMetadataFields.push_back(composite);
          return;
        }

        if (field == composite.secondaryField)
        {
          return;
        }
      }

      schema.metadataFields.push_back(field);
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

  TrackFieldGridSchema buildTrackFieldGridSchema(TrackFieldGridSchemaOptions const options)
  {
    auto projection = TrackFieldGridSchema{};

    for (auto const& def : rt::trackFieldDefinitions())
    {
      if (def.synthetic || def.category == rt::TrackFieldCategory::Tag || !def.presentable)
      {
        continue;
      }

      if (def.category == rt::TrackFieldCategory::Metadata && options.includeMetadata)
      {
        appendMetadataField(projection, def.field);
      }
      else if (def.category == rt::TrackFieldCategory::Technical && options.includeTechnical)
      {
        projection.technicalFields.push_back(def.field);
      }
    }

    return projection;
  }

  std::string formatMetadataHeader(std::string_view const titleText, std::string_view const artistText)
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

    return summary;
  }

  std::string formatTechnicalHeader(std::string_view const codecText,
                                    std::string_view const sampleRateText,
                                    std::string_view const bitDepthText)
  {
    auto summary = std::string{};
    appendHeaderPart(summary, codecText);
    appendHeaderPart(summary, sampleRateText);
    appendHeaderPart(summary, bitDepthText);

    return summary;
  }
} // namespace ao::uimodel
