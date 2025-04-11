// Copyright 2023, Duncan Spaulding.
// Copyright 2022-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Builder for SteamVR proprietary driver wrapper.
 * @author BabbleBones <BabbleBones@protonmail.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup xrt_iface
 */
#include "util/u_builders.h"
#include "xrt/xrt_config_drivers.h"


#include <assert.h>
#include <stdbool.h>

#include "tracking/t_tracking.h"

#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_prober.h"

#include "util/u_debug.h"
#include "util/u_system_helpers.h"

#include "vive/vive_builder.h"
#include "vive/vive_config.h"

#include "target_builder_interface.h"

#include "steamvr_lh/steamvr_lh_interface.h"
#include "xrt/xrt_results.h"

#include "xrt/xrt_space.h"
#include "util/u_space_overseer.h"
#include "vive/vive_calibration.h"
#include "tracking/t_hand_tracking.h"
#include "ht/ht_interface.h"
#include "multi_wrapper/multi.h"
#include "ht_ctrl_emu/ht_ctrl_emu_interface.h"
#include "util/u_sink.h"
#include "xrt/xrt_frameserver.h"

#ifndef XRT_BUILD_DRIVER_STEAMVR_LIGHTHOUSE
#error "This builder requires the SteamVR Lighthouse driver"
#endif

DEBUG_GET_ONCE_LOG_OPTION(svr_log, "STEAMVR_LH_LOG", U_LOGGING_INFO)

#define SVR_TRACE(...) U_LOG_IFL_T(debug_get_log_option_svr_log(), __VA_ARGS__)
#define SVR_DEBUG(...) U_LOG_IFL_D(debug_get_log_option_svr_log(), __VA_ARGS__)
#define SVR_INFO(...) U_LOG_IFL_I(debug_get_log_option_svr_log(), __VA_ARGS__)
#define SVR_WARN(...) U_LOG_IFL_W(debug_get_log_option_svr_log(), __VA_ARGS__)
#define SVR_ERROR(...) U_LOG_IFL_E(debug_get_log_option_svr_log(), __VA_ARGS__)
#define SVR_ASSERT(predicate, ...)                                                                                     \
	do {                                                                                                           \
		bool p = predicate;                                                                                    \
		if (!p) {                                                                                              \
			U_LOG(U_LOGGING_ERROR, __VA_ARGS__);                                                           \
			assert(false && "SVR_ASSERT failed: " #predicate);                                             \
			exit(EXIT_FAILURE);                                                                            \
		}                                                                                                      \
	} while (false);
#define SVR_ASSERT_(predicate) SVR_ASSERT(predicate, "Assertion failed " #predicate)


/*
 *
 * Misc stuff.
 *
 */

DEBUG_GET_ONCE_BOOL_OPTION(steamvr_enable, "STEAMVR_LH_ENABLE", false)
DEBUG_GET_ONCE_TRISTATE_OPTION(lh_handtracking, "LH_HANDTRACKING")

static const char *driver_list[] = {
    "steamvr_lh",
};

struct steamvr_builder
{
	struct xrt_builder base;

	struct xrt_device *head;
	struct xrt_device *left_ht, *right_ht;

	bool is_valve_index;

	struct xrt_frame_context *xfctx;
	struct xrt_fs *xfs;
};

/*
 *
 * Member functions.
 *
 */

static xrt_result_t
steamvr_estimate_system(struct xrt_builder *xb,
                        cJSON *config,
                        struct xrt_prober *xp,
                        struct xrt_builder_estimate *estimate)
{
	struct steamvr_builder *svrb = (struct steamvr_builder *)xb;

#ifdef XRT_BUILD_DRIVER_HANDTRACKING
	bool have_hand_tracking = true;
#else
	bool have_hand_tracking = false;
#endif

	if (debug_get_bool_option_steamvr_enable()) {
		return vive_builder_estimate( //
		    xp,                       // xp
		    true,                     // have_6dof
		    have_hand_tracking,       // have_hand_tracking
		    &svrb->is_valve_index,    // out_have_valve_index
		    estimate);                // out_estimate
	} else {
		return XRT_SUCCESS;
	}
}

static void
steamvr_destroy(struct xrt_builder *xb)
{
	struct steamvr_builder *svrb = (struct steamvr_builder *)xb;
	free(svrb);
}

static uint32_t
get_selected_mode(struct xrt_fs *xfs)
{
	struct xrt_fs_mode *modes = NULL;
	uint32_t count = 0;
	xrt_fs_enumerate_modes(xfs, &modes, &count);

	SVR_ASSERT(count != 0, "No stream modes found in Index camera");

	uint32_t selected_mode = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (modes[i].format == XRT_FORMAT_YUYV422) {
			selected_mode = i;
			break;
		}
	}

	free(modes);
	return selected_mode;
}

