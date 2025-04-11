// Copyright 2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Google cardboard prober interface
 * @author Simon Zeni <simon.zeni@collabora.com>
 * @ingroup drv_cardboard
 */

#include "xrt/xrt_prober.h"

#include "util/u_misc.h"

#include "cardboard_interface.h"

/*!
 * Cardboard prober struct.
 *
 * @ingroup drv_cardboard
 * @implements xrt_auto_prober
 */
struct cardboard_prober
{
	struct xrt_auto_prober base;
};

//! @private @memberof cardboard_prober
static inline struct cardboard_prober *
cardboard_prober(struct xrt_auto_prober *xap)
{
	return (struct cardboard_prober *)xap;
}

//! @private @memberof cardboard_prober
static void
cardboard_prober_destroy(struct xrt_auto_prober *p)
{
	struct cardboard_prober *ap = cardboard_prober(p);
	free(ap);
}

//! @public @memberof cardboard_prober
static int
cardboard_prober_autoprobe(struct xrt_auto_prober *xap,
                           cJSON *attached_data,
                           bool no_hmds,
                           struct xrt_prober *xp,
                           struct xrt_device **out_xdevs)
{
	struct cardboard_prober *ap = cardboard_prober(xap);
	(void)ap;

	out_xdevs[0] = cardboard_hmd_create();
	return 1;
}

struct xrt_auto_prober *
cardboard_create_auto_prober(void)
{
	struct cardboard_prober *ap = U_TYPED_CALLOC(struct cardboard_prober);
	ap->base.name = "Google Cardboard HMD Auto-Prober";
	ap->base.destroy = cardboard_prober_destroy;
	ap->base.lelo_dallas_autoprobe = cardboard_prober_autoprobe;

	return &ap->base;
}
