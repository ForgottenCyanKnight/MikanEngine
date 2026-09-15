#include "Rendering/RenderWorldFinalizeWorker.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>

namespace {

constexpr std::size_t kEntitiesPerFinalizePartition = 2048;
constexpr std::size_t kMaxFinalizePartitions = 8;

std::size_t ResolvePartitionCount(std::size_t entityCount, std::size_t workerCount)
{
    const std::size_t safeWorkerCount = std::max<std::size_t>(workerCount, 1);
    const std::size_t sizeBasedCount = entityCount == 0
        ? 1
        : (entityCount + kEntitiesPerFinalizePartition - 1) /
          kEntitiesPerFinalizePartition;
    return std::max<std::size_t>(
        1,
        std::min({safeWorkerCount, sizeBasedCount, kMaxFinalizePartitions}));
}

double MaxPartitionMilliseconds(const std::vector<double>& partitionMilliseconds)
{
    double result = 0.0;
    for (const double milliseconds : partitionMilliseconds) {
        result = std::max(result, milliseconds);
    }
    return result;
}

} // namespace

RenderWorldFinalizeWorker::RenderWorldFinalizeWorker() = default;

RenderWorldFinalizeWorker::~RenderWorldFinalizeWorker()
{
    try {
        Wait();
    } catch (...) {
        // Destruction cannot report a task failure.  Wait() still clears the
        // handle before rethrowing, so the shared scheduler never references
        // this adapter after its destructor returns.
    }
}

bool RenderWorldFinalizeWorker::Submit(RenderWorld& stagingWorld)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Pending) return false;

    JobSystem& scheduler = JobSystem::GetInstance();
    scheduler.Start();

    const std::size_t partitionCount =
        ResolvePartitionCount(stagingWorld.entities.size(), scheduler.WorkerCount());
    m_Tasks.clear();
    m_Tasks.reserve(partitionCount);
    m_Partitions.clear();
    m_Partitions.resize(partitionCount);
    m_PartitionMilliseconds.assign(partitionCount, 0.0);
    m_PendingWorld = &stagingWorld;
    m_LastFinalizeMilliseconds = 0.0;

    try {
        for (std::size_t partitionIndex = 0; partitionIndex < partitionCount; ++partitionIndex) {
            const std::size_t begin =
                (stagingWorld.entities.size() * partitionIndex) / partitionCount;
            const std::size_t end =
                (stagingWorld.entities.size() * (partitionIndex + 1)) / partitionCount;
            m_Tasks.emplace_back(scheduler.Submit(
                [this, &stagingWorld, partitionIndex, begin, end] {
                    const auto finalizeStart = std::chrono::steady_clock::now();
                    try {
                        RenderWorldBuilder::FinalizeRange(
                            stagingWorld, begin, end, m_Partitions[partitionIndex]);
                    } catch (...) {
                        const auto finalizeEnd = std::chrono::steady_clock::now();
                        m_PartitionMilliseconds[partitionIndex] =
                            std::chrono::duration<double, std::milli>(
                                finalizeEnd - finalizeStart).count();
                        throw;
                    }
                    const auto finalizeEnd = std::chrono::steady_clock::now();
                    m_PartitionMilliseconds[partitionIndex] =
                        std::chrono::duration<double, std::milli>(
                            finalizeEnd - finalizeStart).count();
                }));
        }
    } catch (...) {
        // A partial submission still owns references to the staging buffer.
        // Drain those handles before exposing the failure to the caller.
        for (const JobSystem::JobHandle& task : m_Tasks) {
            try {
                task.get();
            } catch (...) {
            }
        }
        m_Tasks.clear();
        m_Partitions.clear();
        m_PartitionMilliseconds.clear();
        m_PendingWorld = nullptr;
        throw;
    }

    m_Pending = true;
    return true;
}

void RenderWorldFinalizeWorker::Wait(double* finalizeMilliseconds)
{
    std::vector<JobSystem::JobHandle> tasks;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Pending) {
            if (finalizeMilliseconds != nullptr) *finalizeMilliseconds = 0.0;
            return;
        }
        tasks = m_Tasks;
    }

    std::exception_ptr exception;
    for (const JobSystem::JobHandle& task : tasks) {
        try {
            task.get();
        } catch (...) {
            if (!exception) exception = std::current_exception();
        }
    }

    double completedMilliseconds = 0.0;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Pending) {
            if (finalizeMilliseconds != nullptr) *finalizeMilliseconds = 0.0;
            return;
        }

        if (!exception) {
            if (m_PendingWorld == nullptr) {
                exception = std::make_exception_ptr(
                    std::runtime_error("RenderWorld finalize has no pending staging world"));
            } else {
                try {
                    RenderWorldBuilder::MergeFinalizedPartitions(
                        *m_PendingWorld, m_Partitions);
                } catch (...) {
                    exception = std::current_exception();
                }
            }
        }

        completedMilliseconds = MaxPartitionMilliseconds(m_PartitionMilliseconds);
        m_LastFinalizeMilliseconds = 0.0;
        m_Tasks.clear();
        m_Partitions.clear();
        m_PartitionMilliseconds.clear();
        m_PendingWorld = nullptr;
        m_Pending = false;
    }
    if (finalizeMilliseconds != nullptr) {
        *finalizeMilliseconds = completedMilliseconds;
    }
    if (exception) std::rethrow_exception(exception);
}
