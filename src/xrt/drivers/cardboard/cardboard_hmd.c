// Copyright 2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief HMD device for the Google cardboard driver.
 * @author Simon Zeni <simon.zeni@collabora.com>
 * @ingroup drv_cardboard
 */

#include "cardboard_interface.h"

#include "os/os_time.h"
#include "os/os_threading.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"

#include "math/m_api.h"
#include "math/m_mathinclude.h" // IWYU pragma: keep
#include "math/m_imu_3dof.h"

#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_var.h"
#include "util/u_distortion_mesh.h"
#include "xrt/xrt_results.h"

#include "android/android_custom_surface.h"
#include "android/android_content.h"
#include "android/android_globals.h"

#include "cardboard_device.pb.h"
#include "pb_decode.h"

#include <android/sensor.h>

DEBUG_GET_ONCE_LOG_OPTION(cardboard_log, "CARDBOARD_LOG", U_LOGGING_WARN)

#define HMD_TRACE(hmd, ...) U_LOG_XDEV_IFL_T(&hmd->base, hmd->log_level, __VA_ARGS__)
#define HMD_DEBUG(hmd, ...) U_LOG_XDEV_IFL_D(&hmd->base, hmd->log_level, __VA_ARGS__)
#define HMD_INFO(hmd, ...) U_LOG_XDEV_IFL_I(&hmd->base, hmd->log_level, __VA_ARGS__)
#define HMD_ERROR(hmd, ...) U_LOG_XDEV_IFL_E(&hmd->base, hmd->log_level, __VA_ARGS__)

/*!
 * Google cardboard HMD device.
 *
 * @implements xrt_device
 */
struct cardboard_hmd
{
	struct xrt_device base;
	struct os_thread_helper oth;

	ASensorManager *sensor_manager;
	const ASensor *accelerometer;
	const ASensor *gyroscope;
	ASensorEventQueue *event_queue;
	struct u_cardboard_distortion cardboard;

	//! Lock for last and fusion.
	struct os_mutex lock;
	struct m_imu_3dof fusion;

	enum u_logging_level log_level;
};

static inline struct cardboard_hmd *
cardboard_hmd(struct xrt_device *xdev)
{
	return (struct cardboard_hmd *)xdev;
}

static xrt_result_t
cardboard_hmd_get_tracked_pose(struct xrt_device *xdev,
                               enum xrt_input_name name,
                               int64_t at_timestamp_ns,
                               struct xrt_space_relation *out_relation)
{
	struct cardboard_hmd *hmd = cardboard_hmd(xdev);

	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		HMD_ERROR(hmd, "unknown input name");
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	struct xrt_space_relation new_relation = XRT_SPACE_RELATION_ZERO;
	new_relation.pose.orientation = hmd->fusion.rot;

	//! @todo assuming that orientation is actually currently tracked.
	new_relation.relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                              XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	                                                              XRT_SPACE_RELATION_POSITION_VALID_BIT);

	*out_relation = new_relation;
	return XRT_SUCCESS;
}

static bool
cardboard_hmd_compute_distortion(
    struct xrt_device *xdev, uint32_t view, float u, float v, struct xrt_uv_triplet *result)
{
	struct cardboard_hmd *hmd = cardboard_hmd(xdev);
	return u_compute_distortion_cardboard(&hmd->cardboard.values[view], u, v, result);
}

static void
cardboard_hmd_destroy(struct xrt_device *xdev)
{
	struct cardboard_hmd *hmd = cardboard_hmd(xdev);

	os_thread_helper_destroy(&hmd->oth);
	os_mutex_destroy(&hmd->lock);

	m_imu_3dof_close(&hmd->fusion);

	u_var_remove_root(hmd);

	u_device_free(&hmd->base);
}

static bool
read_file(pb_istream_t *stream, uint8_t *buf, size_t count)
{
	U_LOG_E("read_file callback");
	FILE *file = (FILE *)stream->state;
	if (buf == NULL) {
		while (count-- && fgetc(file) != EOF)
			;
		return count == 0;
	}

	bool status = (fread(buf, 1, count, file) == count);

	if (feof(file)) {
		stream->bytes_left = 0;
	}

	return status;
}

