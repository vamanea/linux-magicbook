#ifndef _DP_TU_CALC_H_
#define _DP_TU_CALC_H_

#include <linux/types.h>
#include <drm/drm_device.h>

struct msm_dp_tu_calc_input {
	u64 lclk;        /* 162, 270, 540 and 810 */
	u64 pclk_khz;    /* in KHz */
	u64 hactive;     /* active h-width */
	u64 hporch;      /* bp + fp + pulse */
	int nlanes;      /* no.of.lanes */
	int bpp;         /* bits */
	int pixel_enc;   /* 444, 420, 422 */
	int dsc_en;     /* dsc on/off */
	int async_en;   /* async mode */
	int fec_en;     /* fec */
	int compress_ratio; /* 2:1 = 200, 3:1 = 300, 3.75:1 = 375 */
	int num_of_dsc_slices; /* number of slices per line */
};

struct msm_dp_vc_tu_mapping_table {
	u32 vic;
	u8 lanes;
	u8 lrate; /* DP_LINK_RATE -> 162(6), 270(10), 540(20), 810 (30) */
	u8 bpp;
	u8 valid_boundary_link;
	u16 delay_start_link;
	bool boundary_moderation_en;
	u8 valid_lower_boundary_link;
	u8 upper_boundary_count;
	u8 lower_boundary_count;
	u8 tu_size_minus1;
};

void msm_dp_ctrl_calc_tu(struct drm_device *drm_dev,
				struct msm_dp_tu_calc_input *in,
				struct msm_dp_vc_tu_mapping_table *tu_table);

#endif /* _DP_TU_CALC_H_ */
