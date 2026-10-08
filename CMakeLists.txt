cmake_minimum_required(VERSION 3.20)

project(SightFlowVMS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(FFMPEG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Network)

qt_standard_project_setup()

qt_add_executable(sightflow-vms
    src/main.cpp
    src/RtspSource.cpp
    src/Decoder.cpp
    src/FrameConverter.cpp
    src/FrameQueue.cpp
    src/CaptureWorker.cpp
    src/SnapshotDecoder.cpp
)

qt_add_qml_module(sightflow-vms
    URI SightFlowVMS
    VERSION 1.0
    QML_FILES
        src/qml/Main.qml
    SOURCES
        src/VideoDisplayItem.h
        src/VideoDisplayItem.cpp
        src/ServerStatusModel.h
        src/ServerStatusModel.cpp
        src/SnapshotDisplayItem.h
        src/SnapshotDisplayItem.cpp
)

target_include_directories(sightflow-vms PRIVATE ${FFMPEG_INCLUDE_DIRS} src)
target_link_directories(sightflow-vms PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(sightflow-vms PRIVATE ${FFMPEG_LIBRARIES} Qt6::Quick Qt6::Network)

# Qt (unlike vcpkg's FFmpeg) does not auto-copy its runtime DLLs/plugins next
# to the executable, so the app can't start without this: no Qt platform
# plugin (platforms/qwindows.dll) would be found.
if(WIN32)
    add_custom_command(TARGET sightflow-vms POST_BUILD
        COMMAND Qt6::windeployqt --qmldir "${CMAKE_SOURCE_DIR}/src/qml" "$<TARGET_FILE:sightflow-vms>"
        COMMENT "Deploying Qt runtime next to sightflow-vms.exe"
    )
endif()

# Separate MediaMTX control-API status server (docs/DECISIONS.md D14) --
# independent executable, no shared state with sightflow-vms. Core+Network
# only (no Qml/Quick): the FFmpeg decode loop below runs on its own
# std::thread (DecodeWorker, D16), not through Qt at all. Reuses
# src/RtspSource.cpp and src/Decoder.cpp directly rather than duplicating
# them.
qt_add_executable(sightflow-server
    src/server/main.cpp
    src/server/HttpServer.cpp
    src/server/ClientConnection.cpp
    src/server/MediaMtxClient.cpp
    src/server/ChannelStatusService.cpp
    src/server/DecodeMetrics.cpp
    src/server/DecodeWorker.cpp
    src/server/DecodeMetricsService.cpp
    src/server/ChangeDetector.cpp
    src/server/ChangeEventLog.cpp
    src/server/ChangeEventService.cpp
    src/server/SnapshotEncoder.cpp
    src/RtspSource.cpp
    src/Decoder.cpp
)

target_include_directories(sightflow-server PRIVATE ${FFMPEG_INCLUDE_DIRS} src src/server)
target_link_directories(sightflow-server PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(sightflow-server PRIVATE Qt6::Core Qt6::Network ${FFMPEG_LIBRARIES})

if(WIN32)
    add_custom_command(TARGET sightflow-server POST_BUILD
        COMMAND Qt6::windeployqt "$<TARGET_FILE:sightflow-server>"
        COMMENT "Deploying Qt runtime next to sightflow-server.exe"
    )
endif()
