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

//#include <lib/rs/ml/query/TrackFieldAccessorGenerator.h>

#include "Generator.h"
#include <flatbuffers/reflection.h>
#include <flatbuffers/util.h>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/dll/import.hpp>
#include <iostream>
#include <fstream>
#include <sstream>

using namespace rs::ml::gen;
namespace bpo = boost::program_options;
namespace bfs = boost::filesystem;

namespace
{
  /*
  std::vector<std::pair<std::string, Generator>> generators = 
  {
    {"rs/ml/query/gen/TrackFieldAccessor.h", rs::ml::query::TrackFieldAccessorGenerator{}}
  };*/

  bool getFields(const std::string& schemaContent, const char* name, Fields& fields)
  {
    ::flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t *>(schemaContent.c_str()), schemaContent.length());

    if (!::reflection::VerifySchemaBuffer(verifier))
    {
      std::cerr << "Invalid binary schema file" << std::endl;
      return false;
    }

    // Make sure the schema is what we expect it to be.
    const auto* schema = ::reflection::GetSchema(schemaContent.c_str());
    const auto* root_table = schema->root_table();
    
    if (std::strcmp(root_table->name()->c_str(), name) != 0)
    {
      std::cerr << "Invalid root table, " << name << " expected" << std::endl;
      return false;
    }

    fields.assign(root_table->fields()->begin(), root_table->fields()->end());
    std::sort(fields.begin(), fields.end(), [](auto l, auto r) { return l->id() < r->id(); });
    return true;
  }

  std::ofstream prepareOutputFile(const bfs::path& path)
  {
    auto dir = path.parent_path();
    
    if(!boost::filesystem::exists(dir))
    {
      if (!boost::filesystem::create_directory(dir))
      {
        std::cerr << "Failed to create directory : " << dir << std::endl;
        return {};
      }
    }
    
    std::ofstream ofs{path.c_str(), std::ios::out | std::ios::trunc};

    if(!ofs)
    {  
      std::cerr << "File opening failed : " << path << std::endl;;
      return {};
    }

    return ofs;
  }
}

int main(int argc, char* argv[])
{
  std::string metaSchemaPath, propSchemaPath, outDir;
  std::vector<std::string> generatorPaths;

  bpo::options_description desc("Allowed options");
  desc.add_options()
    ("help", "produce help message")
    ("meta-schema", bpo::value<std::string>(&metaSchemaPath)->required(), "generated binary schema-file of metadata")
    ("prop-schema", bpo::value<std::string>(&propSchemaPath)->required(), "generated binary schema-file of properties")
    ("out-dir", bpo::value<std::string>(&outDir)->default_value("./"), "base dir for generated files")
    ("generators", bpo::value<std::vector<std::string>>(&generatorPaths)->multitoken(), "paths of generator plugins");

  bpo::positional_options_description ppo;
  ppo.add("generators", -1);

  bpo::variables_map vm;
  bpo::store(bpo::command_line_parser(argc, argv).options(desc).positional(ppo).run(), vm);
      
  if (vm.count("help"))
  {
    std::cout << desc << std::endl;
    return 0;
  }

  bpo::notify(vm);

  std::string metaSchemaContent, propSchemaContent;
  
  if (!::flatbuffers::LoadFile(metaSchemaPath.c_str(), true, &metaSchemaContent) 
      || !::flatbuffers::LoadFile(propSchemaPath.c_str(), true, &propSchemaContent))
  {
    std::cerr << "Failed to load schema file content" << std::endl;
    return 1;
  }

  Fields metaFields, propFields;
    
  if (!getFields(metaSchemaContent, "rs.ml.core.Metadata", metaFields) 
      || !getFields(propSchemaContent, "rs.ml.core.Properties", propFields))
  {
    std::cerr << "Failed to get fields info from schema" << std::endl;
    return 1;
  }

  std::vector<boost::shared_ptr<Generator>> generators;

	for (const auto& gp : generatorPaths)
  {
    bfs::path path{gp};
    generators.push_back(boost::dll::import<Generator>(path, "generator", boost::dll::load_mode::append_decorations));
	}

  bfs::path base{outDir};

  for (const auto& g : generators)
  {
    auto path = base / g->outputFile();
    auto ofs = prepareOutputFile(path);

    if (!ofs)
    {
      std::cerr << "Failed to prepare output file " << path << std::endl;
      return 1;
    }

    g->run(ofs, metaFields, propFields);
    std::cout << path << " generated from " << g->name() << std::endl;
  }

  return 0;
}

