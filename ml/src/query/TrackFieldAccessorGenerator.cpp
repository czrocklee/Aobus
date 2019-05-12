/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <flatbuffers/reflection.h>
#include "Generator.h"
#include "CppCodeWriter.h"
 
#include <boost/config.hpp> 

namespace rs::ml::query
{
  class TrackFieldAccessorGenerator : public gen::Generator
  {
    using CppCodeWriter = rs::ml::gen::CppCodeWriter;
    using Fields = gen::Fields;

  public:
    std::string name() const override { return "TrackFieldAccessorGenerator"; }

    std::string outputFile() const override { return "TrackFieldAccessor.h"; }

    void run(std::ostream& os, const Fields& meta, const Fields& prop) override
    {
      CppCodeWriter writer{os};

      writer | "// THIS IS A GENERATED FILE, DO NOT EDIT!!!"
             | ""
             | "#pragma once"
             | ""
             | "#include <rs/ml/core/Track.h>"
             | "#include <rs/ml/core/DataValue.h>"
             | "#include <unordered_map>"
             | ""
             | "namespace rs::ml::query::gen"
             | "{";
      
      {
        auto indent = writer.indent();

        writer | "class MetadataAccessor"
               | "{"
               | "public:";
                    generateName2IdFunction(writer, meta);
        writer | "";
                    generateTrackAccessor(writer, meta, "meta");
        writer | "";
                    generateTrackTAccessor(writer, meta, "meta");
        writer | "};"
               | "";

        writer | "class PropertyAccessor"
               | "{"
               | "public:";
                    generateName2IdFunction(writer, prop);
        writer | "";
                    generateTrackAccessor(writer, prop, "prop");
        writer | "";
                    generateTrackTAccessor(writer, prop, "prop");
        writer | "};";
      }

      writer | "}";
    }

  private:
    template<typename Fields>
    void generateName2IdFunction(CppCodeWriter& writer, const Fields& fields)
    {
      auto indent = writer.indent();

      writer | "static int name2Id(const std::string& name)"
             | "{"
             | "  static std::unordered_map<std::string, int> name2IdMap = "
             | "  {";

      for (auto i = 0u; i < fields.size(); ++i)
      {
        writer ( "    {\"%1%\", %2%},", fields[i]->name()->c_str(), fields[i]->id()); 
        auto attributes = fields[i]->attributes();

        if (attributes != nullptr)
        {
          auto alias = attributes->LookupByKey("alias");
        
          if (alias != nullptr)
          {
            writer ( "    {\"%1%\", %2%},", alias->value()->c_str(), fields[i]->id()); 
          }
        }
      }

      writer | "  };"
             | ""
             | "  return name2IdMap.at(name);"
             | "};";
    }


    template<typename Fields>
    void generateTrackAccessor(CppCodeWriter& writer, const Fields& fields, const char* name)
    {
      auto indent = writer.indent();

      writer | "static rs::ml::core::DataValue get(const rs::ml::fbs::Track* track, int fieldId)"
             | "{"
             | "  switch (fieldId)"
             | "  {";
        
      for (auto field : fields)
      {
        auto indent = writer.indent(2);

        if (field->type()->base_type() == ::reflection::String)
        {
          writer ( "case %1%:", field->id() )
                 | "{";
          writer ( "  auto val = track->%1%()->%2%();", name, field->name()->c_str() )
                 | "  return val == nullptr ? rs::ml::core::DataValue{} : std::string_view{val->c_str(), val->size()};"
                 | "}";
        }
        else if (flatbuffers::IsInteger(field->type()->base_type()))
        {
          writer ( "case %1%:", field->id() )
                 ( "  return {static_cast<std::int64_t>(track->%1%()->%2%())};", name, field->name()->c_str() );
        }
      }

      writer | "    default: "
             | "      return {};"
             | "  }"
             | "}";
    }

    void generateTrackTAccessor(CppCodeWriter& writer, const Fields& fields, const char* name)
    {
      auto indent = writer.indent();

      writer | "static rs::ml::core::DataValue get(const rs::ml::fbs::TrackT& track, int fieldId)"
             | "{"
             | "  switch (fieldId)"
             | "  {";
 
      for (auto field : fields)
      {
        auto indent = writer.indent(2);

        if (field->type()->base_type() == ::reflection::String)
        {
          writer ( "case %1%:", field->id() )
                 ( "  return track.%1%->%2%.empty() ? rs::ml::core::DataValue{} : std::string_view{track.%1%->%2%};", name, field->name()->c_str() );
        }
        else if (flatbuffers::IsInteger(field->type()->base_type()))
        {
          writer ( "case %1%:", field->id() )
                 ( "  return {static_cast<std::int64_t>(track.%1%->%2%)};", name, field->name()->c_str() );
        }
      }

      writer | "    default: "
             | "      return {};"
             | "  }"
             | "}";
    }
  };

  extern "C" BOOST_SYMBOL_EXPORT TrackFieldAccessorGenerator generator;
  TrackFieldAccessorGenerator generator;

}

