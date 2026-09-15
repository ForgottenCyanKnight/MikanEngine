PhysicsManager::PhysicsManager() : 
    physicsSystem(nullptr),
    jobSystem(nullptr),
    tempAllocator(nullptr),
    broadPhaseLayerInterface(nullptr),
    objectVsBroadPhaseLayerFilter(nullptr),
    objectLayerPairFilter(nullptr)
{
}

PhysicsManager::~PhysicsManager() {
    Shutdown();
}

// 碰撞回调实现
class JoltCollisionListener : public JPH::ContactListener {
private:
    Physics::PhysicsManager* physicsManager;
public:
    JoltCollisionListener(Physics::PhysicsManager* manager) : physicsManager(manager) {}
    
    virtual JPH::ValidateResult OnContactValidate(const JPH::Body& body1, const JPH::Body& body2, JPH::RVec3Arg baseOffset, const JPH::CollideShapeResult& collisionResult) override {
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }
    
    virtual void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override {
        physicsManager->QueueCollisionEvent(
            Physics::PhysicsManager::CollisionEventType::Enter,
            body1.GetID(), body2.GetID());
    }
    
    virtual void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override {
        physicsManager->QueueCollisionEvent(
            Physics::PhysicsManager::CollisionEventType::Stay,
            body1.GetID(), body2.GetID());
    }
    
    virtual void OnContactRemoved(const JPH::SubShapeIDPair& subShapePair) override {
        physicsManager->QueueCollisionEvent(
            Physics::PhysicsManager::CollisionEventType::Exit,
            subShapePair.GetBody1ID(), subShapePair.GetBody2ID());
    }
};

void PhysicsManager::Initialize() {
    if (physicsSystem) {
        // Initialize 可能被编辑器重入调用；避免重复创建 Jolt 世界、线程池
        // 和监听器，重复初始化是停止/播放崩溃的重要来源之一。
        return;
    }

    // 初始化JoltPhysics
    JPH::RegisterDefaultAllocator();
    
    // 设置Trace回调函数
    JPH::Trace = Physics::JoltTrace;
    
    // 创建Factory
    if (JPH::Factory::sInstance == nullptr) {
        JPH::Factory::sInstance = new JPH::Factory();
    }
    
    // 注册所有类型
    JPH::RegisterTypes();
    
    // 创建临时分配器
    tempAllocator = new JPH::TempAllocatorImpl(10 * 1024 * 1024);
    
    // Jolt 的依赖/Barrier 由适配层维护，实际任务进入引擎统一队列。
    ::JobSystem& sharedJobSystem = ::JobSystem::GetInstance();
    sharedJobSystem.Start();
    const std::size_t sharedWorkerCount = sharedJobSystem.WorkerCount();
    jobSystem = new SharedJoltJobSystem(sharedWorkerCount);
    
    // 创建碰撞层接口（作为成员变量，确保在 PhysicsSystem 生命周期内保持有效）
    broadPhaseLayerInterface = new BroadPhaseLayerInterfaceImpl();
    objectVsBroadPhaseLayerFilter = new ObjectVsBroadPhaseLayerFilterImpl();
    objectLayerPairFilter = new ObjectLayerPairFilterImpl();
    
    // 创建物理系统
    physicsSystem = new JPH::PhysicsSystem();
    
    m_timeAccumulator = 0.0f;

    // 初始化物理系统。Jolt 的 body-pair/contact 上限超出后会让物体穿过
    // 世界，因此必须覆盖高密度堆叠场景的潜在邻接数量；同时给编辑器
    // 停止/恢复、临时运行时实体留出余量，避免短生命周期 Body 挤满容量。
    // 参数：maxBodies, numBodyMutexes, maxBodyPairs, maxContactConstraints, broadPhaseLayerInterface, objectVsBroadPhaseLayerFilter, objectLayerPairFilter
    physicsSystem->Init(2048, 0, 16384, 8192,
                        *broadPhaseLayerInterface,
                        *objectVsBroadPhaseLayerFilter,
                        *objectLayerPairFilter);
    
    // 设置碰撞监听器
    physicsSystem->SetBodyActivationListener(nullptr);
    m_JoltCollisionListener = std::make_unique<JoltCollisionListener>(this);
    physicsSystem->SetContactListener(m_JoltCollisionListener.get());
    
    // 设置重力（向下为负 Y 轴）
    physicsSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    
    LOGI("[PhysicsManager] Initialized successfully (shared job workers=%zu)",
           sharedWorkerCount);
}

void PhysicsManager::Shutdown() {
    m_timeAccumulator = 0.0f;
    {
        std::lock_guard<std::mutex> lock(m_collisionMutex);
        m_collisionEvents.clear();
        m_activeCollisionPairCounts.clear();
    }
    if (physicsSystem) {
        // ContactListener 的生命周期必须短于 PhysicsSystem；先解绑再销毁
        // Jolt 世界，避免世界析构期间回调访问已失效的 manager。
        physicsSystem->SetContactListener(nullptr);
        m_JoltCollisionListener.reset();
        delete physicsSystem;
        physicsSystem = nullptr;
    } else {
        m_JoltCollisionListener.reset();
    }

    persistentStaticBodies.clear();
    
    // 删除碰撞层接口
    delete broadPhaseLayerInterface;
    broadPhaseLayerInterface = nullptr;
    
    delete objectVsBroadPhaseLayerFilter;
    objectVsBroadPhaseLayerFilter = nullptr;
    
    delete objectLayerPairFilter;
    objectLayerPairFilter = nullptr;
    
    delete jobSystem;
    jobSystem = nullptr;
    
    delete tempAllocator;
    tempAllocator = nullptr;
    
    if (JPH::Factory::sInstance) {
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

void PhysicsManager::Update(float deltaTime) {
    if (!physicsSystem) return;
    
    // 优化：使用固定时间步长进行物理更新，提升稳定性和性能
    const float fixedDeltaTime = 1.0f / 60.0f; // 60Hz 固定步长
    const int collisionSteps = 1;
    
    // 累积时间（属于当前 PhysicsManager 生命周期，停止/重建后不会继承旧值）
    m_timeAccumulator += deltaTime;
    
    // 限制最大累积时间，避免螺旋死亡（spiral of death）
    const float maxAccumulatorTime = 0.25f; // 最多累积 0.25 秒
    if (m_timeAccumulator > maxAccumulatorTime) {
        m_timeAccumulator = maxAccumulatorTime;
    }
    
    // 多次执行固定步长的物理更新
    while (m_timeAccumulator >= fixedDeltaTime) {
        physicsSystem->Update(fixedDeltaTime, collisionSteps, tempAllocator, jobSystem);
        m_timeAccumulator -= fixedDeltaTime;
    }

    // Jolt 的 ContactListener 可能在 worker 线程执行；这里只在物理步骤返回
    // 后于调用线程派发，避免游戏/ECS 回调直接在物理线程改场景。
    DispatchCollisionEvents();
}

void PhysicsManager::Update(float deltaTime, const glm::vec3& cameraPos) {
    // 先执行物理更新
    Update(deltaTime);
    
    // 清理远距离刚体
    CleanupDistantBodies(cameraPos);
    // CleanupDistantBodies 可能触发 OnContactRemoved；保证该重载也能在本次
    // 调用结束前把排队事件交付给监听器。
    DispatchCollisionEvents();
}
