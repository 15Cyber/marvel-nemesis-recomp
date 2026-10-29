# Marvel Nemesis: Rise of the Imperfects (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(game LANGUAGES C CXX)
enable_language(RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The bundled SDK contains title-specific compatibility fixes.
set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
if(NOT EXISTS "${SDK}/cmake/XbeRecomp.cmake")
    message(FATAL_ERROR "The bundled sdk/ is missing. Obtain the complete source tree; do not substitute or SDK Sync an upstream checkout.")
endif()
set(XBE_SEPARATE_CODE ON)
include("${SDK}/cmake/XbeRecomp.cmake")

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

# partitions as xbe_symbols wrote them into config/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: build.bat writes it with xbe_symbols, the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# target name = GAME_EXE in build.bat, xbe = GAME_XBE
add_xbe_image(MarvelNemesis default.xbe game.toml ${GAME_PARTITIONS} game)
target_sources(MarvelNemesis PRIVATE resources/app.rc)
target_include_directories(MarvelNemesis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources)
set_property(SOURCE resources/app.rc APPEND PROPERTY OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources/app-icon.ico)
