// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "QueryHelp.h"

#include <ao/query/Completion.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace ao::cli
{
  namespace
  {
    std::string variableText(query::VariableType type, std::string_view name)
    {
      auto text = std::string{};
      text.push_back(query::variablePrefix(type));
      text += name;
      return text;
    }

    std::string fieldSummaryText(query::QueryVariableSummary const& summary)
    {
      auto text = variableText(summary.type, summary.canonicalName);

      if (!summary.aliases.empty())
      {
        text += "(";

        for (std::size_t index = 0; index < summary.aliases.size(); ++index)
        {
          if (index > 0)
          {
            text += ",";
          }

          text += variableText(summary.type, summary.aliases[index]);
        }

        text += ")";
      }

      return text;
    }

    void appendSummaries(std::string& text, query::VariableType type)
    {
      for (auto const& summary : query::queryVariableSummaries(type))
      {
        if (!text.empty())
        {
          text += " ";
        }

        text += fieldSummaryText(summary);
      }
    }

    std::string systemFieldList()
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
    hint += systemFieldList();
    hint += "; #tag for tags; %customKey for custom metadata";
    return hint;
  }

  std::string formatExpressionUsageHint()
  {
    auto hint = std::string{"\nhint: format expressions look like: $artist + \" - \" + $title\n      fields: "};
    hint += systemFieldList();
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
    footer += systemFieldList();
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
