-- premake5.lua

project "rml"
  kind "ConsoleApp"
  language "C++"
  includedirs {"../include", "../3rd"}
  files "rml/*.C"
  warnings "Extra"
  buildoptions {'-fPIC', '--std=c++1z'} 
  links {'ml', 'capnp', 'kj', 'lmdb', 'tag', 'crypto', 'boost_program_options', 'boost_system', 'boost_filesystem'}
