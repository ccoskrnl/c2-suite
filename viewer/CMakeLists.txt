cmake_minimum_required(VERSION 3.16)
project(RatViewer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC：源文件按 UTF-8 解析（中文注释）
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 复用 client/core 协议栈（protocol/crypto/net）与 im_ctl 的 CONTROL 客户端
set(CORE_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/../client/core)
set(IMCTL_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../im_ctl)

# 统一输出到 <build>/bin/viewer.exe（避免多配置生成器再套一层 Debug/Release 子目录）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_BINARY_DIR}/bin)
endforeach()

add_executable(viewer
    main.cpp
    input_hook.cpp
    ${IMCTL_DIR}/ctl_client.cpp
    ${CORE_DIR}/protocol.cpp
    ${CORE_DIR}/crypto.cpp
    ${CORE_DIR}/net.cpp
)

target_include_directories(viewer PRIVATE
    ${CORE_DIR}
    ${IMCTL_DIR}
)

if(WIN32)
    target_link_libraries(viewer PRIVATE
        ws2_32.lib
        bcrypt.lib
        windowscodecs.lib
        user32.lib
        gdi32.lib
    )
endif()
