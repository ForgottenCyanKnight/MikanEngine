// HostMain.cpp - thin host executable
// Loads Game.dll (linked at build time) and forwards to MikanEngineMain.
// Editor.dll is optional and detected by Game.dll at startup.
#include <cstdio>

extern "C" __declspec(dllimport) int MikanEngineMain(int argc, char* argv[]);

int main(int argc, char* argv[])
{
    return MikanEngineMain(argc, argv);
}