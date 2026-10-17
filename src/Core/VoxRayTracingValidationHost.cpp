#include "Rendering/RayTracing/RayTracingValidation.h"
#ifdef _WIN32
#include "Core/Utf8Path.h"
#include <vector>
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(argc);
    for (int i = 0; i < argc; ++i) arguments.push_back(GenericUtf8String(std::filesystem::path(argv[i])));
    std::vector<char*> pointers;
    for (auto& argument : arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    return RunVoxRayTracingValidation(argc, pointers.data());
}
#else
int main(int argc,char** argv){return RunVoxRayTracingValidation(argc,argv);}
#endif
