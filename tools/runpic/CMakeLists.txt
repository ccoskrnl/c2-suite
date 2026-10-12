cmake_minimum_required(VERSION 3.16)
project(RunPic)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC：源文件按 UTF-8 解析（中文注释）
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 统一输出到 <build>/bin/runpic.exe（避免多配置生成器再套一层 Debug/Release 子目录）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_BINARY_DIR}/bin)
endforeach()

# PIC blob 执行器（反射加载验证 harness）——单文件，无外部依赖
add_executable(runpic runpic.cpp)
