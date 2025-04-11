// Copyright 2024, Patrick Nicolas.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Manage GPU power profiles.
 * @author Patrick Nicolas <patricknicolas@laposte.net>
 *
 * @ingroup aux_gpu_power_profile
 */

#pragma once

#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @interface gpu_power_profile
 *
 * Controls VR power profile for a GPU.
 */
struct gpu_power_profile
{
	bool (*enable)(struct gpu_power_profile *handle);
	void (*disable)(struct gpu_power_profile *handle);

	void (*destroy)(struct gpu_power_profile *handle);
};

/*!
 * Enable VR power profile for a GPU
 *
 * @returns false on error, true on success or no-op
 *
 * @public @memberof gpu_power_profile
 */
static inline bool
gpu_power_profile_enable(struct gpu_power_profile *handle)
{
	if (handle) {
		return handle->enable(handle);
	}
	return true;
}

/*!
 * Disable VR power profile for a GPU
 *
 * @public @memberof gpu_power_profile
 */
static inline void
gpu_power_profile_disable(struct gpu_power_profile *handle)
{
	if (handle) {
		handle->disable(handle);
	}
}

/*!
 * Destroy the handle for GPU power profile management
 *
 * @public @memberof gpu_power_profile
 */
static inline void
gpu_power_profile_destroy(struct gpu_power_profile *handle)
{
	if (handle) {
		handle->destroy(handle);
	}
}

/*!
 * Returns a handle to manage GPU power profiles.
 *
 * @returns a handle on success, or NULL if not supported by
 *          os or device, or in case of error.
 *
 * @ingroup aux_gpu_power_profile
 */
struct gpu_power_profile *
gpu_power_profile_create(struct vk_bundle *vk);

#ifdef __cplusplus
} // extern "C"
#endif