static bool
read_buffer(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
	U_LOG_E("read_file callback");

	uint8_t *buffer = (uint8_t *)*arg;
	return pb_read(stream, buffer, stream->bytes_left);
}

static bool
load_cardboard_distortion(struct cardboard_hmd *hmd,
                          struct xrt_android_display_metrics *metrics,
                          struct u_cardboard_distortion_arguments *args)
{
	char external_storage_dir[PATH_MAX] = {0};
	if (!android_content_get_files_dir(android_globals_get_context(), external_storage_dir,
	                                   sizeof(external_storage_dir))) {
		HMD_ERROR(hmd, "failed to access files dir");
		return false;
	}

	/* TODO: put file in Cardboard folder */
	char device_params_file[PATH_MAX] = {0};
	snprintf(device_params_file, sizeof(device_params_file), "%s/current_device_params", external_storage_dir);

	FILE *file = fopen(device_params_file, "rb");
	if (file == NULL) {
		HMD_ERROR(hmd, "failed to open calibration file '%s'", device_params_file);
		return false;
	}

	pb_istream_t stream = {&read_file, file, SIZE_MAX, NULL};
	cardboard_DeviceParams params = cardboard_DeviceParams_init_zero;

	char vendor[64] = {0};
	params.vendor.arg = vendor;
	params.vendor.funcs.decode = read_buffer;

	char model[64] = {0};
	params.model.arg = model;
	params.model.funcs.decode = read_buffer;

	float angles[4] = {0};
	params.left_eye_field_of_view_angles.arg = angles;
	params.left_eye_field_of_view_angles.funcs.decode = read_buffer;

	params.distortion_coefficients.arg = args->distortion_k;
	params.distortion_coefficients.funcs.decode = read_buffer;

	if (!pb_decode(&stream, cardboard_DeviceParams_fields, &params)) {
		HMD_ERROR(hmd, "failed to read calibration file: %s", PB_GET_ERROR(&stream));
		return false;
	}

	if (params.has_vertical_alignment) {
		if (params.vertical_alignment != cardboard_DeviceParams_VerticalAlignmentType_BOTTOM) {
			HMD_ERROR(hmd, "Only vertical alignment bottom supported");
			return false;
		}
	}

	if (params.has_inter_lens_distance) {
		args->inter_lens_distance_meters = params.inter_lens_distance;
	}
	if (params.has_screen_to_lens_distance) {
		args->screen_to_lens_distance_meters = params.screen_to_lens_distance;
	}
	if (params.has_tray_to_lens_distance) {
		args->tray_to_lens_distance_meters = params.tray_to_lens_distance;
	}

#define DEG_TO_RAD(x) (float)(x * M_PI / 180.0)
	args->fov = (struct xrt_fov){.angle_left = -DEG_TO_RAD(angles[0]),
	                             .angle_right = DEG_TO_RAD(angles[1]),
	                             .angle_down = -DEG_TO_RAD(angles[2]),
	                             .angle_up = DEG_TO_RAD(angles[3])};
#undef DEG_TO_RAD

	HMD_INFO(hmd, "loaded calibration for device %s (%s)", model, vendor);

	return true;
}

