cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

if(NOT ENABLE_FRONTEND_API OR NOT ENABLE_QT)
  message(FATAL_ERROR "Output Mapping requires ENABLE_FRONTEND_API=ON and ENABLE_QT=ON")
endif()

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets
)

target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON CXX_STANDARD 17)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/controller.cpp
    src/controller.hpp
    src/core/geometry.hpp
    src/core/display-match.hpp
    src/model/mapping.cpp
    src/model/mapping.hpp
    src/platform/platform.hpp
    src/render/compositor.cpp
    src/render/compositor.hpp
    src/ui/obs-display-widget.cpp
    src/ui/obs-display-widget.hpp
    src/ui/output-window.cpp
    src/ui/output-window.hpp
    src/ui/preview-view.cpp
    src/ui/preview-view.hpp
    src/ui/mapping-editor.cpp
    src/ui/mapping-editor.hpp
    src/ui/theme.hpp
)

if(OS_MACOS)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/platform/platform-macos.mm)
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE "$<LINK_LIBRARY:FRAMEWORK,AppKit.framework>"
            "$<LINK_LIBRARY:FRAMEWORK,ApplicationServices.framework>"
            "$<LINK_LIBRARY:FRAMEWORK,IOKit.framework>"
  )
elseif(OS_WINDOWS)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/platform/platform-windows.cpp)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE setupapi user32)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/platform/platform-qt.cpp)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# Pure C++ unit tests for geometry & display identity matching (no OBS/Qt).
option(OUTPUT_MAPPING_TESTS "Build core unit tests" ON)
if(OUTPUT_MAPPING_TESTS)
  add_executable(output-mapping-core-tests tests/test_core.cpp)
  target_include_directories(output-mapping-core-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  set_target_properties(output-mapping-core-tests PROPERTIES CXX_STANDARD 17)
endif()
