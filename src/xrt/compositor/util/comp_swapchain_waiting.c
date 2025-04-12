// Copyright 2023-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Utility for swapchain waiting (CPU-side)
 * @author Christoph Haag <christoph.haag@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup comp_util
 */
#include "xrt/xrt_config_os.h"
#include "xrt/xrt_results.h"

#include "util/u_misc.h"
#include "util/u_trace_marker.h"

#include "util/comp_swapchain_waiting.h"

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>
#include <assert.h>

#define CSCW_TRACE(d, ...) U_LOG_IFL_T(d->log_level, __VA_ARGS__)
#define CSCW_DEBUG(d, ...) U_LOG_IFL_D(d->log_level, __VA_ARGS__)
#define CSCW_INFO(d, ...) U_LOG_IFL_I(d->log_level, __VA_ARGS__)
#define CSCW_WARN(d, ...) U_LOG_IFL_W(d->log_level, __VA_ARGS__)
#define CSCW_ERROR(d, ...) U_LOG_IFL_E(d->log_level, __VA_ARGS__)


xrt_result_t
comp_swapchain_waiting_inc_image_use(struct comp_swapchain_waiting *cscw, uint32_t index)
{
	SWAPCHAIN_TRACE_BEGIN(comp_swapchain_waiting_inc_image_use);

	CSCW_TRACE(cscw, "%p INC_IMAGE %d (use %d)", (void *)cscw, index, cscw->wait_images[index].use_count);

	os_mutex_lock(&cscw->wait_images[index].use_mutex);
	cscw->wait_images[index].use_count++;
	os_mutex_unlock(&cscw->wait_images[index].use_mutex);

	SWAPCHAIN_TRACE_END(comp_swapchain_waiting_inc_image_use);

	return XRT_SUCCESS;
}

xrt_result_t
comp_swapchain_waiting_dec_image_use(struct comp_swapchain_waiting *cscw, uint32_t index)
{
	SWAPCHAIN_TRACE_BEGIN(comp_swapchain_waiting_dec_image_use);

	CSCW_TRACE(cscw, "%p DEC_IMAGE %d (use %d)", (void *)cscw, index, cscw->wait_images[index].use_count);

	os_mutex_lock(&cscw->wait_images[index].use_mutex);

	assert(cscw->wait_images[index].use_count > 0 && "use count already 0");

	cscw->wait_images[index].use_count--;
	if (cscw->wait_images[index].use_count == 0) {
		os_mutex_unlock(&cscw->wait_images[index].use_mutex);
		pthread_cond_broadcast(&cscw->wait_images[index].use_cond);
	}

	os_mutex_unlock(&cscw->wait_images[index].use_mutex);

	SWAPCHAIN_TRACE_END(comp_swapchain_waiting_dec_image_use);

	return XRT_SUCCESS;
}

