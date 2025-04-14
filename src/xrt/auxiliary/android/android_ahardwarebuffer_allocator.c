// Copyright 2020-2025, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief AHardwareBuffer backed image buffer allocator.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Simon Zeni <simon.zeni@collabora.com>
 * @ingroup aux_android
 */

#include "android_ahardwarebuffer_allocator.h"

#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_debug.h"
#include "util/u_handles.h"

#include "xrt/xrt_vulkan_includes.h"

#ifdef XRT_GRAPHICS_BUFFER_HANDLE_IS_AHARDWAREBUFFER
#include <android/hardware_buffer.h>

DEBUG_GET_ONCE_LOG_OPTION(ahardwarebuffer_log, "AHARDWAREBUFFER_LOG", U_LOGGING_WARN)
#define AHB_TRACE(...) U_LOG_IFL_T(debug_get_log_option_ahardwarebuffer_log(), __VA_ARGS__)
#define AHB_DEBUG(...) U_LOG_IFL_D(debug_get_log_option_ahardwarebuffer_log(), __VA_ARGS__)
#define AHB_INFO(...) U_LOG_IFL_I(debug_get_log_option_ahardwarebuffer_log(), __VA_ARGS__)
#define AHB_WARN(...) U_LOG_IFL_W(debug_get_log_option_ahardwarebuffer_log(), __VA_ARGS__)
#define AHB_ERROR(...) U_LOG_IFL_E(debug_get_log_option_ahardwarebuffer_log(), __VA_ARGS__)

static inline enum AHardwareBuffer_Format
vk_format_to_ahardwarebuffer(uint64_t format)
{
	switch (format) {
	case VK_FORMAT_X8_D24_UNORM_PACK32: return AHARDWAREBUFFER_FORMAT_D24_UNORM;
	case VK_FORMAT_D24_UNORM_S8_UINT: return AHARDWAREBUFFER_FORMAT_D24_UNORM_S8_UINT;
	case VK_FORMAT_R5G6B5_UNORM_PACK16: return AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM;
	case VK_FORMAT_D16_UNORM: return AHARDWAREBUFFER_FORMAT_D16_UNORM;
	case VK_FORMAT_R8G8B8_UNORM: return AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM;
	case VK_FORMAT_D32_SFLOAT_S8_UINT: return AHARDWAREBUFFER_FORMAT_D32_FLOAT_S8_UINT;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
	case VK_FORMAT_S8_UINT: return AHARDWAREBUFFER_FORMAT_S8_UINT;
	case VK_FORMAT_D32_SFLOAT: return AHARDWAREBUFFER_FORMAT_D32_FLOAT;
	case VK_FORMAT_R16G16B16A16_SFLOAT: return AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT;
	case VK_FORMAT_R8G8B8A8_SRGB:
		// Format not supported natively by AHB, colorspace must be corrected
	case VK_FORMAT_R8G8B8A8_UNORM: return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
	default: AHB_ERROR("Could not convert 0x%02" PRIx64 " to AHardwareBuffer_Format!", format); return 0;
	}
}

static uint64_t
swapchain_usage_to_ahardwarebuffer(enum xrt_swapchain_usage_bits bits)
{
	uint64_t ahb_usage = 0;
	if (bits & (XRT_SWAPCHAIN_USAGE_SAMPLED | XRT_SWAPCHAIN_USAGE_INPUT_ATTACHMENT)) {
		ahb_usage |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
	}

	if (bits & (XRT_SWAPCHAIN_USAGE_COLOR | XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL)) {
		ahb_usage |= AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;
	}

	if (bits & XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS) {
		ahb_usage |= AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
	}

	// Fallback if no bits are set
	if (ahb_usage == 0) {
		ahb_usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
	}

	return ahb_usage;
}