static void
on_video_device(struct xrt_prober *xp,
                struct xrt_prober_device *pdev,
                const char *product,
                const char *manufacturer,
                const char *serial,
                void *ptr)
{
	struct steamvr_builder *svrb = (struct steamvr_builder *)ptr;

	// Hardcoded for the Index.
	if (product != NULL && manufacturer != NULL) {
		if ((strcmp(product, "3D Camera") == 0) && (strcmp(manufacturer, "Etron Technology, Inc.") == 0)) {
			xrt_prober_open_video_device(xp, pdev, svrb->xfctx, &svrb->xfs);
			return;
		}
	}
}

static bool
stream_data_sources(struct steamvr_builder *svrb, struct xrt_prober *xp, struct xrt_slam_sinks sinks)
{
	// Open frame server
	xrt_prober_list_video_devices(xp, on_video_device, svrb);
	if (svrb->xfs == NULL) {
		SVR_WARN("Couldn't find Index camera at all. Is it plugged in?");
		return false;
	}

	uint32_t mode = get_selected_mode(svrb->xfs);

	bool bret = xrt_fs_stream_start(svrb->xfs, sinks.cams[0], XRT_FS_CAPTURE_TYPE_TRACKING, mode);
	if (!bret) {
		SVR_ERROR("Unable to start data streaming");
		return false;
	}

	SVR_DEBUG("Started camera data streaming");

	return true;
}


static xrt_result_t
steamvr_open_system(struct xrt_builder *xb,
                    cJSON *config,
                    struct xrt_prober *xp,
                    struct xrt_session_event_sink *broadcast,
                    struct xrt_system_devices **out_xsysd,
                    struct xrt_space_overseer **out_xso)
{
	struct steamvr_builder *svrb = (struct steamvr_builder *)xb;

	assert(out_xsysd != NULL);
	assert(*out_xsysd == NULL);

	struct vive_config hmd_config = {0};

	bool found_controllers;
	enum xrt_result result = steamvr_lh_create_devices(out_xsysd, &hmd_config, &found_controllers);

	if (result != XRT_SUCCESS) {
		SVR_ERROR("Unable to create devices");
		return result;
	}

#ifdef XRT_BUILD_DRIVER_HANDTRACKING
	bool have_hand_tracking = true;
#else
	bool have_hand_tracking = false;
#endif

	enum debug_tristate_option hand_wanted = debug_get_tristate_option_lh_handtracking();

	struct xrt_system_devices *xsysd = NULL;
	xsysd = *out_xsysd;

