// Native, GPU-free smoke test for the Asteria Vulkan pNext copy path.
// Compile against the *prepared* pinned Asteria source so the test exercises
// the actual layered header after integration/fg/asteria-x64.patch is applied.
#include "DeviceFeatureChain.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

using asteria_device_features::OwnedChain;
using asteria_device_features::PhysicalDeviceDescriptorPoolOverallocationFeaturesNVCompat;
using asteria_device_features::PhysicalDeviceMaintenance7FeaturesKHRCompat;

static_assert(std::is_standard_layout_v<PhysicalDeviceDescriptorPoolOverallocationFeaturesNVCompat>);
static_assert(sizeof(PhysicalDeviceDescriptorPoolOverallocationFeaturesNVCompat) == 24,
              "x64 Vulkan NV feature struct must preserve the actual ABI size");
static_assert(offsetof(PhysicalDeviceDescriptorPoolOverallocationFeaturesNVCompat, pNext) == 8);
static_assert(offsetof(PhysicalDeviceDescriptorPoolOverallocationFeaturesNVCompat, descriptorPoolOverallocation) == 16);

static int fail(const char *message) {
    std::fprintf(stderr, "feature-chain test FAILED: %s\n", message);
    return 1;
}

int main() {
    // This mixed chain reproduces the regression-prone NVIDIA + original GF
    // cases, not just the newly added structure.
    VkPhysicalDeviceVulkan12Features v12{};
    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    v12.timelineSemaphore = VK_TRUE;

    PhysicalDeviceMaintenance7FeaturesKHRCompat maintenance7{};
    maintenance7.sType = static_cast<VkStructureType>(1000562000);
    maintenance7.pNext = &v12;
    maintenance7.maintenance7 = VK_TRUE;

    VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT pageable{};
    pageable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT;
    pageable.pNext = &maintenance7;
    pageable.pageableDeviceLocalMemory = VK_TRUE;

    VkPhysicalDeviceMultiDrawFeaturesEXT multiDraw{};
    multiDraw.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT;
    multiDraw.pNext = &pageable;
    multiDraw.multiDraw = VK_TRUE;

    PhysicalDeviceDescriptorPoolOverallocationFeaturesNVCompat nv{};
    nv.sType = static_cast<VkStructureType>(1000546000);
    nv.pNext = &multiDraw;
    nv.descriptorPoolOverallocation = VK_TRUE;

    VkPhysicalDeviceFeatures2 root{};
    root.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    root.pNext = &nv;
    root.features.shaderInt16 = VK_TRUE;

    OwnedChain clone;
    VkStructureType rejected = VK_STRUCTURE_TYPE_MAX_ENUM;
    if (!clone.Copy(&root, &rejected))
        return fail("known NVIDIA + GF chain must copy successfully");
    if (rejected != VK_STRUCTURE_TYPE_MAX_ENUM)
        return fail("successful copy must not claim a rejected sType");

    const VkStructureType expected[] = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        static_cast<VkStructureType>(1000546000),
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT,
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT,
        static_cast<VkStructureType>(1000562000),
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
    };
    const void *originals[] = {&root, &nv, &multiDraw, &pageable, &maintenance7, &v12};
    auto node = reinterpret_cast<const VkBaseInStructure *>(clone.Head());
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        if (!node || node->sType != expected[i])
            return fail("copied pNext chain lost original node order");
        if (node == originals[i])
            return fail("chain retained a source pointer instead of owning a copy");
        if (i == 0 &&
            !reinterpret_cast<const VkPhysicalDeviceFeatures2 *>(node)->features.shaderInt16)
            return fail("core feature values not preserved");
        if (i == 1 &&
            !reinterpret_cast<const PhysicalDeviceDescriptorPoolOverallocationFeaturesNVCompat *>(node)->descriptorPoolOverallocation)
            return fail("NVIDIA feature value not preserved");
        if (i == 2 &&
            !reinterpret_cast<const VkPhysicalDeviceMultiDrawFeaturesEXT *>(node)->multiDraw)
            return fail("existing MultiDraw feature lost");
        if (i == 3 &&
            !reinterpret_cast<const VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT *>(node)->pageableDeviceLocalMemory)
            return fail("existing Pageable feature lost");
        if (i == 4 &&
            !reinterpret_cast<const PhysicalDeviceMaintenance7FeaturesKHRCompat *>(node)->maintenance7)
            return fail("existing Maintenance7 feature lost");
        if (i == 5 &&
            !reinterpret_cast<const VkPhysicalDeviceVulkan12Features *>(node)->timelineSemaphore)
            return fail("Vulkan 1.2 feature value lost");
        node = node->pNext;
    }
    if (node)
        return fail("copied chain has unexpected trailing nodes");

    nv.descriptorPoolOverallocation = VK_FALSE;
    if (!reinterpret_cast<const PhysicalDeviceDescriptorPoolOverallocationFeaturesNVCompat *>(
             reinterpret_cast<const VkBaseInStructure *>(clone.Head())->pNext)
             ->descriptorPoolOverallocation)
        return fail("copy still aliases source feature data");

    // A truly unknown device feature cannot safely be copied without
    // knowing its ABI size. Reject it and report the exact identifier.
    VkBaseInStructure unknown{};
    unknown.sType = static_cast<VkStructureType>(123456789);
    root.pNext = &unknown;
    rejected = VK_STRUCTURE_TYPE_MAX_ENUM;
    if (clone.Copy(&root, &rejected) ||
        rejected != unknown.sType || clone.Head() != nullptr)
        return fail("unknown sType must fail closed and be reported");

    // Malformed cyclic feature chains must not trigger an infinite loop.
    root.pNext = &root;
    rejected = VK_STRUCTURE_TYPE_MAX_ENUM;
    if (clone.Copy(&root, &rejected) || clone.Head() != nullptr)
        return fail("cyclic pNext chain must fail closed");

    std::puts("feature-chain test PASSED: NVIDIA ABI, copy/order/values, legacy features, unknown/cyclic reject");
    return 0;
}
