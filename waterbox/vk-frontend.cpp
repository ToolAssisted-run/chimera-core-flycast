// The frontend that Flycast's windowless Vulkan context expects, played by the
// core itself (chimera issue 248).
//
// Flycast has two Vulkan contexts. The desktop one is built around a window
// and its swapchain. The other is the libretro port's (vk_context_lr.cpp): it
// is handed an instance, a device and a queue by its frontend, draws each
// frame into an image of its own and hands that image over. A sandbox has no
// window, so it is the second that this core compiles - and this file is the
// frontend it talks to. The Vulkan device is the one Chimera hands the core:
// every Vulkan command is a wrapper linked in from miniBox (source/vk) that
// carries the call to the frontend, asked for by name.
//
// A load of the machine is a new Vulkan device on Chimera's side: the old one
// is destroyed there, with everything made on it. Flycast still has to let go
// of what it holds, and does it by destroying each object, as it would on a
// desktop. While it does, nothing crosses: every command answers "done" at
// once (the quiet switch), because the object it names is already gone.
#include "rend/vulkan/vulkan_context.h"
#include "hw/pvr/Renderer_if.h"

#include <cstdint>
#include <cstdio>
#include <exception>

extern "C" {
typedef uint64_t (*chimera_vk_bridge_fn)(uint64_t op, uint64_t a, uint64_t b, uint64_t c,
                                         uint64_t d, uint64_t e);
bool chimera_vk_install(chimera_vk_bridge_fn fn);
void *chimera_vk_lookup(const char *name);
uint64_t chimera_vk_context_id(void);
void chimera_vk_forget_mappings(void);
}

static chimera_vk_bridge_fn s_host;
static bool s_installed;
static bool s_quiet;

// the opcodes below 100 are the bridge's own questions (which device is this),
// and are answered whatever the switch says
static uint64_t Bridge(uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
	if (s_quiet && op >= 100)
		return 0;
	return s_host(op, a, b, c, d, e);
}

extern "C" void chimera_vk_bridge_offer(uint64_t addr)
{
	s_host = reinterpret_cast<chimera_vk_bridge_fn>(static_cast<uintptr_t>(addr));
	s_installed = s_host && chimera_vk_install(Bridge);
	if (!s_installed)
		fprintf(stderr, "chimera: the Vulkan bridge was offered and refused\n");
}

extern "C" int chimera_vk_bridge_offered(void)
{
	return s_installed;
}

// which device Chimera has now; 0 when it cannot say
extern "C" uint64_t chimera_vk_device_now(void)
{
	return s_installed ? chimera_vk_context_id() : 0;
}

// ---- the libretro interface, answered by this file -------------------------

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL InstanceProcAddr(VkInstance, const char *name)
{
	return reinterpret_cast<PFN_vkVoidFunction>(chimera_vk_lookup(name));
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DeviceProcAddr(VkDevice, const char *name)
{
	return reinterpret_cast<PFN_vkVoidFunction>(chimera_vk_lookup(name));
}

static VkInstance s_instance = VK_NULL_HANDLE;
static retro_vulkan_context s_device{};
static retro_hw_render_interface_vulkan s_interface{};
// Which of the context's two images the frame being drawn goes into. A real
// frontend moves it when it has shown a frame; here it moves when the core
// has taken one (chimera_vk_frame_taken).
static uint32_t s_image;
static bool s_up;

static void SetImage(void *, const retro_vulkan_image *, uint32_t, const VkSemaphore *, uint32_t) {}
static uint32_t GetSyncIndex(void *) { return s_image; }
static uint32_t GetSyncIndexMask(void *) { return 3; }  // two images
static void SetCommandBuffers(void *, uint32_t, const VkCommandBuffer *) {}
static void WaitSyncIndex(void *) {}
static void LockQueue(void *) {}
static void UnlockQueue(void *) {}
static void SetSignalSemaphore(void *, VkSemaphore) {}

// Makes the instance and the device (the second through Flycast's own
// VkCreateDevice, which asks for the features its renderer wants) and then
// the context on them. False when any of it fails, with the reason on stderr.
extern "C" int chimera_vk_start(void)
{
	if (!s_installed || s_up)
		return s_up;
	const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(chimera_vk_lookup("vkCreateInstance"));
	if (!createInstance)
		return 0;
	VkInstanceCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = VkGetApplicationInfo();
	if (createInstance(&info, nullptr, &s_instance) != VK_SUCCESS)
	{
		fprintf(stderr, "chimera: no Vulkan instance\n");
		return 0;
	}
	try
	{
		s_device = {};
		if (!VkCreateDevice(&s_device, s_instance, VK_NULL_HANDLE, VK_NULL_HANDLE, InstanceProcAddr,
		                    nullptr, 0, nullptr, 0, nullptr))
		{
			fprintf(stderr, "chimera: no Vulkan device\n");
			return 0;
		}
		s_interface = {};
		s_interface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
		s_interface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
		s_interface.handle = &s_interface;
		s_interface.instance = s_instance;
		s_interface.gpu = s_device.gpu;
		s_interface.device = s_device.device;
		s_interface.get_device_proc_addr = DeviceProcAddr;
		s_interface.get_instance_proc_addr = InstanceProcAddr;
		s_interface.queue = s_device.queue;
		s_interface.queue_index = s_device.queue_family_index;
		s_interface.set_image = SetImage;
		s_interface.get_sync_index = GetSyncIndex;
		s_interface.get_sync_index_mask = GetSyncIndexMask;
		s_interface.set_command_buffers = SetCommandBuffers;
		s_interface.wait_sync_index = WaitSyncIndex;
		s_interface.lock_queue = LockQueue;
		s_interface.unlock_queue = UnlockQueue;
		s_interface.set_signal_semaphore = SetSignalSemaphore;
		s_image = 0;
		VulkanContext::Create(&s_interface);
	}
	catch (const std::exception &e)
	{
		fprintf(stderr, "chimera: the Vulkan context would not start: %s\n", e.what());
		return 0;
	}
	s_up = true;
	return 1;
}

extern "C" int chimera_vk_available(void)
{
	return s_up;
}

// The core has taken the frame (or let it go): the next one goes into the
// context's other image.
extern "C" void chimera_vk_frame_taken(void)
{
	s_image ^= 1;
}

// Lets go of the renderer, the context, the device and the instance, in the
// order a libretro frontend takes a context away.
static void Stop()
{
	rend_term_renderer();
	GraphicsContext::Term();
	if (s_device.device != VK_NULL_HANDLE)
	{
		const auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(chimera_vk_lookup("vkDestroyDevice"));
		if (destroyDevice)
			destroyDevice(s_device.device, nullptr);
	}
	if (s_instance != VK_NULL_HANDLE)
	{
		const auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(chimera_vk_lookup("vkDestroyInstance"));
		if (destroyInstance)
			destroyInstance(s_instance, nullptr);
	}
	s_device = {};
	s_instance = VK_NULL_HANDLE;
	s_up = false;
}

// After a load: everything made on the device that is gone is let go of with
// nothing crossing the bridge, the copies of mapped memory nothing will unmap
// now are given back, and the device, the context and the renderer are made
// again. False when they could not be.
extern "C" int chimera_vk_rebuild(void)
{
	s_quiet = true;
	Stop();
	chimera_vk_forget_mappings();
	s_quiet = false;
	if (!chimera_vk_start())
		return 0;
	return rend_init_renderer() ? 1 : 0;
}
