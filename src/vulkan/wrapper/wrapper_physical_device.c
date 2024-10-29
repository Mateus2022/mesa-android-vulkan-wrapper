#include "wrapper_private.h"
#include "wrapper_trampolines.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_dispatch_table.h"
#include "vk_extensions.h"
#include "vk_physical_device.h"
#include "wsi_common.h"

static VkResult
wrapper_setup_device_extensions(struct wrapper_physical_device *pdevice) {
   struct vk_device_extension_table *exts = &pdevice->vk.supported_extensions;
   VkExtensionProperties pdevice_extensions[VK_DEVICE_EXTENSION_COUNT];
   uint32_t pdevice_extension_count = VK_DEVICE_EXTENSION_COUNT;
   VkResult result;

   result = pdevice->dispatch_table.EnumerateDeviceExtensionProperties(
      pdevice->dispatch_handle, NULL, &pdevice_extension_count, pdevice_extensions);

   if (result != VK_SUCCESS)
      return result;

   *exts = wrapper_device_extensions;

   for (int i = 0; i < pdevice_extension_count; i++) {
      int idx;
      for (idx = 0; idx < VK_DEVICE_EXTENSION_COUNT; idx++) {
         if (strcmp(vk_device_extensions[idx].extensionName,
                     pdevice_extensions[i].extensionName) == 0)
            break;
      }

      if (idx >= VK_DEVICE_EXTENSION_COUNT)
         continue;

      exts->extensions[idx] = true;
   }

   exts->KHR_present_wait = exts->KHR_timeline_semaphore;

   return VK_SUCCESS;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
wrapper_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(vk_physical_device, pdevice, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(pdevice->instance, pName);
}

VkResult enumerate_physical_device(struct vk_instance *_instance)
{
   struct wrapper_instance *instance = (struct wrapper_instance *)_instance;
   VkPhysicalDevice physical_devices[16];
   uint32_t physical_device_count = 16;
   VkResult result;

   result = instance->dispatch_table.EnumeratePhysicalDevices(
      instance->dispatch_handle, &physical_device_count, physical_devices);

   if (result != VK_SUCCESS)
      return result;

   for (int i = 0; i < physical_device_count; i++) {
      PFN_vkGetInstanceProcAddr get_instance_proc_addr;
      struct wrapper_physical_device *pdevice;

      pdevice = vk_zalloc(&_instance->alloc, sizeof(*pdevice), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
      if (!pdevice)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      struct vk_physical_device_dispatch_table dispatch_table;
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wrapper_physical_device_entrypoints, true);
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wsi_physical_device_entrypoints, false);
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wrapper_physical_device_trampolines, false);

      result = vk_physical_device_init(&pdevice->vk,
                                       &instance->vk,
                                       NULL, NULL, NULL,
                                       &dispatch_table);
      if (result != VK_SUCCESS) {
         vk_free(&_instance->alloc, pdevice);
         return result;
      }

      pdevice->instance = instance;
      pdevice->dispatch_handle = physical_devices[i];
      get_instance_proc_addr = instance->dispatch_table.GetInstanceProcAddr;

      vk_physical_device_dispatch_table_load(&pdevice->dispatch_table,
                                             get_instance_proc_addr,
                                             instance->dispatch_handle);

      result = wrapper_setup_device_extensions(pdevice);
      if (result != VK_SUCCESS) {
         vk_physical_device_finish(&pdevice->vk);
         vk_free(&_instance->alloc, pdevice);
         return result;
      }

      result = wsi_device_init(&pdevice->wsi_device,
                               wrapper_physical_device_to_handle(pdevice),
                               wrapper_wsi_proc_addr, &_instance->alloc, -1,
                               NULL, &(struct wsi_device_options){});
      if (result != VK_SUCCESS) {
         vk_physical_device_finish(&pdevice->vk);
         vk_free(&_instance->alloc, pdevice);
         return result;
      }
      pdevice->vk.wsi_device = &pdevice->wsi_device;
      pdevice->wsi_device.force_bgra8_unorm_first = true;
#ifdef __TERMUX__
      pdevice->wsi_device.wants_ahb = true;
#endif

      list_addtail(&pdevice->vk.link, &_instance->physical_devices.list);
   }

   return VK_SUCCESS;
}

void destroy_physical_device(struct vk_physical_device *pdevice) {
   wsi_device_finish(pdevice->wsi_device, &pdevice->instance->alloc);
   vk_physical_device_finish(pdevice);
   vk_free(&pdevice->instance->alloc, pdevice);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                           const char* pLayerName,
                                           uint32_t* pPropertyCount,
                                           VkExtensionProperties* pProperties)
{
   return vk_common_EnumerateDeviceExtensionProperties(physicalDevice,
                                                       pLayerName,
                                                       pPropertyCount,
                                                       pProperties);
}

