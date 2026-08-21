#pragma once
// Export.h - DLL export/import helper for the Mikan Engine module split.
// Game.dll (runtime) is compiled with MIKAN_BUILD_GAME -> dllexport.
// Editor.dll and the host are compiled with MIKAN_USE_GAME -> dllimport.
// Editor.dll's own entry points use explicit __declspec(dllexport).
#ifdef _WIN32
  #ifdef MIKAN_BUILD_GAME
    #define MIKAN_API __declspec(dllexport)
  #elif defined(MIKAN_USE_GAME)
    #define MIKAN_API __declspec(dllimport)
  #else
    #define MIKAN_API
  #endif
#else
  #define MIKAN_API
#endif