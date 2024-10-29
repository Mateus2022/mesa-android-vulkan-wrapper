#include "vk_util.h"
#include "wrapper_private.h"
#include "wrapper_entrypoints.h"
#include "wrapper_trampolines.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_device.h"
#include "vk_dispatch_table.h"
#include "vk_extensions.h"
#include "vk_queue.h"
#include "util/list.h"

const struct vk_device_extension_table wrapper_device_extensions = {
   .KHR_swapchain = true,
   .EXT_swapchain_maintenance1 = true,
   .KHR_swapchain_mutable_format = true,
#ifdef VK_USE_PLATFORM_DISPLAY_KHR
   .EXT_display_control = true,
#endif
   .KHR_present_id = true,
   .KHR_present_wait = true,
   .KHR_incremental_present = true,
};

static inline void
set_swapchain_required_extensions(const struct vk_device *device,
                                  uint32_t *enable_extension_count,
                                  const char **enable_extensions) {
   uint32_t count = *enable_extension_count;
#define REQUIRED_EXTENSION(name) \
   assert (count < VK_DEVICE_EXTENSION_COUNT); \
   if (!device->enabled_extensions.name && \
       device->physical->supported_extensions.name) { \
      enable_extensions[count++] = "VK_" #name; \
   }
   REQUIRED_EXTENSION(KHR_external_fence);
   REQUIRED_EXTENSION(KHR_external_semaphore);
   REQUIRED_EXTENSION(KHR_external_memory);
   REQUIRED_EXTENSION(KHR_external_fence_fd);
   REQUIRED_EXTENSION(KHR_external_semaphore_fd);
   REQUIRED_EXTENSION(KHR_external_memory_fd);
   REQUIRED_EXTENSION(KHR_dedicated_allocation);
   REQUIRED_EXTENSION(EXT_queue_family_foreign);
   REQUIRED_EXTENSION(KHR_maintenance1)
   REQUIRED_EXTENSION(KHR_timeline_semaphore);
   REQUIRED_EXTENSION(EXT_external_memory_dma_buf);
   REQUIRED_EXTENSION(EXT_image_drm_format_modifier);
   REQUIRED_EXTENSION(ANDROID_external_memory_android_hardware_buffer);
#undef REQUIRED_EXTENSION
   *enable_extension_count = count;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateDevice(VkPhysicalDevice physicalDevice,
                     const VkDeviceCreateInfo* pCreateInfo,
                     const VkAllocationCallbacks* pAllocator,
                     VkDevice* pDevice)
{
   VK_FROM_HANDLE(wrapper_physical_device, physical_device, physicalDevice);
   const char *wrapper_enable_extensions[VK_DEVICE_EXTENSION_COUNT];
   uint32_t wrapper_enable_extension_count = 0;
   VkDeviceCreateInfo wrapper_create_info = *pCreateInfo;
   struct vk_device_dispatch_table dispatch_table;
   struct wrapper_device *device;
   VkResult result;

   device = vk_zalloc2(&physical_device->instance->vk.alloc, pAllocator,
                       sizeof(*device), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   if (!device)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wrapper_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wrapper_device_trampolines, false);

   result = vk_device_init(&device->vk, &physical_device->vk,
                           &dispatch_table, pCreateInfo, pAllocator);

   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, device);
      return vk_error(physical_device, result);
   }

   for (int idx = 0; idx < VK_DEVICE_EXTENSION_COUNT; idx++) {
      if (wrapper_device_extensions.extensions[idx])
         continue;

      if (!device->vk.enabled_extensions.extensions[idx])
         continue;

      wrapper_enable_extensions[wrapper_enable_extension_count++] =
         vk_device_extensions[idx].extensionName;
   }

   if (device->vk.enabled_extensions.KHR_swapchain) {
      set_swapchain_required_extensions(&device->vk,
                                        &wrapper_enable_extension_count,
                                        wrapper_enable_extensions);
   }

   wrapper_create_info.enabledExtensionCount = wrapper_enable_extension_count;
   wrapper_create_info.ppEnabledExtensionNames = wrapper_enable_extensions;

   result = physical_device->dispatch_table.CreateDevice(
      physical_device->dispatch_handle, &wrapper_create_info, pAllocator,
      &device->dispatch_handle);

   if (result != VK_SUCCESS) {
      vk_device_finish(&device->vk);
      vk_free2(&physical_device->instance->vk.alloc, pAllocator, device);
      return vk_error(physical_device, result);
   }

   void *gdpa = physical_device->instance->dispatch_table.GetInstanceProcAddr(
      physical_device->instance->dispatch_handle, "vkGetDeviceProcAddr");
   vk_device_dispatch_table_load(&device->dispatch_table, gdpa,
                                 device->dispatch_handle);

   for (int i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo *create_info = &pCreateInfo->pQueueCreateInfos[i];
      for (int j = 0; j < create_info->queueCount; j++) {
         struct wrapper_queue *queue;
         queue = vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*queue), 8,
                            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
         if (!queue)
            goto fail_queue;

         device->dispatch_table.GetDeviceQueue(device->dispatch_handle,
                                               create_info->queueFamilyIndex,
                                               j, &queue->dispatch_handle);;
         queue->device = device;

         result = vk_queue_init(&queue->vk, &device->vk, create_info, j);
         if (result != VK_SUCCESS) {
            vk_free2(&device->vk.alloc, pAllocator, queue);
            goto fail_queue;
         }
      }
   }
   list_inithead(&device->command_buffers);

   *pDevice = wrapper_device_to_handle(device);

   return VK_SUCCESS;

