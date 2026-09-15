#pragma once

#include "Core/JobSystem.h"
#include "Rendering/RenderWorldBuilder.h"

#include <cstddef>
#include <mutex>
#include <vector>

struct RenderWorld;

// Adapts the pure RenderWorld finalization stage to the engine-wide scheduler.
// It deliberately accepts only an exclusive staging snapshot; ECS capture
// remains on the caller thread.
class RenderWorldFinalizeWorker final {
public:
    RenderWorldFinalizeWorker();
    ~RenderWorldFinalizeWorker();

    RenderWorldFinalizeWorker(const RenderWorldFinalizeWorker&) = delete;
    RenderWorldFinalizeWorker& operator=(const RenderWorldFinalizeWorker&) = delete;

    // Returns false when a task is already pending.  Large snapshots are split
    // into disjoint entity ranges according to the shared worker count.  The
    // caller must retain exclusive ownership of stagingWorld until Wait()
    // returns because the final merge writes its derived lists then.
    bool Submit(RenderWorld& stagingWorld);

    // Wait for the submitted task and rethrow its exception, if any.  When a
    // task completed, write its worker-only Finalize CPU time to the optional
    // output; a synchronous caller with no submitted task receives zero.
    void Wait(double* finalizeMilliseconds = nullptr);

private:
    mutable std::mutex m_Mutex;
    std::vector<JobSystem::JobHandle> m_Tasks;
    std::vector<RenderWorldFinalizePartition> m_Partitions;
    std::vector<double> m_PartitionMilliseconds;
    RenderWorld* m_PendingWorld = nullptr;
    double m_LastFinalizeMilliseconds = 0.0;
    bool m_Pending = false;
};
