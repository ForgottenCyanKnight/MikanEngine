#include "Core/JobSystem.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

JobSystem& JobSystem::GetInstance()
{
    static JobSystem instance;
    return instance;
}

JobSystem::~JobSystem()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_StopRequested = true;
    }
    m_Condition.notify_all();
    for (auto& worker : m_Workers) {
        if (worker.joinable()) worker.join();
    }
}

void JobSystem::Start(std::size_t workerCount)
{
    std::unique_lock<std::mutex> lock(m_Mutex);
    if (m_Started) {
        if (workerCount != 0 && workerCount != m_Workers.size()) {
            throw std::invalid_argument("JobSystem is already running with a different worker count");
        }
        return;
    }
    if (m_StopRequested) {
        throw std::runtime_error("JobSystem cannot restart after shutdown");
    }

    const std::size_t resolvedWorkerCount = ResolveWorkerCount(workerCount);
    m_Started = true;
    try {
        m_Workers.reserve(resolvedWorkerCount);
        for (std::size_t index = 0; index < resolvedWorkerCount; ++index) {
            m_Workers.emplace_back(&JobSystem::WorkerLoop, this);
        }
    } catch (...) {
        m_StopRequested = true;
        lock.unlock();
        m_Condition.notify_all();
        for (auto& worker : m_Workers) {
            if (worker.joinable()) worker.join();
        }
        lock.lock();
        m_Workers.clear();
        m_Started = false;
        m_StopRequested = false;
        throw;
    }
}

JobSystem::JobHandle JobSystem::Submit(Job job)
{
    if (!job) {
        throw std::invalid_argument("JobSystem cannot submit an empty job");
    }

    auto task = std::make_shared<std::packaged_task<void()>>(std::move(job));
    JobHandle handle = task->get_future().share();
    Start();
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_StopRequested) {
            throw std::runtime_error("JobSystem is stopping");
        }
        m_Queue.emplace_back([task]() mutable {
            (*task)();
        });
    }
    m_Condition.notify_one();
    return handle;
}

std::size_t JobSystem::WorkerCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Workers.size();
}

bool JobSystem::IsRunning() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Started && !m_StopRequested;
}

std::size_t JobSystem::ResolveWorkerCount(std::size_t requested) const
{
    if (requested != 0) return requested;

    const char* value = std::getenv("MIKAN_JOB_WORKERS");
    if (value != nullptr && value[0] != '\0') {
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        if (end != value && *end == '\0' && parsed > 0 &&
            parsed <= static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
            return static_cast<std::size_t>(parsed);
        }
    }

    const unsigned int hardwareWorkers = std::thread::hardware_concurrency();
    // Reserve one logical core for the caller/main thread. Physics, World,
    // RenderWorld and future jobs now share this budget instead of each
    // creating a hardware-sized private pool.
    return std::max<std::size_t>(
        1,
        hardwareWorkers > 1
            ? static_cast<std::size_t>(hardwareWorkers - 1)
            : 1);
}

void JobSystem::WorkerLoop()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Condition.wait(lock, [this] {
                return m_StopRequested || !m_Queue.empty();
            });
            if (m_StopRequested && m_Queue.empty()) return;
            job = std::move(m_Queue.front());
            m_Queue.pop_front();
        }

        // Submit wraps work in packaged_task, which stores task exceptions in
        // the returned future.  Keep the worker alive even if a future-facing
        // wrapper itself unexpectedly throws.
        try {
            job();
        } catch (...) {
        }
    }
}
