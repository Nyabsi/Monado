// Copyright 2023-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Utility for swapchain waiting (CPU-side)
 * @author Christoph Haag <christoph.haag@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup comp_util
 */

#pragma once

#include "util/u_logging.h"
#include "util/u_threading.h"
#include "xrt/xrt_compositor.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * A single, waitable swapchain image, holds the needed state for tracking image usage.
 *
 * Only provides for CPU waiting.
 *
 * @ingroup comp_util
 * @see comp_swapchain_waiting
 */
struct comp_swapchain_image_wait_state
{
	//! A usage counter, similar to a reference counter.
	uint32_t use_count;

	//! A condition variable per swapchain image that is notified when @ref use_count count reaches 0.
	pthread_cond_t use_cond;

	//! A mutex per swapchain image that is used with @ref use_cond.
	struct os_mutex use_mutex;
};

/*!
 * State for implementing a (CPU) waitable swapchain.
 *
 * Assumes you've already done any GPU-side waiting before you decrement the image use count.
 *
 * Provides primitives suitable for implementing:
 *
 * - @ref xrt_swapchain_native::inc_image_use
 * - @ref xrt_swapchain_native::dec_image_use
 * - @ref xrt_swapchain_native::wait_image
 *
 * @ingroup comp_util
 *
 */
struct comp_swapchain_waiting
{
	struct comp_swapchain_image_wait_state wait_images[XRT_MAX_SWAPCHAIN_IMAGES];
	enum u_logging_level log_level;
	uint32_t image_count;
};

/*!
 * Initialize @ref comp_swapchain_waiting
 *
 * @param cscw self
 * @param log_level Logging level
 * @param image_count Number of images in the swapchain
 * @return xrt_result_t
 *
 * @public @memberof comp_swapchain_waiting
 */
xrt_result_t
comp_swapchain_waiting_init(struct comp_swapchain_waiting *cscw, enum u_logging_level log_level, uint32_t image_count);

/*!
 * Clean up resources allocated for @ref comp_swapchain_waiting
 *
 * @param cscw self
 * @return xrt_result_t
 *
 * @public @memberof comp_swapchain_waiting
 */
xrt_result_t
comp_swapchain_waiting_fini(struct comp_swapchain_waiting *cscw);

/*!
 * Increment the usage counter for a swapchain image index.
 *
 * @param cscw self
 * @param index image index to increment
 * @return xrt_result_t
 *
 * @public @memberof comp_swapchain_waiting
 */
xrt_result_t
comp_swapchain_waiting_inc_image_use(struct comp_swapchain_waiting *cscw, uint32_t index);

/*!
 * Decrement the usage counter for a swapchain image index.
 *
 * @param cscw self
 * @param image index to decrement
 * @return xrt_result_t
 *
 * @public @memberof comp_swapchain_waiting
 */
xrt_result_t
comp_swapchain_waiting_dec_image_use(struct comp_swapchain_waiting *cscw, uint32_t index);

/*!
 * Wait for the given swapchain image to be available (not used).
 *
 * @param cscw self
 * @param timeout_ns how long to wait
 * @param index image index to wait for.
 * @return xrt_result_t
 *
 * @public @memberof comp_swapchain_waiting
 */
xrt_result_t
comp_swapchain_waiting_wait_image(struct comp_swapchain_waiting *cscw, int64_t timeout_ns, uint32_t index);

#ifdef __cplusplus
}
#endif
