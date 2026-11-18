// Experimental host-only Vulkan layer. All non-draw operations pass through.
// This is deliberately opt-in; it does not remove Vulkan or the game renderer.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAYER_NAME "VK_LAYER_LOCAL_X4_draw_suppression"
typedef struct InstanceData {
    void *key;
    VkInstance handle;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_GetPhysicalDeviceProcAddr physical_gipa;
    struct InstanceData *next;
} InstanceData;
typedef struct DeviceData {
    void *key;
    VkDevice handle;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkQueuePresentKHR present;
    PFN_vkCmdDraw draw;
    PFN_vkCmdDrawIndexed draw_indexed;
    PFN_vkCmdDrawIndirect draw_indirect;
    PFN_vkCmdDrawIndexedIndirect draw_indexed_indirect;
    PFN_vkCmdDrawIndirectCount draw_count;
    PFN_vkCmdDrawIndexedIndirectCount draw_indexed_count;
    struct DeviceData *next;
} DeviceData;
static pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
static InstanceData *instances;
static DeviceData *devices;
static PFN_vkGetInstanceProcAddr bootstrap_gipa;
static atomic_int suppress_draws;
static atomic_ullong draw_calls;
static atomic_ullong presents;
static atomic_ullong forwarded_draws;
static int control_enabled;

