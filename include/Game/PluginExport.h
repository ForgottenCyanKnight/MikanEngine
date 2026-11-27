#pragma once

// Gameplay modules export their entry points while importing the engine API.
#if defined(_WIN32)
  #define MIKAN_GAME_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
  #define MIKAN_GAME_MODULE_EXPORT __attribute__((visibility("default")))
#else
  #define MIKAN_GAME_MODULE_EXPORT
#endif
