#pragma once

struct SDL_Window;

namespace Core {

// Releases runtime resources in dependency order and returns the final process
// exit code, preserving any earlier headless/state-dump failure.
int ShutdownEngine(SDL_Window* window, int screenshotFrame, int finalExitCode);

} // namespace Core