static void *dispatch_key(const void *object) { return object ? *(void *const *)object : NULL; }
static InstanceData instance_data(const void *object) {
    InstanceData result = {0};
    void *key = dispatch_key(object);
    pthread_mutex_lock(&data_lock);
    for (InstanceData *p = instances; p; p = p->next)
        if (p->key == key) { result = *p; break; }
    pthread_mutex_unlock(&data_lock);
    return result;
}
static DeviceData device_data(const void *object) {
    DeviceData result = {0};
    void *key = dispatch_key(object);
    pthread_mutex_lock(&data_lock);
    for (DeviceData *p = devices; p; p = p->next)
        if (p->key == key) { result = *p; break; }
    pthread_mutex_unlock(&data_lock);
    return result;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_gipa(VkInstance, const char *);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_gdpa(VkDevice, const char *);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_physical_gipa(VkInstance instance, const char *name) {
    InstanceData data = instance_data(instance);
    return data.physical_gipa ? data.physical_gipa(instance, name) : (data.gipa ? data.gipa(instance, name) : NULL);
}

static VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkInstance *instance) {
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)info->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_GetPhysicalDeviceProcAddr physical_gipa = link->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;
    InstanceData *data = calloc(1, sizeof(*data));
    if (!data) return VK_ERROR_OUT_OF_HOST_MEMORY;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult result = next_create(info, allocator, instance);
    if (result != VK_SUCCESS) { free(data); return result; }
    data->key = dispatch_key(*instance);
    data->handle = *instance;
    data->gipa = gipa;
    data->physical_gipa = physical_gipa;
    pthread_mutex_lock(&data_lock);
    bootstrap_gipa = gipa;
    data->next = instances;
    instances = data;
    pthread_mutex_unlock(&data_lock);
    fprintf(stderr, "[X4DRAW] instance created; suppress_draws=%d\n", atomic_load(&suppress_draws));
    return result;
}
static VKAPI_ATTR void VKAPI_CALL destroy_instance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    InstanceData data = instance_data(instance);
    PFN_vkDestroyInstance next_destroy = data.gipa ? (PFN_vkDestroyInstance)data.gipa(instance, "vkDestroyInstance") : NULL;
    pthread_mutex_lock(&data_lock);
    InstanceData **entry = &instances;
    while (*entry) {
        if ((*entry)->handle == instance) { InstanceData *old = *entry; *entry = old->next; free(old); break; }
        entry = &(*entry)->next;
    }
    pthread_mutex_unlock(&data_lock);
    if (next_destroy) next_destroy(instance, allocator);
}
static VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice physical,
        const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *device) {
    InstanceData instance = instance_data(physical);
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)info->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    if (!link || !link->u.pLayerInfo || !instance.handle) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)gipa(instance.handle, "vkCreateDevice");
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;
    DeviceData *data = calloc(1, sizeof(*data));
    if (!data) return VK_ERROR_OUT_OF_HOST_MEMORY;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult result = next_create(physical, info, allocator, device);
    if (result != VK_SUCCESS) { free(data); return result; }
    data->key = dispatch_key(*device);
    data->handle = *device;
    data->gdpa = gdpa;
    data->present = (PFN_vkQueuePresentKHR)gdpa(*device, "vkQueuePresentKHR");
    data->draw = (PFN_vkCmdDraw)gdpa(*device, "vkCmdDraw");
    data->draw_indexed = (PFN_vkCmdDrawIndexed)gdpa(*device, "vkCmdDrawIndexed");
    data->draw_indirect = (PFN_vkCmdDrawIndirect)gdpa(*device, "vkCmdDrawIndirect");
    data->draw_indexed_indirect = (PFN_vkCmdDrawIndexedIndirect)gdpa(*device, "vkCmdDrawIndexedIndirect");
    data->draw_count = (PFN_vkCmdDrawIndirectCount)gdpa(*device, "vkCmdDrawIndirectCount");
    if (!data->draw_count) data->draw_count = (PFN_vkCmdDrawIndirectCount)gdpa(*device, "vkCmdDrawIndirectCountKHR");
    if (!data->draw_count) data->draw_count = (PFN_vkCmdDrawIndirectCount)gdpa(*device, "vkCmdDrawIndirectCountAMD");
    data->draw_indexed_count = (PFN_vkCmdDrawIndexedIndirectCount)gdpa(*device, "vkCmdDrawIndexedIndirectCount");
    if (!data->draw_indexed_count) data->draw_indexed_count = (PFN_vkCmdDrawIndexedIndirectCount)gdpa(*device, "vkCmdDrawIndexedIndirectCountKHR");
    if (!data->draw_indexed_count) data->draw_indexed_count = (PFN_vkCmdDrawIndexedIndirectCount)gdpa(*device, "vkCmdDrawIndexedIndirectCountAMD");
    pthread_mutex_lock(&data_lock);
    data->next = devices;
    devices = data;
    pthread_mutex_unlock(&data_lock);
    fprintf(stderr, "[X4DRAW] device created; compute/submission/synchronization pass through\n");
    return result;
}
static VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice device, const VkAllocationCallbacks *allocator) {
    DeviceData data = device_data(device);
    PFN_vkDestroyDevice next_destroy = data.gdpa ? (PFN_vkDestroyDevice)data.gdpa(device, "vkDestroyDevice") : NULL;
    pthread_mutex_lock(&data_lock);
    DeviceData **entry = &devices;
    while (*entry) {
        if ((*entry)->handle == device) { DeviceData *old = *entry; *entry = old->next; free(old); break; }
        entry = &(*entry)->next;
    }
    pthread_mutex_unlock(&data_lock);
    if (next_destroy) next_destroy(device, allocator);
}
static VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue queue, const VkPresentInfoKHR *info) {
    DeviceData data = device_data(queue);
    if (!data.present) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = data.present(queue, info);
    unsigned long long count = atomic_fetch_add(&presents, 1) + 1;
    // Optional test-only control. Fixed path, no arbitrary file reads. Switching
    // changes newly recorded draws; already recorded command buffers stay valid.
    if (control_enabled && count % 15 == 0) {
        FILE *file = fopen("/results/draw-mode.txt", "r");
        if (file) {
            int mode = fgetc(file);
            fclose(file);
            if ((mode == '0' || mode == '1') && mode - '0' != atomic_load(&suppress_draws)) {
                atomic_store(&suppress_draws, mode - '0');
                fprintf(stderr, "[X4DRAW] suppress_draws=%d changed via experiment control\n", mode - '0');
                fflush(stderr);
            }
        }
    }
    if (count == 1 || count % 300 == 0) {
        fprintf(stderr, "[X4DRAW] presents=%llu dropped_draw_calls=%llu result=%d forwarded_draw_calls=%llu suppress=%d\n",
                count, atomic_load(&draw_calls), result, atomic_load(&forwarded_draws), atomic_load(&suppress_draws));
        fflush(stderr);
    }
    return result;
}
// No dispatch, render-pass, compute, query, semaphore or fence commands are removed.
// Only draw calls themselves are omitted. Effects/visibility still need validation.
static VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    if (atomic_load(&suppress_draws)) { atomic_fetch_add(&draw_calls, 1); return; }
    DeviceData data = device_data(a);
    atomic_fetch_add(&forwarded_draws, 1);
    data.draw(a, b, c, d, e);
}
static VKAPI_ATTR void VKAPI_CALL draw_indexed(VkCommandBuffer a, uint32_t b, uint32_t c, uint32_t d, int32_t e, uint32_t f) {
    if (atomic_load(&suppress_draws)) { atomic_fetch_add(&draw_calls, 1); return; }
    DeviceData data = device_data(a);
    atomic_fetch_add(&forwarded_draws, 1);
    data.draw_indexed(a, b, c, d, e, f);
}
static VKAPI_ATTR void VKAPI_CALL draw_indirect(VkCommandBuffer a, VkBuffer b, VkDeviceSize c, uint32_t d, uint32_t e) {
    if (atomic_load(&suppress_draws)) { atomic_fetch_add(&draw_calls, 1); return; }
    DeviceData data = device_data(a);
    atomic_fetch_add(&forwarded_draws, 1);
    data.draw_indirect(a, b, c, d, e);
}
static VKAPI_ATTR void VKAPI_CALL draw_indexed_indirect(VkCommandBuffer a, VkBuffer b, VkDeviceSize c, uint32_t d, uint32_t e) {
    if (atomic_load(&suppress_draws)) { atomic_fetch_add(&draw_calls, 1); return; }
    DeviceData data = device_data(a);
    atomic_fetch_add(&forwarded_draws, 1);
    data.draw_indexed_indirect(a, b, c, d, e);
}
static VKAPI_ATTR void VKAPI_CALL draw_indirect_count(VkCommandBuffer a, VkBuffer b, VkDeviceSize c,
        VkBuffer d, VkDeviceSize e, uint32_t f, uint32_t g) {
    if (atomic_load(&suppress_draws)) { atomic_fetch_add(&draw_calls, 1); return; }
    DeviceData data = device_data(a);
    atomic_fetch_add(&forwarded_draws, 1);
    data.draw_count(a, b, c, d, e, f, g);
}
static VKAPI_ATTR void VKAPI_CALL draw_indexed_count(VkCommandBuffer a, VkBuffer b, VkDeviceSize c,
        VkBuffer d, VkDeviceSize e, uint32_t f, uint32_t g) {
    if (atomic_load(&suppress_draws)) { atomic_fetch_add(&draw_calls, 1); return; }
    DeviceData data = device_data(a);
    atomic_fetch_add(&forwarded_draws, 1);
    data.draw_indexed_count(a, b, c, d, e, f, g);
}
static PFN_vkVoidFunction interception(const char *name) {
#define ENTRY(api, function) if (strcmp(name, api) == 0) return (PFN_vkVoidFunction)function
    ENTRY("vkGetInstanceProcAddr", layer_gipa);
    ENTRY("vkGetDeviceProcAddr", layer_gdpa);
    ENTRY("vkCreateInstance", create_instance);
    ENTRY("vkDestroyInstance", destroy_instance);
    ENTRY("vkCreateDevice", create_device);
    ENTRY("vkDestroyDevice", destroy_device);
    ENTRY("vkQueuePresentKHR", present);
    {
        ENTRY("vkCmdDraw", draw);
        ENTRY("vkCmdDrawIndexed", draw_indexed);
        ENTRY("vkCmdDrawIndirect", draw_indirect);
        ENTRY("vkCmdDrawIndexedIndirect", draw_indexed_indirect);
        ENTRY("vkCmdDrawIndirectCount", draw_indirect_count);
        ENTRY("vkCmdDrawIndirectCountKHR", draw_indirect_count);
        ENTRY("vkCmdDrawIndirectCountAMD", draw_indirect_count);
        ENTRY("vkCmdDrawIndexedIndirectCount", draw_indexed_count);
        ENTRY("vkCmdDrawIndexedIndirectCountKHR", draw_indexed_count);
        ENTRY("vkCmdDrawIndexedIndirectCountAMD", draw_indexed_count);
    }
#undef ENTRY
    return NULL;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_gipa(VkInstance instance, const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "vkCreateInstance") == 0) return (PFN_vkVoidFunction)create_instance;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)layer_gipa;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)layer_gdpa;
    InstanceData data = instance_data(instance);
    PFN_vkGetInstanceProcAddr next = data.gipa;
    if (!next && !instance) {
        pthread_mutex_lock(&data_lock);
        next = bootstrap_gipa;
        pthread_mutex_unlock(&data_lock);
    }
    PFN_vkVoidFunction original = next ? next(instance, name) : NULL;
    PFN_vkVoidFunction replacement = interception(name);
    return original && replacement ? replacement : original;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_gdpa(VkDevice device, const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)layer_gdpa;
    DeviceData data = device_data(device);
    PFN_vkVoidFunction original = data.gdpa ? data.gdpa(device, name) : NULL;
    PFN_vkVoidFunction replacement = interception(name);
    return original && replacement ? replacement : original;
}
__attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
x4DrawNegotiate(VkNegotiateLayerInterface *version) {
    if (!version || version->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = layer_gipa;
    version->pfnGetDeviceProcAddr = layer_gdpa;
    version->pfnGetPhysicalDeviceProcAddr = layer_physical_gipa;
    const char *setting = getenv("X4_DRAW_SUPPRESS");
    atomic_store(&suppress_draws, setting && strcmp(setting, "1") == 0);
    const char *control = getenv("X4_DRAW_CONTROL");
    control_enabled = control && strcmp(control, "/results/draw-mode.txt") == 0;
    return VK_SUCCESS;
}
