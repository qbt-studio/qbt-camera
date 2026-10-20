cmake_minimum_required(VERSION 3.24)
project(QBTCameraStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the camera DLL runs inside a Windows service; it must not depend on a redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_compile_options(/W4 /permissive- /utf-8 /EHsc /MP)

# Virtual camera media source, loaded by the Windows Camera Frame Server.
add_library(QBTVirtualCamera SHARED
    vcam/Activator.cpp
    vcam/FrameReader.cpp
    vcam/MediaSource.cpp
    vcam/MediaStream.cpp
    vcam/Module.cpp
    vcam/Placeholder.cpp
    vcam/QBTVirtualCamera.def)
target_compile_definitions(QBTVirtualCamera PRIVATE __WRL_CLASSIC_COM_STRICT__)
# C4458: IMFAttributes parameter names shadow a WRL base member; C4324: padding inside WRL headers.
target_compile_options(QBTVirtualCamera PRIVATE /wd4458 /wd4324)
target_link_libraries(QBTVirtualCamera PRIVATE mfplat mfuuid ole32 advapi32 gdi32 user32 runtimeobject)

# The Windows app: receives the iPhone stream, decodes on the GPU, previews and feeds the camera.
add_executable(QBTCamera WIN32
    host/ControlPanel.cpp
    host/Decoder.cpp
    host/Main.cpp
    host/PhoneLink.cpp
    host/SharedFrameWriter.cpp
    host/Usbmux.cpp
    host/VideoPipeline.cpp
    host/VirtualCamera.cpp)
target_link_libraries(QBTCamera PRIVATE
    d3d11 dxgi mfplat mfuuid mfsensorgroup wmcodecdspuuid ws2_32 ole32 shell32 user32 gdi32 comctl32 dwmapi)
