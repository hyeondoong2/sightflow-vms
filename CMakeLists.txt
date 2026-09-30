cmake_minimum_required(VERSION 3.20)

project(SightFlowVMS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(FFMPEG REQUIRED)

add_executable(sightflow-vms
    src/main.cpp
    src/RtspSource.cpp
    src/Decoder.cpp
    src/FrameConverter.cpp
)

target_include_directories(sightflow-vms PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(sightflow-vms PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(sightflow-vms PRIVATE ${FFMPEG_LIBRARIES})
