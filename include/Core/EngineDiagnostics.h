#pragma once

#include <string>

namespace Core {

void DumpSchema(const std::string& path);
int RunPrefabSelftest();
bool DumpSceneState(const std::string& path, int frames, float fps);

} // namespace Core
