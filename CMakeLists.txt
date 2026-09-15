cmake_minimum_required(VERSION 3.20)

project(citsy-32blit LANGUAGES C CXX ASM)

set(PROJECT_SOURCE
  game.cpp game.hpp
  host.cpp host.hpp
  launcher.cpp launcher.hpp
  hardware.cpp hardware.hpp
)
set(PROJECT_DISTRIBS LICENSE README.md)

if(MSVC)
  add_compile_options("/W4" "/wd4244" "/wd4324" "/wd4458" "/wd4100")
else()
  add_compile_options("-Wall" "-Wextra" "-Wdouble-promotion" "-Wno-unused-parameter")
endif()

# Pico SDK defaults to -fno-exceptions. Must be set before find_package(32BLIT)
# pulls in pico_sdk_init / pico_cxx_options.
set(PICO_CXX_ENABLE_EXCEPTIONS ON CACHE BOOL "citsy ParseError uses C++ exceptions")

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

# Embedded toolchains (STM32, Pico/VGC, Emscripten) compile with
# -fno-exceptions. citsy throws ParseError and the player catches it.
if(NOT MSVC)
  target_compile_options(citsy PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>)
endif()

# STM32: 32blit.toolchain also forces single-precision literals and treats
# double-promotion as an error. GCC keeps the last of each flag pair.
if(32BLIT_HW)
  target_compile_options(citsy PUBLIC
    -fno-single-precision-constant
    -Wno-error=double-promotion
  )
endif()

blit_executable(${PROJECT_NAME} ${PROJECT_SOURCE})

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)

target_link_libraries(${PROJECT_NAME} citsy)

# Battery ADC / backlight PWM on RP2350 targets (no-ops on desktop).
# BlitHalPico is created inside blit_executable(), so the SSD1351 source
# swap has to happen here — not immediately after find_package(32BLIT).
if(32BLIT_PICO)
  target_link_libraries(${PROJECT_NAME} hardware_adc hardware_pwm)
  target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/display")
  if(TARGET BlitHalPico)
    get_target_property(_citsy_hal_srcs BlitHalPico INTERFACE_SOURCES)
    if(_citsy_hal_srcs)
      list(FILTER _citsy_hal_srcs EXCLUDE REGEX "dbi_ssd1351\\.cpp$")
      list(APPEND _citsy_hal_srcs "${CMAKE_CURRENT_SOURCE_DIR}/display/dbi_ssd1351.cpp")
      set_property(TARGET BlitHalPico PROPERTY INTERFACE_SOURCES ${_citsy_hal_srcs})
      message(STATUS "Using patched SSD1351 driver: display/dbi_ssd1351.cpp")
    else()
      message(FATAL_ERROR "BlitHalPico has no INTERFACE_SOURCES; cannot patch SSD1351 driver")
    endif()
    target_include_directories(BlitHalPico INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/display")
  else()
    message(FATAL_ERROR "32BLIT_PICO is set but BlitHalPico was not created")
  endif()
endif()

if(NOT MSVC)
  # Pico's pico_cxx_options INTERFACE -fno-exceptions can still win on the
  # executable; force EH after linking.
  target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>)
endif()

# Device Release builds: -O3. The old vgc preset compiled CMAKE_BUILD_TYPE=Debug
# (-Og), which left the interpreter and the palette expand in the noise.
if(32BLIT_PICO)
  target_compile_options(citsy PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O3>)
  target_compile_options(${PROJECT_NAME} PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O3>)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  target_compile_options(${PROJECT_NAME} PRIVATE -sDISABLE_EXCEPTION_CATCHING=0)
  target_link_options(${PROJECT_NAME} PRIVATE -fexceptions -sDISABLE_EXCEPTION_CATCHING=0)
endif()

# 32blit PIC stdlibs are GCC 12. GCC 13+ libstdc++ headers call
# std::string::_M_replace_cold, which those archives do not provide.
if(32BLIT_HW AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16
            -print-file-name=libstdc++_nano.a
    OUTPUT_VARIABLE CITSY_LIBSTDCXX_NANO
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(EXISTS "${CITSY_LIBSTDCXX_NANO}")
    target_link_libraries(${PROJECT_NAME} ${CITSY_LIBSTDCXX_NANO})
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".html")
else()
  set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".elf")
endif()

blit_assets_yaml(${PROJECT_NAME} assets.yml)
blit_metadata(${PROJECT_NAME} metadata.yml)
add_custom_target(flash DEPENDS ${PROJECT_NAME}.flash)

# Host-side check of the SSD1351 init table (CLOCK_DIV, enhance, LUT).
if(NOT 32BLIT_PICO AND NOT 32BLIT_HW AND NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  add_executable(ssd1351_init_seq_test tests/ssd1351_init_seq_test.cpp)
  target_include_directories(ssd1351_init_seq_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  set_property(TARGET ssd1351_init_seq_test PROPERTY CXX_STANDARD 20)
  add_dependencies(${PROJECT_NAME} ssd1351_init_seq_test)
  add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ssd1351_init_seq_test
    COMMENT "Verify SSD1351 init sequence"
  )
endif()

install(FILES ${PROJECT_DISTRIBS} DESTINATION .)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_GENERATOR "ZIP" "TGZ")
include(CPack)
