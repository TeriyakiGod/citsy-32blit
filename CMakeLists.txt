cmake_minimum_required(VERSION 3.20)

project(citsy-32blit LANGUAGES C CXX ASM)

set(PROJECT_SOURCE game.cpp game.hpp host.cpp host.hpp)
set(PROJECT_DISTRIBS LICENSE README.md)

if(MSVC)
  add_compile_options("/W4" "/wd4244" "/wd4324" "/wd4458" "/wd4100")
else()
  add_compile_options("-Wall" "-Wextra" "-Wdouble-promotion" "-Wno-unused-parameter")
endif()

find_package(32BLIT CONFIG REQUIRED PATHS ../32blit-sdk $ENV{PATH_32BLIT_SDK})

# ---------------------------------------------------------------------------
# citsy engine (headless). Prefer a local checkout, else FetchContent.
# ---------------------------------------------------------------------------
set(CITSY_DIR "" CACHE PATH "Path to a local citsy source tree")
set(CITSY_BUILD_TESTS OFF CACHE BOOL "Disable citsy tests in this project" FORCE)
set(CITSY_BUILD_EXAMPLES OFF CACHE BOOL "Disable citsy examples in this project" FORCE)

if(CITSY_DIR)
  add_subdirectory(${CITSY_DIR} ${CMAKE_BINARY_DIR}/citsy)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../citsy/CMakeLists.txt")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../citsy" ${CMAKE_BINARY_DIR}/citsy)
else()
  include(FetchContent)
  FetchContent_Declare(
    citsy
    GIT_REPOSITORY https://github.com/TeriyakiGod/citsy.git
    GIT_TAG        main
  )
  FetchContent_MakeAvailable(citsy)
endif()

blit_executable(${PROJECT_NAME} ${PROJECT_SOURCE})

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)

target_link_libraries(${PROJECT_NAME} citsy)

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".html")
else()
  set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".elf")
endif()

blit_assets_yaml(${PROJECT_NAME} assets.yml)
blit_metadata(${PROJECT_NAME} metadata.yml)
add_custom_target(flash DEPENDS ${PROJECT_NAME}.flash)

install(FILES ${PROJECT_DISTRIBS} DESTINATION .)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_GENERATOR "ZIP" "TGZ")
include(CPack)
