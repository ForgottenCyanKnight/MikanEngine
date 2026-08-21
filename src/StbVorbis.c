// StbVorbis.c - stb_vorbis 实现(独立 C 编译单元)
// 原因: stb_vorbis 结构体含成员名 `alloc`(C++ 关键字), 按 C++ 编译会失败;
// 故实现(STB_VORBIS_IMPLEMENTATION)放本 .c 由 C 编译器编译,
// C++ 侧(AudioManager.cpp)只 include 头模式(声明)供 miniaudio 的 Vorbis 后端链接。
#define STB_VORBIS_IMPLEMENTATION
#include "stb/stb_vorbis.c"
