// Copyright 2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Interface for the Google cardboard driver.
 * @author Simon Zeni <simon.zeni@collabora.com>
 * @ingroup drv_cardboard
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @defgroup drv_cardboard Google cardboard driver
 * @ingroup drv
 *
 * @brief Driver for the Google cardboard SDK.
 */

/*!
 * Create a auto prober for the Google cardboard driver.
 *
 * @ingroup drv_cardboard
 */
struct xrt_auto_prober *
cardboard_create_auto_prober(void);

/*!
 * Create a Google cardboard HMD.
 *
 * @ingroup drv_cardboard
 */
struct xrt_device *
cardboard_hmd_create(void);

/*!
 * @dir drivers/cardboard
 *
 * @brief @ref drv_cardboard files.
 */


#ifdef __cplusplus
}
#endif
