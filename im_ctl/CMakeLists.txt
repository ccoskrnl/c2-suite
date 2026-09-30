cmake_minimum_required(VERSION 3.16)
project(ImCtl)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC：源文件按 UTF-8 解析（中文注释）
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 路径设置
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
set(GLAD_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/external/glad)
set(GLFW_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/external/glfw)
set(CORE_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../client/core)

# 可执行文件
add_executable(app
    main.cpp
    ctl_client.cpp
    ${CORE_DIR}/protocol.cpp
    ${CORE_DIR}/crypto.cpp
    ${CORE_DIR}/net.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${GLAD_DIR}/src/glad.c
)

# 头文件目录
target_include_directories(app PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${GLAD_DIR}/include
    ${GLFW_DIR}/include
    ${CORE_DIR}
)

# 链接库
if(WIN32)
    target_link_libraries(app PRIVATE
        ${GLFW_DIR}/lib-vc2026/glfw3.lib   # 根据实际路径调整
        opengl32.lib
        ws2_32.lib
        bcrypt.lib
        windowscodecs.lib
    )
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(app PRIVATE glfw OpenGL::GL)
endif()

# 如果使用的是动态库（glfw3.dll），需要将 DLL 复制到输出目录
if(WIN32)
    add_custom_command(TARGET app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GLFW_DIR}/lib-vc2026/glfw3.dll"
            $<TARGET_FILE_DIR:app>
    )
endif()
