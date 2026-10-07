cmake_minimum_required(VERSION 3.20)

project(SightFlowVMS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(FFMPEG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)

qt_standard_project_setup()

qt_add_executable(sightflow-vms
    src/main.cpp
    src/RtspSource.cpp
    src/Decoder.cpp
    src/FrameConverter.cpp
    src/FrameQueue.cpp
    src/CaptureWorker.cpp
)

qt_add_qml_module(sightflow-vms
    URI SightFlowVMS
    VERSION 1.0
    QML_FILES
        src/qml/Main.qml
    SOURCES
        src/VideoDisplayItem.h
        src/VideoDisplayItem.cpp
)

target_include_directories(sightflow-vms PRIVATE ${FFMPEG_INCLUDE_DIRS} src)
target_link_directories(sightflow-vms PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(sightflow-vms PRIVATE ${FFMPEG_LIBRARIES} Qt6::Quick)

# Qt (unlike vcpkg's FFmpeg) does not auto-copy its runtime DLLs/plugins next
# to the executable, so the app can't start without this: no Qt platform
# plugin (platforms/qwindows.dll) would be found.
if(WIN32)
    add_custom_command(TARGET sightflow-vms POST_BUILD
        COMMAND Qt6::windeployqt --qmldir "${CMAKE_SOURCE_DIR}/src/qml" "$<TARGET_FILE:sightflow-vms>"
        COMMENT "Deploying Qt runtime next to sightflow-vms.exe"
    )
endif()
