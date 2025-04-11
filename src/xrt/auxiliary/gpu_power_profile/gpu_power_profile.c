// Copyright 2024, Patrick Nicolas.
// SPDX-License-Identifier: BSL-1.0

#include "gpu_power_profile.h"

#include "xrt/xrt_config_gpu_power_profile.h"

#include <amdgpu.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <stdio.h>

struct amdgpu_handle
{
	struct gpu_power_profile base;
	amdgpu_device_handle h;
	amdgpu_context_handle ctx;
};

static inline struct amdgpu_handle *
amdgpu_handle(struct gpu_power_profile *base)
{
	return (struct amdgpu_handle *)base;
}

static bool
amdgpu_power_profile_enable(struct gpu_power_profile *base)
{
	struct amdgpu_handle *handle = amdgpu_handle(base);

	U_LOG_D("Enabling amdgpu VR power profile");
	int err = amdgpu_cs_ctx_create(handle->h, &handle->ctx);
	if (err) {
		U_LOG_W("Failed to initialize amdgpu context");
		return false;
	}
	err = amdgpu_cs_ctx_stable_pstate(handle->ctx,
	                                  7, // AMDGPU_CTX_OP_SET_WORKLOAD_PROFILE
	                                  3, // AMDGPU_CTX_WORKLOAD_PROFILE_VR
	                                  NULL);
	if (err) {
		U_LOG_I("Failed to set VR power profile");
		return false;
	}
	return true;
}
static void
amdgpu_power_profile_disable(struct gpu_power_profile *base)
{
	struct amdgpu_handle *handle = amdgpu_handle(base);
	U_LOG_D("Disabling amdgpu VR power profile");
	if (handle->ctx) {
		amdgpu_cs_ctx_free(handle->ctx);
	}
	handle->ctx = NULL;
}
static void
amdgpu_power_profile_destroy(struct gpu_power_profile *base)
{
	struct amdgpu_handle *handle = amdgpu_handle(base);
	if (handle->ctx) {
		amdgpu_cs_ctx_free(handle->ctx);
	}
	amdgpu_device_deinitialize(handle->h);
	free(handle);
}

static struct gpu_power_profile *
amdgpu_power_profile_create(const VkPhysicalDeviceDrmPropertiesEXT *drm_prop)
{
	if (!drm_prop->hasRender) {
		return NULL;
	}

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "/dev/dri/renderD%" PRIi64, drm_prop->renderMinor);

	int drm_fd = open(path, O_RDWR);
	if (drm_fd < 0) {
		U_LOG_W("Failed to open DRI device %s", path);
		return NULL;
	}

	struct amdgpu_handle *handle = U_TYPED_CALLOC(struct amdgpu_handle);
	handle->base.enable = amdgpu_power_profile_enable;
	handle->base.disable = amdgpu_power_profile_disable;
	handle->base.destroy = amdgpu_power_profile_destroy;

	uint32_t major, minor;
	int err = amdgpu_device_initialize(drm_fd, &major, &minor, &handle->h);
	close(drm_fd);
	if (err) {
		U_LOG_W("Failed to initialize amdgpu device");
		goto err;
	}

	U_LOG_D("amdgpu power profile controller initialized");
	return &handle->base;

err:
	if (handle->h) {
		amdgpu_device_deinitialize(handle->h);
	}
	free(handle);
	return NULL;
}

struct gpu_power_profile *
gpu_power_profile_create(struct vk_bundle *vk)
{
#ifdef VK_EXT_physical_device_drm
	if (vk->has_EXT_physical_device_drm) {
		VkPhysicalDeviceDrmPropertiesEXT drm_prop = {
		    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
		};
		VkPhysicalDeviceProperties2 pdp2 = {
		    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		    .pNext = &drm_prop,
		};


		vk->vkGetPhysicalDeviceProperties2(vk->physical_device, &pdp2);

		// AMD GPU
		if (pdp2.properties.vendorID == 0x1002) {
			return amdgpu_power_profile_create(&drm_prop);
		}
	}
#endif

	return NULL;
}
