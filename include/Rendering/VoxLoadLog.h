#pragma once
#include <cstdlib>
inline bool VoxLoadVerbose(){
    static const bool enabled=[](){const char* v=std::getenv("MIKAN_VOX_LOAD_VERBOSE");return v&&v[0]=='1';}();
    return enabled;
}
