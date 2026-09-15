#pragma once

#include "Platform/Export.h"

#include <cstddef>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

// Engine-wide CPU task scheduler seam.
//
// Physics, World and RenderWorld submit CPU work through this shared queue so
// the engine can control one global worker budget instead of creating separate
// pools for each subsystem.
class MIKAN_API JobSystem final {
public:
    using Job = std::function<void()>;
    using JobHandle = std::shared_future<void>;

    static JobSystem& GetInstance();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // Start the scheduler explicitly, or let the first Submit call start it
    // lazily. workerCount=0 uses MIKAN_JOB_WORKERS, then hardware threads
    // minus one, falling back to one. Physics, World and RenderWorld share
    // this budget.
    void Start(std::size_t workerCount = 0);

    // Enqueue one task.  Exceptions are captured by the returned handle and
    // are rethrown by JobHandle::get().
    JobHandle Submit(Job job);

    std::size_t WorkerCount() const;
    bool IsRunning() const;

private:
    JobSystem() = default;
    ~JobSystem();

    void WorkerLoop();
    std::size_t ResolveWorkerCount(std::size_t requested) const;

    mutable std::mutex m_Mutex;
    std::condition_variable m_Condition;
    std::deque<Job> m_Queue;
    std::vector<std::thread> m_Workers;
    bool m_Started = false;
    bool m_StopRequested = false;
};
