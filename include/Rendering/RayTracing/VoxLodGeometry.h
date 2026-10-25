#pragma once
#include "Rendering/RayTracing/AccelerationStructure.h"
#include "Rendering/RayTracing/VoxLod.h"
class VoxLodGeometry {
public:
    bool RecordBuild(VkCommandBuffer cmd,const mikan::rt::VoxLodLevel& level);
    void ReleaseBuildInputs(){positions.Cleanup();indices.Cleanup();blas->ReleaseScratch();}
    VkDeviceAddress Address()const{return blas->Address();}
    const std::shared_ptr<AccelerationStructure>& Blas()const{return blas;}
private:
    std::shared_ptr<AccelerationStructure> blas=std::make_shared<AccelerationStructure>();
    VulkanBuffer positions,indices;
};
