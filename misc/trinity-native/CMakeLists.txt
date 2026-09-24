cmake_minimum_required(VERSION 3.20)
project(trinity_native_fallback C)
set(BUILD_TRINITY_NATIVE_FALLBACK ON CACHE BOOL "Build bundled modules")
include(../../cmake_modules/TrinityNative.cmake)