static int
cardboard_sensor_callback(int fd, int events, void *data)
{
	struct cardboard_hmd *hmd = (struct cardboard_hmd *)data;

	if (hmd->accelerometer == NULL || hmd->gyroscope == NULL)
		return 1;

	ASensorEvent event;
	struct xrt_vec3 gyro;
	struct xrt_vec3 accel;
	while (ASensorEventQueue_getEvents(hmd->event_queue, &event, 1) > 0) {

		switch (event.type) {
		case ASENSOR_TYPE_ACCELEROMETER: {
			accel.x = event.acceleration.y;
			accel.y = -event.acceleration.x;
			accel.z = event.acceleration.z;

			HMD_TRACE(hmd, "accel %" PRId64 " %.2f %.2f %.2f", event.timestamp, accel.x, accel.y, accel.z);
			break;
		}
		case ASENSOR_TYPE_GYROSCOPE: {
			gyro.x = -event.data[1];
			gyro.y = event.data[0];
			gyro.z = event.data[2];

			HMD_TRACE(hmd, "gyro %" PRId64 " %.2f %.2f %.2f", event.timestamp, gyro.x, gyro.y, gyro.z);

			// TODO: Make filter handle accelerometer
			struct xrt_vec3 null_accel;

			// Lock last and the fusion.
			os_mutex_lock(&hmd->lock);

			m_imu_3dof_update(&hmd->fusion, event.timestamp, &null_accel, &gyro);

			// Now done.
			os_mutex_unlock(&hmd->lock);
		}
		default: HMD_TRACE(hmd, "Unhandled event type %d", event.type);
		}
	}

	return 1;
}

static void *
cardboard_thread(void *ptr)
{
	struct cardboard_hmd *hmd = (struct cardboard_hmd *)ptr;
	const float freq_multiplier = 1.0f / 3.0f;
	const int32_t poll_rate_usec =
	    (int32_t)(hmd->base.hmd->screens[0].nominal_frame_interval_ns * freq_multiplier * 0.001f);

#if __ANDROID_API__ >= 26
	hmd->sensor_manager = ASensorManager_getInstanceForPackage(XRT_ANDROID_PACKAGE);
#else
	hmd->sensor_manager = ASensorManager_getInstance();
#endif

	hmd->accelerometer = ASensorManager_getDefaultSensor(hmd->sensor_manager, ASENSOR_TYPE_ACCELEROMETER);
	hmd->gyroscope = ASensorManager_getDefaultSensor(hmd->sensor_manager, ASENSOR_TYPE_GYROSCOPE);

	ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);

	hmd->event_queue = ASensorManager_createEventQueue(hmd->sensor_manager, looper, ALOOPER_POLL_CALLBACK,
	                                                   cardboard_sensor_callback, (void *)hmd);

	/*
	 * Start sensors in case this was not done already.
	 *
	 * On some Android devices, such as Pixel 4 and Meizu 20 series, running
	 * apps was not smooth due to the failure in setting the sensor's event
	 * rate. This was caused by the calculated sensor's event rate based on
	 * the screen refresh rate, which could be smaller than the sensor's
	 * minimum delay value. Make sure to set it to a valid value.
	 */
	if (hmd->accelerometer != NULL) {
		int32_t accelerometer_min_delay = ASensor_getMinDelay(hmd->accelerometer);
		int32_t accelerometer_poll_rate_usec = MAX(poll_rate_usec, accelerometer_min_delay);

		ASensorEventQueue_enableSensor(hmd->event_queue, hmd->accelerometer);
		ASensorEventQueue_setEventRate(hmd->event_queue, hmd->accelerometer, accelerometer_poll_rate_usec);
	}
	if (hmd->gyroscope != NULL) {
		int32_t gyroscope_min_delay = ASensor_getMinDelay(hmd->gyroscope);
		int32_t gyroscope_poll_rate_usec = MAX(poll_rate_usec, gyroscope_min_delay);

		ASensorEventQueue_enableSensor(hmd->event_queue, hmd->gyroscope);
		ASensorEventQueue_setEventRate(hmd->event_queue, hmd->gyroscope, gyroscope_poll_rate_usec);
	}

	int ret = 0;
	while (hmd->oth.running && ret != ALOOPER_POLL_ERROR) {
		ret = ALooper_pollAll(0, NULL, NULL, NULL);
	}

	return NULL;
}