fail_queue:
   list_for_each_entry_safe(struct vk_queue, queue, &device->vk.queues, link) {
      vk_queue_finish(queue);
      vk_free2(&device->vk.alloc, pAllocator, queue);
   }
   device->dispatch_table.DestroyDevice(device->dispatch_handle, pAllocator);
   vk_device_finish(&device->vk);
   vk_free2(&physical_device->instance->vk.alloc, pAllocator, device);
   return vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                       uint32_t queueIndex, VkQueue* pQueue) {
   vk_common_GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo,
                        VkQueue* pQueue) {
   vk_common_GetDeviceQueue2(device, pQueueInfo, pQueue);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
wrapper_GetDeviceProcAddr(VkDevice _device, const char* pName) {
   VK_FROM_HANDLE(wrapper_device, device, _device);
   return vk_device_get_proc_addr(&device->vk, pName);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_QueueSubmit(VkQueue _queue, uint32_t submitCount,
                    const VkSubmitInfo* pSubmits, VkFence fence)
{
   VK_FROM_HANDLE(wrapper_queue, queue, _queue);
   VkCommandBuffer wrapper_command_buffers[submitCount][32];
   VkSubmitInfo wrapper_submits[submitCount];

   for (int i = 0; i < submitCount; i++) {
      const VkSubmitInfo *submit_info = &pSubmits[i];
      for (int j = 0; j < submit_info->commandBufferCount; j++) {
         VK_FROM_HANDLE(wrapper_command_buffer, wcb,
                        submit_info->pCommandBuffers[j]);
         wrapper_command_buffers[i][j] = wcb->dispatch_handle;
      }
      wrapper_submits[i] = pSubmits[i];
      wrapper_submits[i].pCommandBuffers = wrapper_command_buffers[i];
   }
   return queue->device->dispatch_table.QueueSubmit(queue->dispatch_handle,
                                                    submitCount,
                                                    wrapper_submits,
                                                    fence);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_QueueSubmit2(VkQueue _queue, uint32_t submitCount,
                     const VkSubmitInfo2* pSubmits, VkFence fence)
{
   VK_FROM_HANDLE(wrapper_queue, queue, _queue);
   VkCommandBufferSubmitInfo wrapper_command_buffers[submitCount][32];
   VkSubmitInfo2 wrapper_submits[submitCount];

   for (int i = 0; i < submitCount; i++) {
      const VkSubmitInfo2 *submit_info = &pSubmits[i];
      for (int j = 0; j < submit_info->commandBufferInfoCount; j++) {
         VK_FROM_HANDLE(wrapper_command_buffer, wcb,
                        submit_info->pCommandBufferInfos[j].commandBuffer);
         wrapper_command_buffers[i][j] = pSubmits[i].pCommandBufferInfos[j];
         wrapper_command_buffers[i][j].commandBuffer = wcb->dispatch_handle;
      }
      wrapper_submits[i] = pSubmits[i];
      wrapper_submits[i].pCommandBufferInfos = wrapper_command_buffers[i];
   }
   return queue->device->dispatch_table.QueueSubmit2(queue->dispatch_handle,
                                                     submitCount,
                                                     wrapper_submits,
                                                     fence);
}

static VkResult
wrapper_command_buffer_create(struct wrapper_device *device,
                              VkCommandPool pool,
                              VkCommandBuffer dispatch_handle,
                              VkCommandBuffer *pCommandBuffers) {
   struct wrapper_command_buffer *wrapper_command_buffer;
   wrapper_command_buffer = vk_object_zalloc(&device->vk, &device->vk.alloc,
                              sizeof(struct wrapper_command_buffer),
                              VK_OBJECT_TYPE_COMMAND_BUFFER);
   if (!wrapper_command_buffer)
      return vk_error(&device->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   wrapper_command_buffer->device = device;
   wrapper_command_buffer->pool = pool;
   wrapper_command_buffer->dispatch_handle = dispatch_handle;
   list_add(&wrapper_command_buffer->link, &device->command_buffers);

   *pCommandBuffers = wrapper_command_buffer_to_handle(wrapper_command_buffer);

   return VK_SUCCESS;
}

static void
wrapper_command_buffer_destroy(struct wrapper_device *device,
                               struct wrapper_command_buffer *wcb) {
   list_del(&wcb->link);
   vk_object_free(&device->vk, NULL, wcb);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_AllocateCommandBuffers(VkDevice _device,
                               const VkCommandBufferAllocateInfo* pAllocateInfo,
                               VkCommandBuffer* pCommandBuffers)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   VkCommandBuffer dispatch_handles[pAllocateInfo->commandBufferCount];
   VkResult result;
   uint32_t i;
   
   result = device->dispatch_table.AllocateCommandBuffers(device->dispatch_handle,
                                                          pAllocateInfo,
                                                          dispatch_handles);
   if (result != VK_SUCCESS)
      return result;

   for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {
      result = wrapper_command_buffer_create(device,
                                             pAllocateInfo->commandPool,
                                             dispatch_handles[i],
                                             &pCommandBuffers[i]);
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      device->dispatch_table.FreeCommandBuffers(device->dispatch_handle,
                                                pAllocateInfo->commandPool,
                                                pAllocateInfo->commandBufferCount,
                                                dispatch_handles);
      for (int q = 0; q < i; q++) {
         VK_FROM_HANDLE(wrapper_command_buffer, wcb, pCommandBuffers[q]);
         wrapper_command_buffer_destroy(device, wcb);
      }

      for (i = 0; i < pAllocateInfo->commandBufferCount; i++)
         pCommandBuffers[i] = VK_NULL_HANDLE;

      return result;
   }

   return VK_SUCCESS;
}


VKAPI_ATTR void VKAPI_CALL
wrapper_FreeCommandBuffers(VkDevice _device,
                           VkCommandPool commandPool,
                           uint32_t commandBufferCount,
                           const VkCommandBuffer* pCommandBuffers)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   VkCommandBuffer dispatch_handles[commandBufferCount];

   for (int i = 0; i < commandBufferCount; i++) {
      VK_FROM_HANDLE(wrapper_command_buffer, wcb, pCommandBuffers[i]);
      dispatch_handles[i] = wcb->dispatch_handle;
      wrapper_command_buffer_destroy(device, wcb);
   }
   device->dispatch_table.FreeCommandBuffers(device->dispatch_handle,
                                             commandPool, commandBufferCount,
                                             dispatch_handles);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroyCommandPool(VkDevice _device, VkCommandPool commandPool,
                           const VkAllocationCallbacks* pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   list_for_each_entry_safe(struct wrapper_command_buffer, wcb,
                            &device->command_buffers, link) {
      if (wcb->pool == commandPool) {
         wrapper_command_buffer_destroy(device, wcb);
      }
   }
   device->dispatch_table.DestroyCommandPool(device->dispatch_handle,
                                             commandPool, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroyDevice(VkDevice _device, const VkAllocationCallbacks* pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   list_for_each_entry_safe(struct wrapper_command_buffer, wcb,
                            &device->command_buffers, link) {
      wrapper_command_buffer_destroy(device, wcb);
   }
   list_for_each_entry_safe(struct vk_queue, queue, &device->vk.queues, link) {
      vk_queue_finish(queue);
      vk_free2(&device->vk.alloc, pAllocator, queue);
   }
   device->dispatch_table.DestroyDevice(device->dispatch_handle, pAllocator);
   vk_device_finish(&device->vk);
   vk_free2(&device->vk.alloc, pAllocator, device);
}

