/*
 * Vulkan
 *
 * Copyright (C) 2015 Valve Corporation
 * Copyright (C) 2015 Google Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Author: Chris Forbes <chrisforbes@google.com>
 */
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unordered_map>
#include <vector>
#include "util.hpp"
#include <vk_loader_platform.h>
#include <vulkan/vulkan.h>
#include <vk_dispatch_table_helper.h>
#include <vulkan/vk_layer.h>
#include "vk_layer_config.h"
#include "vk_layer_table.h"
#include "vk_enum_string_helper.h"
#include "vk_layer_logging.h"
#include "vk_layer_extension_utils.h"


struct WsiImageData {
    VkImage image;

    void Cleanup(VkDevice dev);
};


struct SwapChainData {
    unsigned width, height;
    VkFormat format;

    std::vector<WsiImageData *> presentableImages;

    void Cleanup(VkDevice dev);
};


struct layer_data {
    PFN_vkCreateSwapchainKHR pfnCreateSwapchainKHR;
    PFN_vkGetSwapchainImagesKHR pfnGetSwapchainImagesKHR;
    PFN_vkQueuePresentKHR pfnQueuePresentKHR;
    PFN_vkDestroySwapchainKHR pfnDestroySwapchainKHR;

    VkPhysicalDevice gpu;
    VkDevice dev;

    std::unordered_map<VkSwapchainKHR, SwapChainData*>* swapChains;

    void Cleanup();
};

static std::unordered_map<void *, layer_data *> layer_data_map;
static device_table_map overlay_device_table_map;
static instance_table_map overlay_instance_table_map;


template layer_data *get_my_data_ptr<layer_data>(
        void *data_key,
        std::unordered_map<void *, layer_data *> &data_map);


//static LOADER_PLATFORM_THREAD_ONCE_DECLARATION(g_initOnce);
// TODO : This can be much smarter, using separate locks for separate global data
static int globalLockInitialized = 0;
static loader_platform_thread_mutex globalLock;


static void
init_overlay(layer_data *my_data)
{
    if (!globalLockInitialized)
    {
        // TODO/TBD: Need to delete this mutex sometime.  How???  One
        // suggestion is to call this during vkCreateInstance(), and then we
        // can clean it up during vkDestroyInstance().  However, that requires
        // that the layer have per-instance locks.  We need to come back and
        // address this soon.
        loader_platform_thread_create_mutex(&globalLock);
        globalLockInitialized = 1;
    }
}


static void
after_device_create(VkPhysicalDevice gpu, VkDevice device, VkLayerDispatchTable *pTable, layer_data *data)
{
    VkResult U_ASSERT_ONLY err;

    data->gpu = gpu;
    data->dev = device;

    /* Get our WSI hooks in. */
    data->pfnCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)pTable->GetDeviceProcAddr(device, "vkCreateSwapchainKHR");
    data->pfnGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)pTable->GetDeviceProcAddr(device, "vkGetSwapchainImagesKHR");
    data->pfnQueuePresentKHR = (PFN_vkQueuePresentKHR)pTable->GetDeviceProcAddr(device, "vkQueuePresentKHR");
    data->pfnDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)pTable->GetDeviceProcAddr(device, "vkDestroySwapchainKHR");
    data->swapChains = new std::unordered_map<VkSwapchainKHR, SwapChainData*>;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks *pAllocator, VkDevice* pDevice)
{
    VkLayerDispatchTable *pDeviceTable = get_dispatch_table(overlay_device_table_map, *pDevice);
    VkResult result = pDeviceTable->CreateDevice(gpu, pCreateInfo, pAllocator, pDevice);
    if (result == VK_SUCCESS) {
        VkLayerDispatchTable *pTable = get_dispatch_table(overlay_device_table_map, *pDevice);
        layer_data *my_device_data = get_my_data_ptr(get_dispatch_key(*pDevice), layer_data_map);

        after_device_create(gpu, *pDevice, pTable, my_device_data);
    }
    return result;
}

/* hook DestroyDevice to remove tableMap entry */
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
    dispatch_key key = get_dispatch_key(device);

    layer_data *my_data = get_my_data_ptr(key, layer_data_map);
    my_data->Cleanup();

    VkLayerDispatchTable *pDisp =  get_dispatch_table(overlay_device_table_map, device);
    pDisp->DestroyDevice(device, pAllocator);
    overlay_device_table_map.erase(key);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo*                 pCreateInfo,
    const VkAllocationCallbacks                *pAllocator,
    VkInstance*                                 pInstance)
{
    VkLayerInstanceDispatchTable *pTable = get_dispatch_table(overlay_instance_table_map,*pInstance);
    VkResult result = pTable->CreateInstance(pCreateInfo, pAllocator, pInstance);

    if (result == VK_SUCCESS) {
        layer_data *my_data = get_my_data_ptr(get_dispatch_key(*pInstance), layer_data_map);
        init_overlay(my_data);
    }
    return result;
}

