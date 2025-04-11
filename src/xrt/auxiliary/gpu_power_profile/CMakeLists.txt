# Copyright 2024, Patrick Nicolas.
# SPDX-License-Identifier: BSL-1.0

###
# GPU power profile library

if(XRT_FEATURE_AMDGPU_POWER_PROFILE)
	add_library(aux_gpu_power_profile STATIC gpu_power_profile.h gpu_power_profile.c)
	target_link_libraries(
		aux_gpu_power_profile
		PUBLIC aux-includes PkgConfig::LIBDRM_AMDGPU
		PRIVATE aux_vk
		)
endif()
