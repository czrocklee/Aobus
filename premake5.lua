-- premake5.lua

workspace "RockStudio"
  location "build"
  configurations { "Debug", "Release" }
  filter "configurations:Debug"
    defines {"DEBUG"}
    symbols "On"

  filter "configurations:Release"
    defines {"NDEBUG"}
    optimize "On"
    symbols "On"
    
include "lib"
include "app"


