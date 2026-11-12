cmake_minimum_required(VERSION 3.10)
project(pxtone-linux CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(pxINCLUDE_OGGVORBIS px64BIT)

find_package(SDL2 REQUIRED)

add_library(pxtn STATIC
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
target_include_directories(pxtn PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/pxtone)
target_link_libraries(pxtn PUBLIC vorbisfile)

add_executable(pxtone-play src/main.cpp)
target_link_libraries(pxtone-play PRIVATE pxtn SDL2::SDL2 m pthread)