static bool
ahardwarebuffer_compute_desc(const struct xrt_swapchain_create_info *xsci, AHardwareBuffer_Desc *desc)
{
	desc->format = vk_format_to_ahardwarebuffer(xsci->format);
	if (desc->format == 0) {
		return false;
	}

	desc->width = xsci->width;
	desc->height = xsci->height;
	desc->layers = xsci->array_size;
	desc->usage = swapchain_usage_to_ahardwarebuffer(xsci->bits);

	if (xsci->face_count == 6) {
		desc->usage |= AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP;
		desc->layers *= 6;
	}

	if (xsci->mip_count > 1) {
		desc->usage |= AHARDWAREBUFFER_USAGE_GPU_MIPMAP_COMPLETE;
	}

	if (xsci->create & XRT_SWAPCHAIN_CREATE_PROTECTED_CONTENT) {
		desc->usage |= AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT;
	}

	return true;
}

bool
ahardwarebuffer_is_supported(uint64_t format, enum xrt_swapchain_usage_bits xbits)
{
	// Minimal buffer description to probe support
	const struct xrt_swapchain_create_info xsci = {
	    .bits = xbits,
	    .format = format,
	    .width = 16,
	    .height = 16,
	    .array_size = 1,
	};

	AHardwareBuffer_Desc desc = {0};
	if (!ahardwarebuffer_compute_desc(&xsci, &desc)) {
		return false;
	}

#if __ANDROID_API__ >= 29
	return AHardwareBuffer_isSupported(&desc);
#else
	AHardwareBuffer *buffer;
	int ret = AHardwareBuffer_allocate(&desc, &buffer);
	if (ret != 0) {
		return false;
	}

	AHardwareBuffer_release(buffer);
	return true;
#endif
}

xrt_result_t
ahardwarebuffer_image_allocate(const struct xrt_swapchain_create_info *xsci, xrt_graphics_buffer_handle_t *out_image)
{
	if (!ahardwarebuffer_is_supported(xsci->format, xsci->bits)) {
		return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
	}

	AHardwareBuffer_Desc desc = {0};
	if (!ahardwarebuffer_compute_desc(xsci, &desc)) {
		return XRT_ERROR_ALLOCATION;
	}

	int ret = AHardwareBuffer_allocate(&desc, out_image);
	if (ret != 0) {
		AHB_ERROR("Failed to allocate AHardwareBuffer");
		return XRT_ERROR_ALLOCATION;
	}

	return XRT_SUCCESS;
}

static xrt_result_t
ahardwarebuffer_images_allocate(struct xrt_image_native_allocator *xina,
                                const struct xrt_swapchain_create_info *xsci,
                                size_t image_count,
                                struct xrt_image_native *out_images)
{
	memset(out_images, 0, sizeof(*out_images) * image_count);
	bool failed = false;
	xrt_result_t xret = XRT_SUCCESS;
	for (size_t i = 0; i < image_count; ++i) {
		xret = ahardwarebuffer_image_allocate(xsci, &(out_images[i].handle));
		if (xret != XRT_SUCCESS) {
			failed = true;
			break;
		}
	}
	if (failed) {
		for (size_t i = 0; i < image_count; ++i) {
			u_graphics_buffer_unref(&(out_images[i].handle));
		}
	}
	return xret;
}

static xrt_result_t
ahardwarebuffer_images_free(struct xrt_image_native_allocator *xina,
                            size_t image_count,
                            struct xrt_image_native *images)
{
	for (size_t i = 0; i < image_count; ++i) {
		u_graphics_buffer_unref(&(images[i].handle));
	}
	return XRT_SUCCESS;
}
static void
ahardwarebuffer_destroy(struct xrt_image_native_allocator *xina)
{
	if (xina != NULL) {
		free(xina);
	}
}

struct xrt_image_native_allocator *
android_ahardwarebuffer_allocator_create(void)
{
	struct xrt_image_native_allocator *xina = U_TYPED_CALLOC(struct xrt_image_native_allocator);
	xina->images_allocate = ahardwarebuffer_images_allocate;
	xina->images_free = ahardwarebuffer_images_free;
	xina->destroy = ahardwarebuffer_destroy;
	return xina;
}

#endif // XRT_GRAPHICS_BUFFER_HANDLE_IS_AHARDWAREBUFFER
