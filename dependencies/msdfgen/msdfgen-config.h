#pragma once

// 静态版 msdfgen-config.h（替代 CMake configure_file 生成）
// MSDFGEN_PUBLIC 空定义：Game.dll 直接编译源文件，无独立 DLL 边界

#define MSDFGEN_PUBLIC
#define MSDFGEN_EXT_PUBLIC

#define MSDFGEN_VERSION 1.2
#define MSDFGEN_VERSION_MAJOR 1
#define MSDFGEN_VERSION_MINOR 2
#define MSDFGEN_VERSION_REVISION 0
#define MSDFGEN_COPYRIGHT_YEAR 2025