/* hook DestroyInstance to remove tableInstanceMap entry */
VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
    dispatch_key key = get_dispatch_key(instance);
    VkLayerInstanceDispatchTable *pTable = get_dispatch_table(overlay_instance_table_map, instance);
    pTable->DestroyInstance(instance, pAllocator);

    layer_data_map.erase(pTable);

    overlay_instance_table_map.erase(key);
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice                                 device,
    const VkSwapchainCreateInfoKHR*          pCreateInfo,
    const VkAllocationCallbacks*             pAllocator,
    VkSwapchainKHR*                          pSwapChain)
{
    VkLayerDispatchTable *pTable = get_dispatch_table(overlay_device_table_map, device);
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = my_data->pfnCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapChain);

    if (result == VK_SUCCESS) {
        auto & data = (*my_data->swapChains)[*pSwapChain];
        data = new SwapChainData;
        data->width = pCreateInfo->imageExtent.width;
        data->height = pCreateInfo->imageExtent.height;
        data->format = pCreateInfo->imageFormat;

#ifdef OVERLAY_DEBUG
        printf("Creating resources for scribbling on swapchain format %u width %u height %u\n",
                data->format, data->width, data->height);
#endif
    }

    return result;
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapChain,
    uint32_t *pCount,
    VkImage *pImages)
{
    VkLayerDispatchTable *pTable = get_dispatch_table(overlay_device_table_map, device);
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = my_data->pfnGetSwapchainImagesKHR(device, swapChain, pCount, pImages);
    VkResult U_ASSERT_ONLY err;

    /* GetSwapChainImagesWSI may be called without an images buffer, in which case it
     * just returns the count to the caller. We're only interested in acting on the
     * /actual/ fetch of the images.
     */
    if (pImages) {
        auto data = (*my_data->swapChains)[swapChain];

        for (int i = 0; i < *pCount; i++) {


            auto imageData = new WsiImageData;
            imageData->image = pImages[i];

            data->presentableImages.push_back(imageData);
        }
    }
    return result;
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
    VkQueue                                     queue,
    uint32_t                                    submitCount,
    const VkSubmitInfo*                         pSubmits,
    VkFence                                     fence)
{
    VkLayerDispatchTable *pTable = get_dispatch_table(overlay_device_table_map, queue);
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(queue), layer_data_map);

    return pTable->QueueSubmit(queue, submitCount, pSubmits, fence);
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(queue), layer_data_map);

    for (int i = 0; i < pPresentInfo->swapchainCount; i++) {

        auto data = my_data->swapChains->find(pPresentInfo->pSwapchains[i]);
        assert(data != my_data->swapChains->end());

        /* TODO: scribble here */
    }

    VkResult result = my_data->pfnQueuePresentKHR(queue, pPresentInfo);
    return result;
}


void WsiImageData::Cleanup(VkDevice dev)
{
    VkLayerDispatchTable *pTable = get_dispatch_table(overlay_device_table_map, dev);
}


void SwapChainData::Cleanup(VkDevice dev)
{
    for (int i = 0; i < presentableImages.size(); i++) {
        presentableImages[i]->Cleanup(dev);
        delete presentableImages[i];
    }

    presentableImages.clear();
}


void layer_data::Cleanup()
{
}


VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice                                 device,
    VkSwapchainKHR                           swapchain,
    const VkAllocationCallbacks*             pAllocator)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);

    /* Clean up our resources associated with this swapchain */
    auto it = my_data->swapChains->find(swapchain);
    assert(it != my_data->swapChains->end());

    it->second->Cleanup(device);
    delete it->second;
    my_data->swapChains->erase(it->first);

    my_data->pfnDestroySwapchainKHR(device, swapchain, pAllocator);
}


VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char* funcName)
{
    if (dev == NULL)
        return NULL;

    /* loader uses this to force layer initialization; device object is wrapped */
    if (!strcmp("vkGetDeviceProcAddr", funcName)) {
        initDeviceTable(overlay_device_table_map, (const VkBaseLayerObject *) dev);
        return (PFN_vkVoidFunction) vkGetDeviceProcAddr;
    }

#define ADD_HOOK(fn)    \
    if (!strncmp(#fn, funcName, sizeof(#fn))) \
        return (PFN_vkVoidFunction) fn

    ADD_HOOK(vkCreateDevice);
    ADD_HOOK(vkDestroyDevice);
    ADD_HOOK(vkCreateSwapchainKHR);
    ADD_HOOK(vkGetSwapchainImagesKHR);
    ADD_HOOK(vkQueuePresentKHR);
    ADD_HOOK(vkDestroySwapchainKHR);
    ADD_HOOK(vkQueueSubmit);
#undef ADD_HOOK

    VkLayerDispatchTable* pTable = get_dispatch_table(overlay_device_table_map, dev);
    if (pTable->GetDeviceProcAddr == NULL)
        return NULL;
    return pTable->GetDeviceProcAddr(dev, funcName);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* funcName)
{
    if (instance == NULL)
        return NULL;

    if (!strcmp("vkGetInstanceProcAddr", funcName)) {
        initInstanceTable(overlay_instance_table_map, (const VkBaseLayerObject *) instance);
        return (PFN_vkVoidFunction) vkGetInstanceProcAddr;
    }
#define ADD_HOOK(fn)    \
    if (!strncmp(#fn, funcName, sizeof(#fn))) \
        return (PFN_vkVoidFunction) fn

    ADD_HOOK(vkCreateInstance);
    ADD_HOOK(vkDestroyInstance);
#undef ADD_HOOK

    VkLayerInstanceDispatchTable* pTable = get_dispatch_table(overlay_instance_table_map, instance);
    if (pTable->GetInstanceProcAddr == NULL)
        return NULL;
    return pTable->GetInstanceProcAddr(instance, funcName);
}
