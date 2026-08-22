cmake_minimum_required(VERSION 3.10)
project(pxtone-linux CXX)
set(CMAKE_BUILD_RPATH "$ORIGIN")
set(CMAKE_INSTALL_RPATH "$ORIGIN")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(pxINCLUDE_OGGVORBIS px64BIT)

find_package(SDL2 REQUIRED)

# Core library shared between the CLI player and GUI apps.
add_library(pxtn SHARED
    pxtone/pxtnData.cpp
    pxtone/pxtnDelay.cpp
    pxtone/pxtnError.cpp
    pxtone/pxtnEvelist.cpp
    pxtone/pxtnMaster.cpp
    pxtone/pxtnMem.cpp
    pxtone/pxtnOverDrive.cpp
    pxtone/pxtnPulse_Frequency.cpp
    pxtone/pxtnPulse_Noise.cpp
    pxtone/pxtnPulse_NoiseBuilder.cpp
    pxtone/pxtnPulse_Oggv.cpp
    pxtone/pxtnPulse_PCM.cpp
    pxtone/pxtnPulse_Oscillator.cpp
    pxtone/pxtnService.cpp
    pxtone/pxtnService_moo.cpp
    pxtone/pxtnText.cpp
    pxtone/pxtnUnit.cpp
    pxtone/pxtnWoice.cpp
    pxtone/pxtnWoicePTV.cpp
    pxtone/pxtnWoice_io.cpp
    pxtone/pxtoneNoise.cpp
)
set_target_properties(pxtn PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(pxtn PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/pxtone)
target_link_libraries(pxtn PUBLIC vorbisfile)

add_executable(pxtone-play src/main.cpp)
target_link_libraries(pxtone-play PRIVATE pxtn SDL2::SDL2 m pthread)

# --- GUI: GTK4 piano-roll editor ------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)

add_executable(pxtone-editor src/editor.cpp)
target_include_directories(pxtone-editor PRIVATE ${GTK4_INCLUDE_DIRS})
target_link_libraries(pxtone-editor PRIVATE pxtn SDL2::SDL2 ${GTK4_LIBRARIES} m pthread)

# --- edit-logic smoke test (no GUI) ----------------------------------------
add_executable(edit_smoke tests/edit_smoke.cpp)
target_link_libraries(edit_smoke PRIVATE pxtn pthread)

add_executable(woice_smoke tests/woice_smoke.cpp)
target_link_libraries(woice_smoke PRIVATE pxtn pthread)

add_executable(history_smoke tests/history_smoke.cpp)
target_link_libraries(history_smoke PRIVATE pxtn pthread)

add_executable(event_smoke tests/event_smoke.cpp)
target_link_libraries(event_smoke PRIVATE pxtn pthread)

add_executable(song_smoke tests/song_smoke.cpp)
target_link_libraries(song_smoke PRIVATE pxtn pthread)

add_executable(unit_smoke tests/unit_smoke.cpp)
target_link_libraries(unit_smoke PRIVATE pxtn pthread)

# --- GUI: piano-roll style note visualizer -------------------------------
add_executable(pxtone-visualizer src/visualizer.cpp)
target_link_libraries(pxtone-visualizer PRIVATE pxtn SDL2::SDL2 m pthread)
