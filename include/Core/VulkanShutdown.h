#pragma once

// Release all device-owned resources in the order required by the engine's
// global lifetime contract. EngineMain waits for the device before calling it.
void CleanupVulkan();
