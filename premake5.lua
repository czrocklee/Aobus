-- premake5.lua

workspace "RockStudio"
  configurations { "Debug", "Release" }
  filter "configurations:Debug"
    defines {"DEBUG"}
    symbols "On"

  filter "configurations:Release"
    defines {"NDEBUG"}
    optimize "On"
    
    
include "rml"

project "rml"
  kind "ConsoleApp"
  language "C++"
  targetdir "bin/%{cfg.buildcfg}"
    includedirs {"rml/include"}
  
  files { "*.h", "*.cpp", "*.c++" }

	buildoptions {'-fPIC', '--std=c++1z'} 
	links {'rml', 'capnp', 'kj', 'lmdb', 'tag', 'crypto', 'boost_program_options', 'boost_system', 'boost_filesystem'}
 
