// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "QueryHelp.h"

#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FieldCatalog.h>

#include <cstddef>
#include <string>

namespace ao::cli
{
  namespace
  {
    std::string fieldSummaryText(query::QueryVariableDescriptor const& descriptor)
    {
      auto text = query::variableDisplayName(descriptor.type, descriptor.canonicalName);

      if (!descriptor.aliases.empty())
      {
        text += "(";

        for (std::size_t index = 0; index < descriptor.aliases.size(); ++index)
        {
          if (index > 0)
          {
            text += ",";
          }

          text += query::variableDisplayName(descriptor.type, descriptor.aliases[index]);
        }

        text += ")";
      }

      return text;
    }

    void appendSummaries(std::string& text, query::VariableType type)
    {
      for (auto const& descriptor : query::queryVariableDescriptors(type))
      {
        if (!text.empty())
        {
          text += " ";
        }

        text += fieldSummaryText(descriptor);
      }
    }

    std::string systemFieldSummaryText()
    {
      auto text = std::string{};
      appendSummaries(text, query::VariableType::Metadata);
      appendSummaries(text, query::VariableType::Property);
      return text;
    }
  } // namespace

  std::string queryFilterUsageHint()
  {
    auto hint = std::string{"\nhint: expressions look like: $genre = \"Rock\" | not $genre? | $year in 1990..1999 | "
                            "$title ~ \"love\" and $artist = \"Miles Davis\"\n      fields: "};
    hint += systemFieldSummaryText();
    hint += "; #tag for tags; %customKey for custom metadata";
    return hint;
  }

  std::string formatExpressionUsageHint()
  {
    auto hint = std::string{"\nhint: format expressions look like: $artist + \" - \" + $title\n      fields: "};
    hint += systemFieldSummaryText();
    hint += "; %customKey for custom metadata";
    return hint;
  }

  std::string trackShowHelpFooter()
  {
    auto footer = std::string{"\nQuery language:\n"
                              "  Variables: $metadata, @property, #tag, %customKey\n"
                              "  Operators: = != ~ < <= > >= in .. ? and/&& or/|| not/!\n"
                              "  Missing field idiom: not $genre?\n"
                              "  Examples:\n"
                              "    aobus track show 1 2 3\n"
                              "    aobus track show --filter 'not $genre?'\n"
                              "    aobus track show --filter '$year in 1990..1999 and $artist ~ \"Miles\"'\n"
                              "    aobus -O json track show --filter 'not $genre?'\n"
                              "    aobus track show --format '$artist + \" - \" + $title'\n"
                              "  Fields: "};
    footer += systemFieldSummaryText();
    return footer;
  }

  std::string trackUpdateHelpFooter()
  {
    return "\nExamples:\n"
           "  aobus track update 12 13 --genre Jazz --dry-run\n"
           "  aobus track update --filter 'not $genre?' --genre Jazz";
  }

  std::string trackHelpFooter()
  {
    return "\nFilter fields, aliases, operators, and examples are listed in `aobus track show --help`.";
  }

  std::string listCreateHelpFooter()
  {
    return "\nSmart-list example:\n"
           "  aobus list create --name 'Missing Genre' --filter 'not $genre?'";
  }
} // namespace ao::cli
