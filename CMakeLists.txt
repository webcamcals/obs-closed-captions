cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

if(NOT OS_WINDOWS)
  message(FATAL_ERROR "${_name} uses DirectShow and only supports Windows.")
endif()

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# Copy of libdshowcapture (the library OBS's own Video Capture Device uses),
# with closed caption (CC pin) support added.
add_subdirectory(deps/libdshowcapture)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/win-dshow.cpp
    src/encode-dstr.hpp
    src/cea608-decoder.cpp
    src/cea608-decoder.hpp
    src/caption-sink.cpp
    src/caption-sink.hpp
    src/vbi-slicer.cpp
    src/vbi-slicer.hpp
)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE OBS::libdshowcapture strmiids ksuser setupapi winmm wmcodecdspuuid shell32 ole32 uuid
)

# Vendored third-party code produces warnings that are harmless.
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
