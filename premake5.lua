-- premake5.lua

workspace "RockStudio"
  location "build"
  configurations { "Debug", "Release" }
  filter "configurations:Debug"
    defines {"DEBUG"}
    symbols "On"
--    buildoptions {'-fsanitize=address', '-static-libasan'}
--    linkoptions {'-fsanitize=address', '-static-libasan'}

  filter "configurations:Release"
    defines {"NDEBUG"}
    optimize "On"
    symbols "On"

project "gen"
  kind "ConsoleApp"
  language "C++"
  files "gen/*.C"
  includedirs {"./", "./include"}
  warnings "Extra"
  buildoptions {'--std=c++1z'} 
  linkoptions {'-no-pie'}
  links {'boost_program_options', 'boost_system', 'boost_filesystem'}

  buildinputs {'include/rs/ml/core/Track.fbs'}
  postbuildcommands {"(cd ../ && build/%{cfg.buildtarget.abspath} include/rs/ml/core/Track.bfbs)"}

include "lib"
include "app"


