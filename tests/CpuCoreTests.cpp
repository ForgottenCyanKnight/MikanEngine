#include "Core/JobSystem.h"
#include "Core/Log.h"
#include "Core/Utf8Path.h"
#include "Core/Physics2DManager.h"
#include "ECS/EntityManager.h"
#include "ECS/ComponentManager.h"
#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        Core::SetLogFileEnabled(false);
        // Exercise production task dispatch, completion and exception delivery.
        auto& jobs = JobSystem::GetInstance();
        jobs.Start(2);
        std::atomic<int> total{0};
        std::vector<JobSystem::JobHandle> handles;
        for (int i = 1; i <= 100; ++i)
            handles.push_back(jobs.Submit([&, i] { total.fetch_add(i); }));
        for (auto& handle : handles) handle.get();
        Require(total == 5050, "scheduler lost work");
        bool caught = false;
        try { jobs.Submit([] { throw std::runtime_error("expected"); }).get(); }
        catch (const std::runtime_error&) { caught = true; }
        Require(caught, "scheduler lost task exception");

        // Actual ECS storage: compaction, generation invalidation and reuse.
        auto entities = std::make_unique<ECS::EntityManager>();
        ECS::ComponentManager components;
        struct Position { float x = 0, y = 0; };
        components.RegisterComponent<Position>();
        const auto first = entities->CreateEntity();
        const auto second = entities->CreateEntity();
        const auto stale = entities->GetHandle(first);
        components.AddComponent(first, Position{1, 2});
        components.AddComponent(second, Position{3, 4});
        components.EntityDestroyed(first);
        Require(entities->DestroyEntity(first), "entity destruction failed");
        Require(!entities->DestroyEntity(first), "duplicate destruction accepted");
        Require(!entities->IsAlive(stale), "stale entity handle remained alive");
        Require(components.GetComponent<Position>(second).y == 4, "component compaction corrupted data");
        for (ECS::Entity i = 0; i < ECS::MAX_ENTITIES - 2; ++i) entities->CreateEntity();
        const auto reused = entities->CreateEntity();
        Require(reused == first && !entities->IsAlive(stale), "recycled entity accepted stale generation");

        // Fixed-step Box2D world: fall onto a static floor, then settle.
        auto& physics = Physics2DManager::GetInstance();
        physics.Initialize();
        Require(physics.IsInitialized(), "engine Box2D world initialization failed");
        physics.SetGravity({0, -10});
        const auto floor = physics.CreateBoxBody({0, -0.5f}, {10, 0.5f}, false, 1, 0.3f, 0);
        const auto body = physics.CreateBoxBody({0, 5}, {0.5f, 0.5f}, true, 1, 0.3f, 0);
        Require(physics.IsBodyValid(floor) && physics.IsBodyValid(body), "engine body handle invalid");
        for (int frame = 0; frame < 240; ++frame) physics.Update(1.0f / 60);
        const auto position = physics.GetBodyPosition(body);
        physics.Shutdown();
        Require(!physics.IsInitialized(), "engine physics shutdown failed");
        Require(std::isfinite(position.y) && std::abs(position.y - 0.5f) < 0.05f,
                "Box2D body did not settle on floor");
        Require(Utf8String(Utf8Path("测试目录/model.glb")) == "测试目录/model.glb", "UTF-8 path roundtrip failed");
        // Machine-readable result for cloud automation.
        std::cout << "{\"status\":\"pass\",\"frames\":240,\"jobs_sum\":" << total.load()
                  << ",\"box2d_y\":" << position.y << ",\"ecs_generation\":true}\n";
        Core::ShutdownLog();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