struct xrt_device *
cardboard_hmd_create(void)
{
	enum u_device_alloc_flags flags =
	    (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE);

	struct cardboard_hmd *hmd = U_DEVICE_ALLOCATE(struct cardboard_hmd, flags, 1, 0);

	hmd->log_level = debug_get_log_option_cardboard_log();

	// Print name.
	snprintf(hmd->base.str, XRT_DEVICE_NAME_LEN, "Google cardboard HMD");
	snprintf(hmd->base.serial, XRT_DEVICE_NAME_LEN, "Google cardboard HMD");

	// Setup input.
	hmd->base.name = XRT_DEVICE_GENERIC_HMD;
	hmd->base.device_type = XRT_DEVICE_TYPE_HMD;
	hmd->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;

	// This list should be ordered, most preferred first.
	size_t idx = 0;
	hmd->base.hmd->blend_modes[idx++] = XRT_BLEND_MODE_OPAQUE;
	hmd->base.hmd->blend_mode_count = idx;

	hmd->base.update_inputs = u_device_noop_update_inputs;
	hmd->base.get_tracked_pose = cardboard_hmd_get_tracked_pose;
	hmd->base.set_output = u_device_ni_set_output;
	hmd->base.get_view_poses = u_device_get_view_poses;
	hmd->base.compute_distortion = cardboard_hmd_compute_distortion;
	hmd->base.get_visibility_mask = u_device_ni_get_visibility_mask;
	hmd->base.destroy = cardboard_hmd_destroy;

	m_imu_3dof_init(&hmd->fusion, M_IMU_3DOF_USE_GRAVITY_DUR_20MS);

	struct xrt_android_display_metrics metrics;
	if (!android_custom_surface_get_display_metrics(android_globals_get_vm(), android_globals_get_context(),
	                                                &metrics)) {
		HMD_ERROR(hmd, "Could not get Android display metrics.");
		cardboard_hmd_destroy(&hmd->base);
		return NULL;
	}

	hmd->base.hmd->screens[0].nominal_frame_interval_ns = time_s_to_ns(1.0f / metrics.refresh_rate);

	const uint32_t w_pixels = metrics.width_pixels;
	const uint32_t h_pixels = metrics.height_pixels;

	const float w_meters = ((float)w_pixels / (float)metrics.xdpi) * 0.0254f;
	const float h_meters = ((float)h_pixels / (float)metrics.ydpi) * 0.0254f;

	struct u_cardboard_distortion_arguments args = {
	    .screen =
	        {
	            .w_pixels = w_pixels,
	            .h_pixels = h_pixels,
	            .w_meters = w_meters,
	            .h_meters = h_meters,
	        },
	};

	if (!load_cardboard_distortion(hmd, &metrics, &args)) {
		HMD_ERROR(hmd, "Failed to load cardboard calibration file");
		cardboard_hmd_destroy(&hmd->base);
		return NULL;
	}

	u_distortion_cardboard_calculate(&args, hmd->base.hmd, &hmd->cardboard);

	// Distortion information.
	u_distortion_mesh_fill_in_compute(&hmd->base);

	int ret = os_mutex_init(&hmd->lock);
	if (ret != 0) {
		HMD_ERROR(hmd, "Failed to init mutex!");
		cardboard_hmd_destroy(&hmd->base);
		return NULL;
	}

	// Everything done, finally start the thread.
	os_thread_helper_init(&hmd->oth);
	ret = os_thread_helper_start(&hmd->oth, cardboard_thread, hmd);
	if (ret != 0) {
		HMD_ERROR(hmd, "Failed to start thread!");
		cardboard_hmd_destroy(&hmd->base);
		return NULL;
	}

	// Setup variable tracker: Optional but useful for debugging
	u_var_add_root(hmd, "Google cardboard HMD", true);
	u_var_add_log_level(hmd, &hmd->log_level, "log_level");
	u_var_add_ro_vec3_f32(hmd, &hmd->fusion.last.accel, "last.accel");
	u_var_add_ro_vec3_f32(hmd, &hmd->fusion.last.gyro, "last.gyro");

	hmd->base.orientation_tracking_supported = true;
	hmd->base.position_tracking_supported = false;

	return &hmd->base;
}