xrt_result_t
comp_swapchain_waiting_wait_image(struct comp_swapchain_waiting *cscw, int64_t timeout_ns, uint32_t index)
{
	SWAPCHAIN_TRACE_BEGIN(comp_swapchain_waiting_wait_image);

	CSCW_TRACE(cscw, "%p WAIT_IMAGE %d (use %d)", (void *)cscw, index, cscw->wait_images[index].use_count);

	os_mutex_lock(&cscw->wait_images[index].use_mutex);

	if (cscw->wait_images[index].use_count == 0) {
		CSCW_TRACE(cscw, "%p WAIT_IMAGE %d: NO WAIT", (void *)cscw, index);
		os_mutex_unlock(&cscw->wait_images[index].use_mutex);
		SWAPCHAIN_TRACE_END(comp_swapchain_waiting_wait_image);
		return XRT_SUCCESS;
	}

	// on windows pthread_cond_timedwait can not be used with monotonic time
	int64_t start_wait_rt = os_realtime_get_ns();

	int64_t end_wait_rt;
	// don't wrap on big or indefinite timeout
	if (start_wait_rt > INT64_MAX - timeout_ns) {
		end_wait_rt = INT64_MAX;
	} else {
		end_wait_rt = start_wait_rt + timeout_ns;
	}

	struct timespec spec;
	os_ns_to_timespec(end_wait_rt, &spec);

	CSCW_TRACE(cscw, "%p WAIT_IMAGE %d (use %d) start wait at: %" PRIu64 " (timeout at %" PRIu64 ")", (void *)cscw,
	           index, cscw->wait_images[index].use_count, start_wait_rt, end_wait_rt);

	int ret = 0;
	while (cscw->wait_images[index].use_count > 0) {
		// use pthread_cond_timedwait to implement timeout behavior
		ret = pthread_cond_timedwait(&cscw->wait_images[index].use_cond,
		                             &cscw->wait_images[index].use_mutex.mutex, &spec);

		int64_t now_rt = os_realtime_get_ns();
		double diff = time_ns_to_ms_f(now_rt - start_wait_rt);

		if (ret == 0) {

			if (cscw->wait_images[index].use_count == 0) {
				// image became available within timeout limits
				CSCW_TRACE(cscw, "%p WAIT_IMAGE %d: success at %" PRIu64 " after %fms", (void *)cscw,
				           index, now_rt, diff);
				os_mutex_unlock(&cscw->wait_images[index].use_mutex);
				SWAPCHAIN_TRACE_END(comp_swapchain_waiting_wait_image);
				return XRT_SUCCESS;
			}
			// cond got signaled but image is still in use, continue waiting
			CSCW_TRACE(cscw, "%p WAIT_IMAGE %d: woken at %" PRIu64 " after %fms but still (%d use)",
			           (void *)cscw, index, now_rt, diff, cscw->wait_images[index].use_count);
			continue;
		}

		if (ret == ETIMEDOUT) {
			CSCW_TRACE(cscw, "%p WAIT_IMAGE %d (use %d): timeout at %" PRIu64 " after %fms", (void *)cscw,
			           index, cscw->wait_images[index].use_count, now_rt, diff);

			if (now_rt >= end_wait_rt) {
				// image did not become available within timeout limits
				CSCW_TRACE(cscw, "%p WAIT_IMAGE %d (use %d): timeout (%" PRIu64 " > %" PRIu64 ")",
				           (void *)cscw, index, cscw->wait_images[index].use_count, now_rt,
				           end_wait_rt);
				os_mutex_unlock(&cscw->wait_images[index].use_mutex);
				SWAPCHAIN_TRACE_END(comp_swapchain_waiting_wait_image);
				return XRT_TIMEOUT;
			}
			// spurious cond wakeup
			CSCW_TRACE(cscw, "%p WAIT_IMAGE %d (use %d): spurious timeout at %" PRIu64 " (%fms to timeout)",
			           (void *)cscw, index, cscw->wait_images[index].use_count, now_rt,
			           time_ns_to_ms_f(end_wait_rt - now_rt));
			continue;
		}

		// if no other case applied
		CSCW_TRACE(cscw, "%p WAIT_IMAGE %d: condition variable error %d", (void *)cscw, index, ret);
		os_mutex_unlock(&cscw->wait_images[index].use_mutex);
		SWAPCHAIN_TRACE_END(comp_swapchain_waiting_wait_image);
		return XRT_ERROR_VULKAN;
	}

	CSCW_TRACE(cscw, "%p WAIT_IMAGE %d: became available before spurious wakeup %d", (void *)cscw, index, ret);

	os_mutex_unlock(&cscw->wait_images[index].use_mutex);
	SWAPCHAIN_TRACE_END(comp_swapchain_waiting_wait_image);

	return XRT_SUCCESS;
}

xrt_result_t
comp_swapchain_waiting_init(struct comp_swapchain_waiting *cscw, enum u_logging_level log_level, uint32_t image_count)
{
	assert(cscw != NULL);
	assert(image_count > 0);
	*cscw = (struct comp_swapchain_waiting){
	    .log_level = log_level,
	    .image_count = image_count,
	};

	xrt_result_t xret = XRT_SUCCESS;
	// Init all of the threading objects.
	for (uint32_t i = 0; i < image_count; i++) {

		int ret = pthread_cond_init(&cscw->wait_images[i].use_cond, NULL);
		if (ret) {
			CSCW_ERROR(cscw, "Failed to init image use cond: %d", ret);
			xret = XRT_ERROR_THREADING_INIT_FAILURE;
			continue;
		}

		ret = os_mutex_init(&cscw->wait_images[i].use_mutex);
		if (ret) {
			CSCW_ERROR(cscw, "Failed to init image use mutex: %d", ret);
			xret = XRT_ERROR_THREADING_INIT_FAILURE;
			continue;
		}

		cscw->wait_images[i].use_count = 0;
	}

	return xret;
}

xrt_result_t
comp_swapchain_waiting_fini(struct comp_swapchain_waiting *cscw)
{
	assert(cscw != NULL);

	const uint32_t image_count = cscw->image_count;
	for (uint32_t i = 0; i < image_count; i++) {
		// compositor ensures to garbage collect after gpu work finished
		if (cscw->wait_images[i].use_count != 0) {
			CSCW_ERROR(cscw, "swapchain destroy while image %d use count %d", i,
			           cscw->wait_images[i].use_count);
			assert(false);
			continue; // leaking better than crashing?
		}

		os_mutex_destroy(&cscw->wait_images[i].use_mutex);
		pthread_cond_destroy(&cscw->wait_images[i].use_cond);
	}
	return XRT_SUCCESS;
}