	if (xsysd->static_roles.head == NULL) {
		SVR_ERROR("Unable to find HMD");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	svrb->head = xsysd->static_roles.head;

	struct xrt_device *left_ht = NULL, *right_ht = NULL;

	left_ht = u_system_devices_get_ht_device_left(xsysd);
	right_ht = u_system_devices_get_ht_device_right(xsysd);

	bool hand_enabled = false;

	svrb->xfctx = &u_system_devices(xsysd)->xfctx;

	SVR_INFO("a\n");

#ifdef XRT_BUILD_DRIVER_HANDTRACKING
	SVR_INFO("is valve index: %d\n", svrb->is_valve_index);

	if (svrb->is_valve_index && have_hand_tracking) {
		if (hand_wanted == DEBUG_TRISTATE_ON) {
			hand_enabled = true;
		} else if (hand_wanted == DEBUG_TRISTATE_AUTO) {
			hand_enabled = !found_controllers;
		}
	}

	SVR_INFO("hand wanted: %d, hand_enabled: %d\n", hand_wanted, hand_enabled);

	if (hand_enabled && svrb->head != NULL) {
		struct xrt_device *hand_devices[2] = {NULL};
		struct xrt_slam_sinks *sinks = NULL;

		// Hand tracking calibration
		struct t_stereo_camera_calibration *stereo_calib = NULL;
		struct xrt_pose head_in_left_cam;
		vive_get_stereo_camera_calibration(&hmd_config, &stereo_calib, &head_in_left_cam);

		// zero-initialized out of paranoia
		struct t_camera_extra_info info = {0};

		info.views[0].camera_orientation = CAMERA_ORIENTATION_0;
		info.views[1].camera_orientation = CAMERA_ORIENTATION_0;

		info.views[0].boundary_type = HT_IMAGE_BOUNDARY_CIRCLE;
		info.views[1].boundary_type = HT_IMAGE_BOUNDARY_CIRCLE;


		//!@todo This changes by like 50ish pixels from device to device. For now, the solution is simple: just
		//! make the circle a bit bigger than we'd like.
		// Maybe later we can do vignette calibration? Write a tiny optimizer that tries to fit Index's
		// gradient? Unsure.
		info.views[0].boundary.circle.normalized_center.x = 0.5f;
		info.views[0].boundary.circle.normalized_center.y = 0.5f;

		info.views[1].boundary.circle.normalized_center.x = 0.5f;
		info.views[1].boundary.circle.normalized_center.y = 0.5f;

		info.views[0].boundary.circle.normalized_radius = 0.55;
		info.views[1].boundary.circle.normalized_radius = 0.55;

		struct t_hand_tracking_create_info create_info = {.cams_info = info, .masks_sink = NULL};

		struct xrt_device *ht_device = NULL;
		int create_status = ht_device_create( //
		    svrb->xfctx,                      //
		    stereo_calib,                     //
		    create_info,                      //
		    &sinks,                           //
		    &ht_device);
		if (create_status != 0) {
			SVR_WARN("Failed to create hand tracking device\n");
			return false;
		}

		struct xrt_frame_sink *entry_left_sink = NULL;
		struct xrt_frame_sink *entry_right_sink = NULL;
		struct xrt_frame_sink *entry_sbs_sink = NULL;

		ht_device = multi_create_tracking_override( //
		    XRT_TRACKING_OVERRIDE_ATTACHED,         //
		    ht_device,                              //
		    svrb->head,                             //
		    XRT_INPUT_GENERIC_HEAD_POSE,            //
		    &head_in_left_cam);                     //

		int created_devices = cemu_devices_create( //
		    svrb->head,                            //
		    ht_device,                             //
		    hand_devices);                         //
		if (created_devices != 2) {
			SVR_WARN("Unexpected amount of hand devices created (%d)\n", create_status);
			xrt_device_destroy(&ht_device);
			return false;
		}

		entry_left_sink = sinks->cams[0];
		entry_right_sink = sinks->cams[1];
		u_sink_stereo_sbs_to_slam_sbs_create(svrb->xfctx, entry_left_sink, entry_right_sink, &entry_sbs_sink);
		u_sink_create_format_converter(svrb->xfctx, XRT_FORMAT_L8, entry_sbs_sink, &entry_sbs_sink);

		//! @todo Using a single slot queue is wrong for SLAM
		u_sink_simple_queue_create(svrb->xfctx, entry_sbs_sink, &entry_sbs_sink);

		struct xrt_slam_sinks entry_sinks = {
		    .cam_count = 1,
		    .cams = {entry_sbs_sink},
		    .imu = NULL,
		    .gt = NULL,
		};

		if (hand_devices[0] != NULL) {
			xsysd->xdevs[xsysd->xdev_count++] = hand_devices[0];
			left_ht = hand_devices[0];
		}

		if (hand_devices[1] != NULL) {
			xsysd->xdevs[xsysd->xdev_count++] = hand_devices[1];
			right_ht = hand_devices[1];
		}

		stream_data_sources(svrb, xp, entry_sinks);
	}
#endif /* XRT_BUILD_DRIVER_HANDTRACKING */

	svrb->left_ht = left_ht;
	xsysd->static_roles.hand_tracking.left = left_ht;

	svrb->right_ht = right_ht;
	xsysd->static_roles.hand_tracking.right = right_ht;

	/*
	 * Space overseer.
	 */

	struct u_space_overseer *uso = u_space_overseer_create(broadcast);

	struct xrt_pose T_stage_local = XRT_POSE_IDENTITY;

	u_space_overseer_legacy_setup( //
	    uso,                       // uso
	    xsysd->xdevs,              // xdevs
	    xsysd->xdev_count,         // xdev_count
	    svrb->head,                // head
	    &T_stage_local,            // local_offset
	    false,                     // root_is_unbounded
	    true                       // per_app_local_spaces
	);

	*out_xso = (struct xrt_space_overseer *)uso;

	return result;
}


/*
 *
 * 'Exported' functions.
 *
 */

struct xrt_builder *
t_builder_steamvr_create(void)
{
	struct steamvr_builder *svrb = U_TYPED_CALLOC(struct steamvr_builder);
	svrb->base.estimate_system = steamvr_estimate_system;
	svrb->base.open_system = steamvr_open_system;
	svrb->base.destroy = steamvr_destroy;
	svrb->base.identifier = "steamvr";
	svrb->base.name = "SteamVR proprietary wrapper (Vive, Index, Tundra trackers, etc.) devices builder";
	svrb->base.driver_identifiers = driver_list;
	svrb->base.driver_identifier_count = ARRAY_SIZE(driver_list);

	return &svrb->base;
}
