#pragma once
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Core {
// Opt-in buffered CPU wall-time trace; contains nested scopes, never GPU durations.
// Set MIKAN_CPU_STAGE_TRACE to an output CSV path before launching the process.
class CpuStageTrace {
    using Clock=std::chrono::steady_clock;
    struct Writer {
        std::mutex mutex;
        std::ofstream file;
        std::unordered_map<std::string,uint64_t> calls;
        Clock::time_point epoch=Clock::now();
        Writer(){const char* path=std::getenv("MIKAN_CPU_STAGE_TRACE");if(path&&*path){file.open(path);if(file)file<<"stage,call,start_ms,duration_ms\n";}}
    };
    static Writer& Output(){static Writer writer;return writer;}
    static bool Enabled(){static const bool enabled=[](){const char* p=std::getenv("MIKAN_CPU_STAGE_TRACE");return p&&*p;}();return enabled;}
    const char* stage;
    bool active;
    Clock::time_point start;
public:
    explicit CpuStageTrace(const char* name):stage(name),active(Enabled()){
        if(active){(void)Output();start=Clock::now();}
    }
    ~CpuStageTrace(){
        if(!active)return;
        const auto end=Clock::now();auto& writer=Output();
        std::lock_guard lock(writer.mutex);if(!writer.file)return;
        writer.file<<std::quoted(stage)<<','<<++writer.calls[stage]<<','<<std::fixed<<std::setprecision(6)
            <<std::chrono::duration<double,std::milli>(start-writer.epoch).count()<<','
            <<std::chrono::duration<double,std::milli>(end-start).count()<<'\n';
    }
    CpuStageTrace(const CpuStageTrace&)=delete;
    CpuStageTrace& operator=(const CpuStageTrace&)=delete;
};
}
