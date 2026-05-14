// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2020, The Linux Foundation. All rights reserved.
 */

#include "dp_panel.h"
#include "dp_reg.h"
#include "dp_utils.h"

#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_fixed.h>
#include <drm/display/drm_dsc_helper.h>

#include <linux/io.h>
#include <linux/types.h>
#include <asm/byteorder.h>

#define DP_INTF_CONFIG_DATABUS_WIDEN     BIT(4)

struct msm_dp_panel_private {
	struct device *dev;
	struct drm_device *drm_dev;
	struct msm_dp_panel msm_dp_panel;
	struct drm_dp_aux *aux;
	struct msm_dp_link *link;
	void __iomem *link_base;
	void __iomem *p0_base;
	bool panel_on;
};

/* corresponds to the min src_bpp in the table that follows */
#define MSM_DP_MIN_SUPPORTED_DSC_SRC_BPP 24

/* assumed to be sorted by tgt_bpp */
static struct msm_dp_dto msm_dp_dto_params[] = {
	{30, 15, 5, 8},
	{24, 12, 1, 2},
	{30, 10, 5, 12},
	{30, 8, 1, 3},
	{24, 8, 1, 3},
};

static inline u32 msm_dp_read_link(struct msm_dp_panel_private *panel, u32 offset)
{
	return readl_relaxed(panel->link_base + offset);
}

static inline void msm_dp_write_link(struct msm_dp_panel_private *panel,
			       u32 offset, u32 data)
{
	/*
	 * To make sure link reg writes happens before any other operation,
	 * this function uses writel() instread of writel_relaxed()
	 */
	writel(data, panel->link_base + offset);
}

static inline void msm_dp_write_p0(struct msm_dp_panel_private *panel,
			       u32 offset, u32 data)
{
	/*
	 * To make sure interface reg writes happens before any other operation,
	 * this function uses writel() instread of writel_relaxed()
	 */
	writel(data, panel->p0_base + offset);
}

static inline u32 msm_dp_read_p0(struct msm_dp_panel_private *panel,
			       u32 offset)
{
	/*
	 * To make sure interface reg writes happens before any other operation,
	 * this function uses writel() instread of writel_relaxed()
	 */
	return readl_relaxed(panel->p0_base + offset);
}

static void msm_dp_panel_read_psr_cap(struct msm_dp_panel_private *panel)
{
	ssize_t rlen;
	struct msm_dp_panel *msm_dp_panel;

	msm_dp_panel = &panel->msm_dp_panel;

	/* edp sink */
	if (msm_dp_panel->dpcd[DP_EDP_CONFIGURATION_CAP]) {
		rlen = drm_dp_dpcd_read(panel->aux, DP_PSR_SUPPORT,
				&msm_dp_panel->psr_cap, sizeof(msm_dp_panel->psr_cap));
		if (rlen == sizeof(msm_dp_panel->psr_cap)) {
			drm_dbg_dp(panel->drm_dev,
				"psr version: 0x%x, psr_cap: 0x%x\n",
				msm_dp_panel->psr_cap.version,
				msm_dp_panel->psr_cap.capabilities);
		} else
			DRM_ERROR("failed to read psr info, rlen=%zd\n", rlen);
	}
}

static int msm_dp_panel_read_dpcd(struct msm_dp_panel *msm_dp_panel)
{
	int rc, max_lttpr_lanes, max_lttpr_rate;
	struct msm_dp_panel_private *panel;
	struct msm_dp_link_info *link_info;
	struct msm_dp_link *link;
	u8 *dpcd, major, minor;

	panel = container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);
	dpcd = msm_dp_panel->dpcd;
	rc = drm_dp_read_dpcd_caps(panel->aux, dpcd);
	if (rc)
		return rc;

	msm_dp_panel->vsc_sdp_supported = drm_dp_vsc_sdp_supported(panel->aux, dpcd);
	link_info = &msm_dp_panel->link_info;
	link_info->revision = dpcd[DP_DPCD_REV];
	major = (link_info->revision >> 4) & 0x0f;
	minor = link_info->revision & 0x0f;

	link = panel->link;
	drm_dbg_dp(panel->drm_dev, "max_lanes=%d max_link_rate=%d\n",
		   link->max_dp_lanes, link->max_dp_link_rate);

	max_lttpr_lanes = drm_dp_lttpr_max_lane_count(link->lttpr_common_caps);
	max_lttpr_rate = drm_dp_lttpr_max_link_rate(link->lttpr_common_caps);

	/* eDP sink */
	if (msm_dp_panel->dpcd[DP_EDP_CONFIGURATION_CAP]) {
		u8 edp_rev;

		rc = drm_dp_dpcd_read_byte(panel->aux, DP_EDP_DPCD_REV, &edp_rev);
		if (rc)
			return rc;

		drm_dbg_dp(panel->drm_dev, "edp_rev=0x%x\n", edp_rev);

		/* For eDP v1.4+, parse the SUPPORTED_LINK_RATES table */
		if (edp_rev >= DP_EDP_14) {
			__le16 rates[DP_MAX_SUPPORTED_RATES];
			u8 bw_set;
			int i;

			rc = drm_dp_dpcd_read_data(panel->aux, DP_SUPPORTED_LINK_RATES,
						   rates, sizeof(rates));
			if (rc)
				return rc;

			rc = drm_dp_dpcd_read_byte(panel->aux, DP_LINK_BW_SET, &bw_set);
			if (rc)
				return rc;

			/* Find index of max supported link rate that does not exceed dtsi limits */
			for (i = 0; i < ARRAY_SIZE(rates); i++) {
				/*
				 * The value from the DPCD multiplied by 200 gives
				 * the link rate in kHz. Divide by 10 to convert to
				 * symbol rate, accounting for 8b/10b encoding.
				 */
				u32 rate = (le16_to_cpu(rates[i]) * 200) / 10;

				if (!rate)
					break;

				drm_dbg_dp(panel->drm_dev,
					   "SUPPORTED_LINK_RATES[%d]: %d\n", i, rate);

				/*
				 * Limit link rate from link-frequencies of endpoint
				 * property of dtsi
				 */
				if (rate > link->max_dp_link_rate)
					break;

				/* Limit link rate from LTTPR capabilities, if any */
				if (max_lttpr_rate && rate > max_lttpr_rate)
					break;

				link_info->rate = rate;
				link_info->supported_rates[i] = rate;
				link_info->rate_set = i;
			}

			/* Only use LINK_RATE_SET if LINK_BW_SET hasn't already been written to */
			if (!bw_set && link_info->rate)
				link_info->use_rate_set = true;
		}
	}

	/* Fall back on MAX_LINK_RATE/LINK_BW_SET (DP, eDP <= v1.3) */
	if (!link_info->rate) {
		link_info->rate = drm_dp_max_link_rate(dpcd);

		/* Limit link rate from link-frequencies of endpoint property of dtsi */
		if (link_info->rate > link->max_dp_link_rate)
			link_info->rate = link->max_dp_link_rate;

		/* Limit link rate from LTTPR capabilities, if any */
		if (max_lttpr_rate && max_lttpr_rate < link_info->rate)
			link_info->rate = max_lttpr_rate;
	}

	link_info->num_lanes = drm_dp_max_lane_count(dpcd);

	/* Limit data lanes from data-lanes of endpoint property of dtsi */
	if (link_info->num_lanes > link->max_dp_lanes)
		link_info->num_lanes = link->max_dp_lanes;

	/* Limit data lanes from LTTPR capabilities, if any */
	if (max_lttpr_lanes && max_lttpr_lanes < link_info->num_lanes)
		link_info->num_lanes = max_lttpr_lanes;

	drm_dbg_dp(panel->drm_dev, "version: %d.%d\n", major, minor);
	drm_dbg_dp(panel->drm_dev, "link_rate=%d\n", link_info->rate);
	drm_dbg_dp(panel->drm_dev, "link_rate_set=%d\n", link_info->rate_set);
	drm_dbg_dp(panel->drm_dev, "use_rate_set=%d\n", link_info->use_rate_set);
	drm_dbg_dp(panel->drm_dev, "lane_count=%d\n", link_info->num_lanes);

	if (drm_dp_enhanced_frame_cap(dpcd))
		link_info->capabilities |= DP_LINK_CAP_ENHANCED_FRAMING;

	msm_dp_panel_read_psr_cap(panel);

	return rc;
}

static u32 msm_dp_panel_calc_link_rate(struct msm_dp_panel *msm_dp_panel,
			bool fec_en)
{
	const struct msm_dp_link_info *link_info;
	s64 rate_fp;
	s64 fec_overhead_fp;
	u32 data_rate_khz;

	link_info = &msm_dp_panel->link_info;
	data_rate_khz = link_info->num_lanes * link_info->rate * 8;

	if (fec_en) {
		fec_overhead_fp = drm_fixp_from_fraction(100000, 97582);

		rate_fp = drm_int2fixp(data_rate_khz);
		rate_fp = drm_fixp_div(rate_fp, fec_overhead_fp);
		data_rate_khz = drm_fixp2int(rate_fp);
	}

	return data_rate_khz;
}

static u8 msm_dp_panel_get_supported_bpp_no_dsc(
		u8 mode_edid_bpp,
		u32 mode_pclk_khz,
		u32 data_rate_khz)
{
	const u8 max_supported_bpp = 30, min_supported_bpp = 18;
	u8 bpp = min(mode_edid_bpp, max_supported_bpp);

	for (; bpp >= min_supported_bpp; bpp -= 6)
		if (mode_pclk_khz * bpp <= data_rate_khz)
			return bpp;

	return MSM_DP_DISPLAY_MODE_BPP_UNAVAILABLE;
}

static int msm_dp_panel_dsc_populate_params(struct msm_dp_panel *msm_dp_panel,
			struct msm_dp_display_mode *msm_dp_mode,
			u32 num_dsc,
			struct msm_dp_dto *dto_params);
static u8 msm_dp_panel_get_supported_bpp_dsc(
		struct msm_dp_panel *msm_dp_panel,
		u8 mode_edid_bpp,
		u32 mode_pclk_khz,
		u32 data_rate_khz,
		u8 *tgt_bpp,
		struct msm_dp_display_mode *msm_dp_mode,
		u32 num_dsc)
{
	const u8 max_supported_bpp = min(mode_edid_bpp, 30);
	const u8 num_components = 3;
	int i, j, rc;
	s64 data_rate_fp = drm_int2fixp(data_rate_khz);
	struct msm_dp_dto *dto_params = &msm_dp_mode->msm_dp_dsc.dto;

	for (j = 0; j < ARRAY_SIZE(msm_dp_dto_params); j++) {
		for (i = 0; i < 3; i++) {
			if (msm_dp_panel->dsc_cap.bpc[i]) {
				u8 bpp = msm_dp_panel->dsc_cap.bpc[i] * num_components;
				s64 data_rate_dsc_fp;
				u32 data_rate_dsc;

				if (bpp != msm_dp_dto_params[j].src_bpp)
					continue;

				if (bpp > max_supported_bpp)
					continue;

				msm_dp_mode->mode_cfg.bpp = msm_dp_dto_params[j].src_bpp;
				msm_dp_mode->mode_cfg.tgt_bpp = msm_dp_dto_params[j].tgt_bpp;

				if ((rc = msm_dp_panel_dsc_populate_params(msm_dp_panel,
							msm_dp_mode, num_dsc, dto_params)))
					continue;

				data_rate_dsc_fp = drm_fixp_div(data_rate_fp,
							msm_dp_mode->msm_dp_dsc.dsc_overhead_fp);
				data_rate_dsc = drm_fixp2int(data_rate_dsc_fp);

				if (mode_pclk_khz * msm_dp_dto_params[j].tgt_bpp <= data_rate_dsc) {
					if (tgt_bpp)
						*tgt_bpp = msm_dp_dto_params[j].tgt_bpp;
					return bpp;
				}
			}
		}
	}

	if (tgt_bpp)
		*tgt_bpp = 0;
	return MSM_DP_DISPLAY_MODE_BPP_UNAVAILABLE;
}

static struct msm_dp_display_mode_cfg msm_dp_panel_get_cfg_from_bpp(
	u8 bpp, u8 dsc_bpp, enum msm_mode_dsc_cfg dsc_cfg, bool fec_en, u8 tgt_bpp)
{
	const u8 min_supported_bpp = MSM_DP_MIN_SUPPORTED_DSC_SRC_BPP;
	struct msm_dp_display_mode_cfg cfg = {0};

	cfg.fec_available = fec_en;
	cfg.tgt_bpp = tgt_bpp;

	if (!dsc_bpp)
		dsc_cfg = MSM_MODE_DSC_UNAVAILABLE;
	else if (!bpp)
		dsc_cfg = MSM_MODE_DSC_REQUIRED;

	switch (dsc_cfg) {
	default:
	case MSM_MODE_DSC_UNAVAILABLE:
		cfg.bpp = bpp;
		break;
	case MSM_MODE_DSC_OPTIONAL:
	case MSM_MODE_DSC_PREFERRED:
		/*
		 * Return "if dsc is preferred" when not requested to be required.
		 * dsc is preferred if non-dsc bpp is of lower bpp than
		 * min supported dsc bpp
		 */
		dsc_cfg = bpp < min_supported_bpp ?
					MSM_MODE_DSC_PREFERRED :
					MSM_MODE_DSC_OPTIONAL;
		cfg.bpp = bpp;
		break;
	case MSM_MODE_DSC_REQUIRED:
		cfg.bpp = dsc_bpp;
		break;
	}

	cfg.dsc = dsc_cfg;

	return cfg;
}

static struct msm_dp_display_mode_cfg msm_dp_panel_get_supported_cfg(
		struct msm_dp_panel *msm_dp_panel,
		u8 mode_edid_bpp,
		enum msm_mode_dsc_cfg dsc_cfg,
		struct msm_dp_display_mode *msm_dp_mode,
		u32 num_dsc)
{
	u8 bpp = MSM_DP_DISPLAY_MODE_BPP_UNAVAILABLE;
	u8 dsc_bpp = MSM_DP_DISPLAY_MODE_BPP_UNAVAILABLE;
	u32 data_rate_khz;
	bool fec_en;
	u8 tgt_bpp = 0;
	u32 mode_pclk_khz = msm_dp_mode->drm_mode.clock;

	fec_en = msm_dp_panel->fec_cap.supported;
	data_rate_khz = msm_dp_panel_calc_link_rate(msm_dp_panel, fec_en);

	if (dsc_cfg < MSM_MODE_DSC_REQUIRED)
		bpp = msm_dp_panel_get_supported_bpp_no_dsc(
					mode_edid_bpp, mode_pclk_khz, data_rate_khz);

	if (dsc_cfg > MSM_MODE_DSC_UNAVAILABLE)
		dsc_bpp = msm_dp_panel_get_supported_bpp_dsc(
					msm_dp_panel,
					mode_edid_bpp, mode_pclk_khz, data_rate_khz, &tgt_bpp,
					msm_dp_mode, num_dsc);

	return msm_dp_panel_get_cfg_from_bpp(bpp, dsc_bpp, dsc_cfg, fec_en, tgt_bpp);
}

static void msm_dp_panel_read_sink_fec_caps(struct msm_dp_panel_private *panel)
{
	int rlen;
	u8 fec_dpcd;
	rlen = drm_dp_dpcd_readb(panel->aux, DP_FEC_CAPABILITY, &fec_dpcd);
	if (rlen < 1) {
		DRM_ERROR("fec capability read failed, rlen=%d\n", rlen);
		return;
	}

	panel->msm_dp_panel.fec_cap.supported = (fec_dpcd & DP_FEC_CAPABLE) > 0;
}

static bool msm_dp_panel_dsc_version_supported(u8 version_major, u8 version_minor)
{
	return version_major == 0x1 &&
				(version_minor == 0x1 || version_minor == 0x2);
}

static void msm_dp_panel_decode_dsc_dpcd(struct msm_dp_panel_dsc *dsc_cap,
			const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE])
{
	if (drm_dp_sink_supports_dsc(dsc_dpcd)) {
		u8 version = dsc_dpcd[DP_DSC_REV - DP_DSC_SUPPORT];

		dsc_cap->version_major = (version & DP_DSC_MAJOR_MASK) >> DP_DSC_MAJOR_SHIFT;
		dsc_cap->version_minor = (version & DP_DSC_MINOR_MASK) >> DP_DSC_MINOR_SHIFT;

		dsc_cap->supported =
				msm_dp_panel_dsc_version_supported(dsc_cap->version_major, dsc_cap->version_minor);

		if (dsc_cap->supported) {
			int num_bpc, i;
			dsc_cap->block_pred_en =
					(dsc_dpcd[DP_DSC_BLK_PREDICTION_SUPPORT - DP_DSC_SUPPORT] &
							DP_DSC_BLK_PREDICTION_IS_SUPPORTED) > 0;
			num_bpc = drm_dp_dsc_sink_supported_input_bpcs(dsc_dpcd, dsc_cap->bpc);
			for (i = num_bpc; i < ARRAY_SIZE(dsc_cap->bpc); i++)
				dsc_cap->bpc[i] = 0;
		}
	}
}

static void msm_dp_panel_read_sink_dsc_caps(struct msm_dp_panel_private *panel)
{
	int rlen;
	u8 dpcd_rev;

	dpcd_rev = panel->msm_dp_panel.dpcd[DP_DPCD_REV];

	if (dpcd_rev >= DP_DPCD_REV_14) {
		rlen = drm_dp_dpcd_read(panel->aux, DP_DSC_SUPPORT,
					panel->msm_dp_panel.dsc_dpcd, DP_DSC_RECEIVER_CAP_SIZE);
		if (rlen < DP_DSC_RECEIVER_CAP_SIZE) {
			DRM_ERROR("dsc dpcd read failed, rlen=%d\n", rlen);
			return;
		}

		msm_dp_panel_decode_dsc_dpcd(&panel->msm_dp_panel.dsc_cap,
					panel->msm_dp_panel.dsc_dpcd);
	}
}

int msm_dp_panel_read_sink_caps(struct msm_dp_panel *msm_dp_panel,
	struct drm_connector *connector)
{
	int rc, bw_code;
	int count;
	struct msm_dp_panel_private *panel;

	if (!msm_dp_panel || !connector) {
		DRM_ERROR("invalid input\n");
		return -EINVAL;
	}

	panel = container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);

	rc = msm_dp_panel_read_dpcd(msm_dp_panel);
	if (rc) {
		DRM_ERROR("read dpcd failed %d\n", rc);
		return rc;
	}

	bw_code = drm_dp_link_rate_to_bw_code(msm_dp_panel->link_info.rate);
	if (!is_link_rate_valid(bw_code) ||
			!is_lane_count_valid(msm_dp_panel->link_info.num_lanes) ||
			(bw_code > msm_dp_panel->max_bw_code)) {
		DRM_ERROR("Illegal link rate=%d lane=%d\n", msm_dp_panel->link_info.rate,
				msm_dp_panel->link_info.num_lanes);
		return -EINVAL;
	}

	if (drm_dp_is_branch(msm_dp_panel->dpcd)) {
		count = drm_dp_read_sink_count(panel->aux);
		if (!count) {
			panel->link->sink_count = 0;
			return -ENOTCONN;
		}
	}

	rc = drm_dp_read_downstream_info(panel->aux, msm_dp_panel->dpcd,
					 msm_dp_panel->downstream_ports);
	if (rc)
		return rc;

	drm_edid_free(msm_dp_panel->drm_edid);

	msm_dp_panel->drm_edid = drm_edid_read_ddc(connector, &panel->aux->ddc);

	drm_edid_connector_update(connector, msm_dp_panel->drm_edid);

	if (!msm_dp_panel->drm_edid) {
		DRM_ERROR("panel edid read failed\n");
		/* check edid read fail is due to unplug */
		if (!msm_dp_aux_is_link_connected(panel->aux)) {
			rc = -ETIMEDOUT;
			goto end;
		}
	}

	memset(&msm_dp_panel->fec_cap, 0, sizeof(msm_dp_panel->fec_cap));
	msm_dp_panel_read_sink_fec_caps(panel);

	memset(&msm_dp_panel->dsc_cap, 0, sizeof(msm_dp_panel->dsc_cap));
	if (msm_dp_panel->fec_cap.supported)
		msm_dp_panel_read_sink_dsc_caps(panel);

	drm_dbg_dp(panel->drm_dev, "fec_cap=%d dsc_cap=%d\n",
			msm_dp_panel->fec_cap.supported,
			msm_dp_panel->dsc_cap.supported);

end:
	return rc;
}

struct msm_dp_display_mode_cfg msm_dp_panel_get_mode_cfg(
		struct msm_dp_panel *msm_dp_panel,
		u32 mode_edid_bpp,
		enum msm_mode_dsc_cfg dsc_cfg,
		struct msm_dp_display_mode *msm_dp_mode,
		u8 num_dsc)
{
	struct msm_dp_panel_private *panel;
	struct msm_dp_display_mode_cfg cfg = {0};

	if (!msm_dp_panel || !mode_edid_bpp) {
		DRM_ERROR("invalid input\n");
		return cfg;
	}

	panel = container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);

	if (msm_dp_panel->video_test) {
		cfg.bpp = msm_dp_link_bit_depth_to_bpp(
				panel->link->test_video.test_bit_depth);
		return cfg;
	}

	return msm_dp_panel_get_supported_cfg(msm_dp_panel, mode_edid_bpp,
			dsc_cfg, msm_dp_mode, num_dsc);
}

int msm_dp_panel_get_modes(struct msm_dp_panel *msm_dp_panel,
	struct drm_connector *connector)
{
	if (!msm_dp_panel) {
		DRM_ERROR("invalid input\n");
		return -EINVAL;
	}

	if (msm_dp_panel->drm_edid)
		return drm_edid_connector_add_modes(connector);

	return 0;
}

static u8 msm_dp_panel_get_edid_checksum(const struct edid *edid)
{
	edid += edid->extensions;

	return edid->checksum;
}

void msm_dp_panel_handle_sink_request(struct msm_dp_panel *msm_dp_panel)
{
	struct msm_dp_panel_private *panel;

	if (!msm_dp_panel) {
		DRM_ERROR("invalid input\n");
		return;
	}

	panel = container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);

	if (panel->link->sink_request & DP_TEST_LINK_EDID_READ) {
		/* FIXME: get rid of drm_edid_raw() */
		const struct edid *edid = drm_edid_raw(msm_dp_panel->drm_edid);
		u8 checksum;

		if (edid)
			checksum = msm_dp_panel_get_edid_checksum(edid);
		else
			checksum = msm_dp_panel->connector->real_edid_checksum;

		msm_dp_link_send_edid_checksum(panel->link, checksum);
		msm_dp_link_send_test_response(panel->link);
	}
}

static void msm_dp_panel_tpg_enable(struct msm_dp_panel *msm_dp_panel,
				    struct drm_display_mode *drm_mode)
{
	struct msm_dp_panel_private *panel =
		container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);
	u32 hsync_period, vsync_period;
	u32 display_v_start, display_v_end;
	u32 hsync_start_x, hsync_end_x;
	u32 v_sync_width;
	u32 hsync_ctl;
	u32 display_hctl;

	/* TPG config parameters*/
	hsync_period = drm_mode->htotal;
	vsync_period = drm_mode->vtotal;

	display_v_start = ((drm_mode->vtotal - drm_mode->vsync_start) *
					hsync_period);
	display_v_end = ((vsync_period - (drm_mode->vsync_start -
					drm_mode->vdisplay))
					* hsync_period) - 1;

	display_v_start += drm_mode->htotal - drm_mode->hsync_start;
	display_v_end -= (drm_mode->hsync_start - drm_mode->hdisplay);

	hsync_start_x = drm_mode->htotal - drm_mode->hsync_start;
	hsync_end_x = hsync_period - (drm_mode->hsync_start -
					drm_mode->hdisplay) - 1;

	v_sync_width = drm_mode->vsync_end - drm_mode->vsync_start;

	hsync_ctl = (hsync_period << 16) |
			(drm_mode->hsync_end - drm_mode->hsync_start);
	display_hctl = (hsync_end_x << 16) | hsync_start_x;


	msm_dp_write_p0(panel, MMSS_DP_INTF_HSYNC_CTL, hsync_ctl);
	msm_dp_write_p0(panel, MMSS_DP_INTF_VSYNC_PERIOD_F0, vsync_period *
			hsync_period);
	msm_dp_write_p0(panel, MMSS_DP_INTF_VSYNC_PULSE_WIDTH_F0, v_sync_width *
			hsync_period);
	msm_dp_write_p0(panel, MMSS_DP_INTF_VSYNC_PERIOD_F1, 0);
	msm_dp_write_p0(panel, MMSS_DP_INTF_VSYNC_PULSE_WIDTH_F1, 0);
	msm_dp_write_p0(panel, MMSS_DP_INTF_DISPLAY_HCTL, display_hctl);
	msm_dp_write_p0(panel, MMSS_DP_INTF_ACTIVE_HCTL, 0);
	msm_dp_write_p0(panel, MMSS_INTF_DISPLAY_V_START_F0, display_v_start);
	msm_dp_write_p0(panel, MMSS_DP_INTF_DISPLAY_V_END_F0, display_v_end);
	msm_dp_write_p0(panel, MMSS_INTF_DISPLAY_V_START_F1, 0);
	msm_dp_write_p0(panel, MMSS_DP_INTF_DISPLAY_V_END_F1, 0);
	msm_dp_write_p0(panel, MMSS_DP_INTF_ACTIVE_V_START_F0, 0);
	msm_dp_write_p0(panel, MMSS_DP_INTF_ACTIVE_V_END_F0, 0);
	msm_dp_write_p0(panel, MMSS_DP_INTF_ACTIVE_V_START_F1, 0);
	msm_dp_write_p0(panel, MMSS_DP_INTF_ACTIVE_V_END_F1, 0);
	msm_dp_write_p0(panel, MMSS_DP_INTF_POLARITY_CTL, 0);

	msm_dp_write_p0(panel, MMSS_DP_TPG_MAIN_CONTROL,
				DP_TPG_CHECKERED_RECT_PATTERN);
	msm_dp_write_p0(panel, MMSS_DP_TPG_VIDEO_CONFIG,
				DP_TPG_VIDEO_CONFIG_BPP_8BIT |
				DP_TPG_VIDEO_CONFIG_RGB);
	msm_dp_write_p0(panel, MMSS_DP_BIST_ENABLE,
				DP_BIST_ENABLE_DPBIST_EN);
	msm_dp_write_p0(panel, MMSS_DP_TIMING_ENGINE_EN,
				DP_TIMING_ENGINE_EN_EN);
	drm_dbg_dp(panel->drm_dev, "%s: enabled tpg\n", __func__);
}

static void msm_dp_panel_tpg_disable(struct msm_dp_panel *msm_dp_panel)
{
	struct msm_dp_panel_private *panel =
		container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);

	msm_dp_write_p0(panel, MMSS_DP_TPG_MAIN_CONTROL, 0x0);
	msm_dp_write_p0(panel, MMSS_DP_BIST_ENABLE, 0x0);
	msm_dp_write_p0(panel, MMSS_DP_TIMING_ENGINE_EN, 0x0);
}

void msm_dp_panel_tpg_config(struct msm_dp_panel *msm_dp_panel, bool enable)
{
	struct msm_dp_panel_private *panel;

	if (!msm_dp_panel) {
		DRM_ERROR("invalid input\n");
		return;
	}

	panel = container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);

	if (!panel->panel_on) {
		drm_dbg_dp(panel->drm_dev,
				"DP panel not enabled, handle TPG on next on\n");
		return;
	}

	if (!enable) {
		msm_dp_panel_tpg_disable(msm_dp_panel);
		return;
	}

	drm_dbg_dp(panel->drm_dev, "calling panel's tpg_enable\n");
	msm_dp_panel_tpg_enable(msm_dp_panel, &panel->msm_dp_panel.msm_dp_mode.drm_mode);
}

static void msm_dp_panel_send_vsc_sdp(struct msm_dp_panel_private *panel, struct dp_sdp *vsc_sdp)
{
	u32 header[2];
	u32 val;
	int i;

	msm_dp_utils_pack_sdp_header(&vsc_sdp->sdp_header, header);

	msm_dp_write_link(panel, MMSS_DP_GENERIC0_0, header[0]);
	msm_dp_write_link(panel, MMSS_DP_GENERIC0_1, header[1]);

	for (i = 0; i < sizeof(vsc_sdp->db); i += 4) {
		val = ((vsc_sdp->db[i]) | (vsc_sdp->db[i + 1] << 8) | (vsc_sdp->db[i + 2] << 16) |
		       (vsc_sdp->db[i + 3] << 24));
		msm_dp_write_link(panel, MMSS_DP_GENERIC0_2 + i, val);
	}
}

static void msm_dp_panel_update_sdp(struct msm_dp_panel_private *panel)
{
	u32 hw_revision = panel->msm_dp_panel.hw_revision;

	if (hw_revision >= DP_HW_VERSION_1_0 &&
	    hw_revision < DP_HW_VERSION_1_2) {
		msm_dp_write_link(panel, MMSS_DP_SDP_CFG3, UPDATE_SDP);
		msm_dp_write_link(panel, MMSS_DP_SDP_CFG3, 0x0);
	}
}

void msm_dp_panel_enable_vsc_sdp(struct msm_dp_panel *msm_dp_panel, struct dp_sdp *vsc_sdp)
{
	struct msm_dp_panel_private *panel =
		container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);
	u32 cfg, cfg2, misc;

	cfg = msm_dp_read_link(panel, MMSS_DP_SDP_CFG);
	cfg2 = msm_dp_read_link(panel, MMSS_DP_SDP_CFG2);
	misc = msm_dp_read_link(panel, REG_DP_MISC1_MISC0);

	cfg |= GEN0_SDP_EN;
	msm_dp_write_link(panel, MMSS_DP_SDP_CFG, cfg);

	cfg2 |= GENERIC0_SDPSIZE_VALID;
	msm_dp_write_link(panel, MMSS_DP_SDP_CFG2, cfg2);

	msm_dp_panel_send_vsc_sdp(panel, vsc_sdp);

	/* indicates presence of VSC (BIT(6) of MISC1) */
	misc |= DP_MISC1_VSC_SDP;

	drm_dbg_dp(panel->drm_dev, "vsc sdp enable=1\n");

	pr_debug("misc settings = 0x%x\n", misc);
	msm_dp_write_link(panel, REG_DP_MISC1_MISC0, misc);

	msm_dp_panel_update_sdp(panel);
}

void msm_dp_panel_disable_vsc_sdp(struct msm_dp_panel *msm_dp_panel)
{
	struct msm_dp_panel_private *panel =
		container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);
	u32 cfg, cfg2, misc;

	cfg = msm_dp_read_link(panel, MMSS_DP_SDP_CFG);
	cfg2 = msm_dp_read_link(panel, MMSS_DP_SDP_CFG2);
	misc = msm_dp_read_link(panel, REG_DP_MISC1_MISC0);

	cfg &= ~GEN0_SDP_EN;
	msm_dp_write_link(panel, MMSS_DP_SDP_CFG, cfg);

	cfg2 &= ~GENERIC0_SDPSIZE_VALID;
	msm_dp_write_link(panel, MMSS_DP_SDP_CFG2, cfg2);

	/* switch back to MSA */
	misc &= ~DP_MISC1_VSC_SDP;

	drm_dbg_dp(panel->drm_dev, "vsc sdp enable=0\n");

	pr_debug("misc settings = 0x%x\n", misc);
	msm_dp_write_link(panel, REG_DP_MISC1_MISC0, misc);

	msm_dp_panel_update_sdp(panel);
}

static int msm_dp_panel_setup_vsc_sdp_yuv_420(struct msm_dp_panel *msm_dp_panel)
{
	struct msm_dp_display_mode *msm_dp_mode;
	struct drm_dp_vsc_sdp vsc_sdp_data;
	struct dp_sdp vsc_sdp;
	ssize_t len;

	if (!msm_dp_panel) {
		DRM_ERROR("invalid input\n");
		return -EINVAL;
	}

	msm_dp_mode = &msm_dp_panel->msm_dp_mode;

	memset(&vsc_sdp_data, 0, sizeof(vsc_sdp_data));

	/* VSC SDP header as per table 2-118 of DP 1.4 specification */
	vsc_sdp_data.sdp_type = DP_SDP_VSC;
	vsc_sdp_data.revision = 0x05;
	vsc_sdp_data.length = 0x13;

	/* VSC SDP Payload for DB16 */
	vsc_sdp_data.pixelformat = DP_PIXELFORMAT_YUV420;
	vsc_sdp_data.colorimetry = DP_COLORIMETRY_DEFAULT;

	/* VSC SDP Payload for DB17 */
	vsc_sdp_data.bpc = msm_dp_mode->mode_cfg.bpp / 3;
	vsc_sdp_data.dynamic_range = DP_DYNAMIC_RANGE_CTA;

	/* VSC SDP Payload for DB18 */
	vsc_sdp_data.content_type = DP_CONTENT_TYPE_GRAPHICS;

	len = drm_dp_vsc_sdp_pack(&vsc_sdp_data, &vsc_sdp);
	if (len < 0) {
		DRM_ERROR("unable to pack vsc sdp\n");
		return len;
	}

	msm_dp_panel_enable_vsc_sdp(msm_dp_panel, &vsc_sdp);

	return 0;
}

int msm_dp_panel_timing_cfg(struct msm_dp_panel *msm_dp_panel)
{
	u32 data, total_ver, total_hor;
	struct msm_dp_panel_private *panel;
	struct drm_display_mode *drm_mode;
	u32 width_blanking;
	u32 sync_start;
	u32 msm_dp_active;
	u32 total;
	u32 reg;

	panel = container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);
	drm_mode = &msm_dp_panel->msm_dp_mode.drm_mode;

	drm_dbg_dp(panel->drm_dev, "width=%d hporch= %d %d %d\n",
		drm_mode->hdisplay, drm_mode->htotal - drm_mode->hsync_end,
		drm_mode->hsync_start - drm_mode->hdisplay,
		drm_mode->hsync_end - drm_mode->hsync_start);

	drm_dbg_dp(panel->drm_dev, "height=%d vporch= %d %d %d\n",
		drm_mode->vdisplay, drm_mode->vtotal - drm_mode->vsync_end,
		drm_mode->vsync_start - drm_mode->vdisplay,
		drm_mode->vsync_end - drm_mode->vsync_start);

	total_hor = drm_mode->htotal;

	total_ver = drm_mode->vtotal;

	data = total_ver;
	data <<= 16;
	data |= total_hor;

	total = data;

	data = (drm_mode->vtotal - drm_mode->vsync_start);
	data <<= 16;
	data |= (drm_mode->htotal - drm_mode->hsync_start);

	sync_start = data;

	data = drm_mode->vsync_end - drm_mode->vsync_start;
	data <<= 16;
	data |= (msm_dp_panel->msm_dp_mode.v_active_low << 31);
	data |= drm_mode->hsync_end - drm_mode->hsync_start;
	data |= (msm_dp_panel->msm_dp_mode.h_active_low << 15);

	width_blanking = data;

	data = drm_mode->vdisplay;
	data <<= 16;
	data |= drm_mode->hdisplay;

	msm_dp_active = data;

	msm_dp_write_link(panel, REG_DP_TOTAL_HOR_VER, total);
	msm_dp_write_link(panel, REG_DP_START_HOR_VER_FROM_SYNC, sync_start);
	msm_dp_write_link(panel, REG_DP_HSYNC_VSYNC_WIDTH_POLARITY, width_blanking);
	msm_dp_write_link(panel, REG_DP_ACTIVE_HOR_VER, msm_dp_active);

	reg = msm_dp_read_p0(panel, MMSS_DP_INTF_CONFIG);
	if (msm_dp_panel->msm_dp_mode.wide_bus_en)
		reg |= DP_INTF_CONFIG_DATABUS_WIDEN;
	else
		reg &= ~DP_INTF_CONFIG_DATABUS_WIDEN;

	drm_dbg_dp(panel->drm_dev, "wide_bus_en=%d reg=%#x\n",
				msm_dp_panel->msm_dp_mode.wide_bus_en, reg);

	msm_dp_write_p0(panel, MMSS_DP_INTF_CONFIG, reg);

	if (msm_dp_panel->msm_dp_mode.out_fmt_is_yuv_420)
		msm_dp_panel_setup_vsc_sdp_yuv_420(msm_dp_panel);

	panel->panel_on = true;

	return 0;
}

/**
 * msm_dp_panel_get_dto_params() - get numerator and denominator for dsc bpp config
 * @src_bpp: uncompressed bits per pixel
 * @tgt_bpp: compressed bits per pixel
 * @num: returning numerator
 * @denom: returning denominator
 */
static inline void msm_dp_panel_get_dto_params(u8 src_bpp, u8 tgt_bpp, u8 *num, u8 *denom)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(msm_dp_dto_params); i++) {
		if (msm_dp_dto_params[i].src_bpp == src_bpp && msm_dp_dto_params[i].tgt_bpp == tgt_bpp) {
			*num = msm_dp_dto_params[i].dto_n;
			*denom = msm_dp_dto_params[i].dto_d;
			return;
		}
	}
	DRM_ERROR("dto params not found for sbpp=%d tbpp=%d\n", src_bpp, tgt_bpp);
	*num = 0;
	*denom = 1;
}

struct msm_dp_dsc_slices_per_line {
	u32 min_ppr;
	u32 max_ppr;
	u8 num_slices;
};

struct msm_dp_dsc_slice_caps_bit_map {
	u32 num_slices;
	u32 bit_index;
};

static const struct msm_dp_dsc_slices_per_line slice_per_line_tbl[] = {
	{0,     340,    1   },
	{340,   680,    2   },
	{680,   1360,   4   },
	{1360,  3200,   8   },
	{3200,  4800,   12  },
	{4800,  6400,   16  },
	{6400,  8000,   20  },
	{8000,  9600,   24  }
};

static const u32 peak_throughput_mode_0_tbl[] = {
	0,
	340,
	400,
	450,
	500,
	550,
	600,
	650,
	700,
	750,
	800,
	850,
	900,
	950,
	1000,
};

static const struct msm_dp_dsc_slice_caps_bit_map slice_caps_bit_map_tbl[] = {
	{1, 0},
	{2, 1},
	{4, 3},
	{6, 4},
	{8, 5},
	{10, 6},
	{12, 7},
	{16, 0},
	{20, 1},
	{24, 2},
};

static bool msm_dp_panel_check_slice_support(u32 num_slices, u32 slice_caps_1,
		u32 slice_caps_2)
{
	const struct msm_dp_dsc_slice_caps_bit_map *bcap;
	u32 slice_caps;
	int i;

	if (num_slices <= 12)
		slice_caps = slice_caps_1;
	else
		slice_caps = slice_caps_2;

	for (i = 0; i < ARRAY_SIZE(slice_caps_bit_map_tbl); i++) {
		bcap = &slice_caps_bit_map_tbl[i];

		if (bcap->num_slices == num_slices)
			return !!(slice_caps & (1 << bcap->bit_index));
	}

	return false;
}

static int msm_dp_panel_dsc_prepare(struct msm_dp_panel *msm_dp_panel,
			struct msm_dp_display_mode *msm_dp_mode,
			u32 num_dsc,
			struct msm_dp_dto *dto_params)
{
	struct msm_dp_panel_private *panel =
				container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);
	struct drm_display_mode *drm_mode = &msm_dp_mode->drm_mode;
	struct drm_dsc_config *drm_dsc = &msm_dp_mode->drm_dsc;
	struct msm_dp_dsc_cfg *msm_dp_dsc = &msm_dp_mode->msm_dp_dsc;
	struct msm_dp_display_mode_cfg *mode_cfg = &msm_dp_mode->mode_cfg;

	int slice_per_line_tbl_i;
	const struct msm_dp_dsc_slices_per_line *rec;
	u32 peak_throughput;
	u32 max_slice_width;
	u32 slice_width;
	u32 ppr_per_slice;
	u32 slice_caps_1;
	u32 slice_caps_2;

	u32 ppr = drm_mode->clock / 1000;
	u32 ppr_max_index = msm_dp_panel->dsc_dpcd[DP_DSC_PEAK_THROUGHPUT - DP_DSC_SUPPORT];
	ppr_max_index = (ppr_max_index & DP_DSC_THROUGHPUT_MODE_0_MASK) >>
				DP_DSC_THROUGHPUT_MODE_0_SHIFT;

	drm_dsc->dsc_version_major = msm_dp_panel->dsc_cap.version_major;
	drm_dsc->dsc_version_minor = msm_dp_panel->dsc_cap.version_minor;

	msm_dp_dsc->slice_per_pkt = 0;

	for (slice_per_line_tbl_i = 0;
				slice_per_line_tbl_i < ARRAY_SIZE(slice_per_line_tbl);
				slice_per_line_tbl_i++) {
		rec = &slice_per_line_tbl[slice_per_line_tbl_i];
		if ((ppr > rec->min_ppr) && (ppr <= rec->max_ppr)) {
			msm_dp_dsc->slice_per_pkt = rec->num_slices;
			slice_per_line_tbl_i++;
			break;
		}
	}

	if (!msm_dp_dsc->slice_per_pkt)
		return -EINVAL;

	if (ppr_max_index <= DP_DSC_THROUGHPUT_MODE_0_UNSUPPORTED ||
				ppr_max_index >= DP_DSC_THROUGHPUT_MODE_0_170) {
		drm_dbg_dp(panel->drm_dev,
					"Throughput mode 0 0x%x not supported\n", ppr_max_index);
		return -EINVAL;
	}

	peak_throughput = peak_throughput_mode_0_tbl[ppr_max_index];

	if (!num_dsc)
		return -EINVAL;

	max_slice_width = msm_dp_panel->dsc_dpcd[DP_DSC_MAX_SLICE_WIDTH - DP_DSC_SUPPORT] *
				DP_DSC_SLICE_WIDTH_MULTIPLIER;
	max_slice_width = min(max_slice_width, drm_mode->hdisplay / num_dsc);

	slice_width = (drm_mode->hdisplay /
				msm_dp_dsc->slice_per_pkt);
	ppr_per_slice = ppr / msm_dp_dsc->slice_per_pkt;

	slice_caps_1 = msm_dp_panel->dsc_dpcd[DP_DSC_SLICE_CAP_1 - DP_DSC_SUPPORT];
	slice_caps_2 = msm_dp_panel->dsc_dpcd[DP_DSC_SLICE_CAP_2 - DP_DSC_SUPPORT];

	/*
	 * There are 3 conditions to check for sink support:
	 * 1. The slice width cannot exceed the maximum.
	 * 2. The ppr per slice cannot exceed the maximum.
	 * 3. The number of slices must be explicitly supported.
	 */
	while (slice_width > max_slice_width ||
			ppr_per_slice > peak_throughput ||
			!msm_dp_panel_check_slice_support(
					msm_dp_dsc->slice_per_pkt,
					slice_caps_1, slice_caps_2)) {
		if (slice_per_line_tbl_i == ARRAY_SIZE(slice_per_line_tbl))
			return -EINVAL;

		rec = &slice_per_line_tbl[slice_per_line_tbl_i];
		msm_dp_dsc->slice_per_pkt = rec->num_slices;
		slice_width = (drm_mode->hdisplay /
				msm_dp_dsc->slice_per_pkt);
		ppr_per_slice = ppr / msm_dp_dsc->slice_per_pkt;

		slice_per_line_tbl_i++;
	}

	drm_dsc->block_pred_enable = msm_dp_panel->dsc_cap.block_pred_en;

	drm_dsc->pic_width = drm_mode->hdisplay;
	drm_dsc->pic_height = drm_mode->vdisplay;
	drm_dsc->slice_width = slice_width;

	if (drm_dsc->pic_height % 108 == 0)
		drm_dsc->slice_height = 108;
	else if (drm_dsc->pic_height % 16 == 0)
		drm_dsc->slice_height = 16;
	else if (drm_dsc->pic_height % 12 == 0)
		drm_dsc->slice_height = 12;
	else
		drm_dsc->slice_height = 15;

	drm_dsc->bits_per_component = mode_cfg->bpp / 3;
	drm_dsc->bits_per_pixel = mode_cfg->tgt_bpp << 4;
	drm_dsc->slice_count = DIV_ROUND_UP(drm_mode->hdisplay, slice_width);

	dto_params->src_bpp = mode_cfg->bpp;
	dto_params->tgt_bpp = mode_cfg->tgt_bpp;
	msm_dp_panel_get_dto_params(dto_params->src_bpp,
				dto_params->tgt_bpp,
				&dto_params->dto_n,
				&dto_params->dto_d);

	return 0;
}

static int msm_populate_dsc_params(struct msm_dp_panel *msm_dp_panel, struct drm_dsc_config *dsc)
{
	int ret;
	u32 bpp;

	struct msm_dp_panel_private *panel =
				container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);

	dsc->convert_rgb = 1;

	drm_dsc_set_const_params(dsc);
	drm_dsc_set_rc_buf_thresh(dsc);

	if (!msm_dp_panel_dsc_version_supported(dsc->dsc_version_major, dsc->dsc_version_minor))
		return -EINVAL;

	bpp = dsc->bits_per_pixel >> 4;
	ret = drm_dsc_setup_rc_params(dsc,
				dsc->dsc_version_minor == 1 || bpp == 8 || bpp == 12
						? DRM_DSC_1_1_PRE_SCR : DRM_DSC_1_2_444);
	if (ret) {
		drm_dbg_dp(panel->drm_dev, "could not find DSC RC parameters\n");
		return ret;
	}

	dsc->initial_scale_value = drm_dsc_initial_scale_value(dsc);
	dsc->line_buf_depth = dsc->bits_per_component + 1;

	return drm_dsc_compute_rc_parameters(dsc);
}

static s64 msm_dp_panel_dsc_bw_overhead_fixp(struct msm_dp_panel *msm_dp_panel,
		struct msm_dp_dsc_cfg *msm_dp_dsc, u32 dsc_byte_cnt)
{
	int num_slices, tot_num_eoc_symbols;
	int tot_num_hor_bytes, tot_num_dummy_bytes;
	int dwidth_dsc_bytes, eoc_bytes;
	u32 num_lanes;
	struct msm_dp_panel_private *panel;

	panel = container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);

	num_lanes = panel->link->link_params.num_lanes;
	num_slices = msm_dp_dsc->slice_per_pkt;

	eoc_bytes = dsc_byte_cnt % num_lanes;
	tot_num_eoc_symbols = num_lanes * num_slices;
	tot_num_hor_bytes = dsc_byte_cnt * num_slices;
	tot_num_dummy_bytes = (num_lanes - eoc_bytes) * num_slices;

	if (!eoc_bytes)
		tot_num_dummy_bytes = 0;

	dwidth_dsc_bytes = tot_num_hor_bytes + tot_num_eoc_symbols +
				tot_num_dummy_bytes;

	drm_dbg_dp(panel->drm_dev, "dwidth_dsc_bytes:%d, tot_num_hor_bytes:%d\n",
			dwidth_dsc_bytes, tot_num_hor_bytes);

	return drm_fixp_from_fraction(dwidth_dsc_bytes,
			tot_num_hor_bytes);
}

static int msm_dp_populate_dsc_private_params(struct msm_dp_panel *msm_dp_panel,
			struct msm_dp_display_mode *msm_dp_mode,
			const struct msm_dp_dto *dto_params)
{
	struct msm_dp_panel_private *panel;

	struct drm_display_mode *drm_mode = &msm_dp_mode->drm_mode;
	struct drm_dsc_config *drm_dsc = &msm_dp_mode->drm_dsc;
	struct msm_dp_dsc_cfg *msm_dp_dsc = &msm_dp_mode->msm_dp_dsc;
	u32 intf_width = drm_mode->hdisplay;
	u32 slice_per_pkt;
	u32 slice_per_intf;
	u32 bpp;
	u32 bytes_in_slice;
	u32 comp_ratio;
	s64 temp1_fp, temp2_fp;
	s64 numerator_fp, denominator_fp;
	s64 dsc_byte_count_fp;
	u32 dsc_byte_count, temp1, temp2;

	panel = container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);

	if (!drm_dsc->slice_width ||
			!drm_dsc->slice_height ||
			intf_width < drm_dsc->slice_width) {
		drm_dbg_dp(panel->drm_dev, "invalid input, intf_width=%d slice_width=%d\n",
			intf_width, drm_dsc->slice_width);
		return -EINVAL;
	}

	slice_per_pkt = msm_dp_dsc->slice_per_pkt;
	slice_per_intf = DIV_ROUND_UP(intf_width,
				drm_dsc->slice_width);

	bpp = drm_dsc->bits_per_pixel >> 4;
	bytes_in_slice = DIV_ROUND_UP(drm_dsc->slice_width * bpp, 8);

	msm_dp_dsc->bytes_per_pkt = bytes_in_slice * slice_per_pkt;
	comp_ratio = mult_frac(100,
				dto_params->src_bpp,
				dto_params->tgt_bpp);

	temp1_fp = drm_fixp_from_fraction(comp_ratio, 100);
	temp2_fp = drm_int2fixp(slice_per_pkt * 8);
	denominator_fp = drm_fixp_mul(temp1_fp, temp2_fp);
	numerator_fp = drm_int2fixp(intf_width * drm_dsc->bits_per_component * 3);
	dsc_byte_count_fp = drm_fixp_div(numerator_fp, denominator_fp);
	dsc_byte_count = drm_fixp2int_ceil(dsc_byte_count_fp);

	temp1 = dsc_byte_count * slice_per_intf;
	temp2 = temp1;
	if (temp1 % 3 != 0)
		temp1 += 3 - (temp1 % 3);

	msm_dp_dsc->eol_byte_num = temp1 - temp2;

	temp1_fp = drm_fixp_from_fraction(slice_per_intf, 6);
	temp2_fp = drm_fixp_mul(dsc_byte_count_fp, temp1_fp);
	msm_dp_dsc->pclk_per_line = drm_fixp2int_ceil(temp2_fp) - 1;

	msm_dp_dsc->dsc_overhead_fp =
				msm_dp_panel_dsc_bw_overhead_fixp(msm_dp_panel, msm_dp_dsc, dsc_byte_count);

	return 0;
}

static int msm_dp_panel_dsc_populate_params(struct msm_dp_panel *msm_dp_panel,
			struct msm_dp_display_mode *msm_dp_mode,
			u32 num_dsc,
			struct msm_dp_dto *dto_params)
{
	struct msm_dp_panel_private *panel =
				container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);
	int rc;

	if ((rc = msm_dp_panel_dsc_prepare(msm_dp_panel, msm_dp_mode, num_dsc, dto_params))) {
		drm_dbg_dp(panel->drm_dev,
				"prepare dsc basic params failed\n");
		return rc;
	}

	if ((rc = msm_populate_dsc_params(msm_dp_panel, &msm_dp_mode->drm_dsc))) {
		drm_dbg_dp(panel->drm_dev,
				"failed populating dsc params\n");
		return rc;
	}

	if ((rc = msm_dp_populate_dsc_private_params(msm_dp_panel, msm_dp_mode, dto_params))) {
		drm_dbg_dp(panel->drm_dev,
				"failed populating other dsc params\n");
		return rc;
	}

	return 0;
}

int msm_dp_panel_init_panel_info(struct msm_dp_panel *msm_dp_panel)
{
	struct drm_display_mode *drm_mode;
	struct msm_dp_display_mode_cfg *mode_cfg;
	enum msm_mode_dsc_cfg dsc_cfg;
	struct msm_dp_panel_private *panel;
	struct msm_dp_display_mode *msm_dp_mode = &msm_dp_panel->msm_dp_mode;

	drm_mode = &msm_dp_mode->drm_mode;
	mode_cfg = &msm_dp_mode->mode_cfg;

	panel = container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);

	/*
	 * print resolution info as this is a result
	 * of user initiated action of cable connection
	 */
	drm_dbg_dp(panel->drm_dev, "SET NEW RESOLUTION:\n");
	drm_dbg_dp(panel->drm_dev, "%dx%d@%dfps\n",
		drm_mode->hdisplay, drm_mode->vdisplay, drm_mode_vrefresh(drm_mode));
	drm_dbg_dp(panel->drm_dev,
			"h_porches(back|front|width) = (%d|%d|%d)\n",
			drm_mode->htotal - drm_mode->hsync_end,
			drm_mode->hsync_start - drm_mode->hdisplay,
			drm_mode->hsync_end - drm_mode->hsync_start);
	drm_dbg_dp(panel->drm_dev,
			"v_porches(back|front|width) = (%d|%d|%d)\n",
			drm_mode->vtotal - drm_mode->vsync_end,
			drm_mode->vsync_start - drm_mode->vdisplay,
			drm_mode->vsync_end - drm_mode->vsync_start);
	drm_dbg_dp(panel->drm_dev, "pixel clock (KHz)=(%d)\n",
				drm_mode->clock);
	drm_dbg_dp(panel->drm_dev, "bpp = %d\n", mode_cfg->bpp);

	dsc_cfg = msm_dp_panel->dsc_cap.enabled ?
				MSM_MODE_DSC_REQUIRED : MSM_MODE_DSC_UNAVAILABLE;

	*mode_cfg = msm_dp_panel_get_mode_cfg(
				msm_dp_panel,
				mode_cfg->bpp,
				dsc_cfg,
				&msm_dp_panel->msm_dp_mode,
				msm_dp_panel->dsc_cap.num_dsc);

	if (mode_cfg->dsc != dsc_cfg) {
		drm_dbg_dp(panel->drm_dev,
				"dsc config failed\n");
		return -EINVAL;
	}
	msm_dp_panel->fec_cap.enabled = mode_cfg->fec_available;
	drm_dbg_dp(panel->drm_dev, "updated_bpp=%d fec=%d dsc=%d dsc_bpp=%d\n",
				mode_cfg->bpp,
				msm_dp_panel->fec_cap.enabled,
				msm_dp_panel->dsc_cap.enabled,
				mode_cfg->tgt_bpp);

	return 0;
}

u8 msm_dp_panel_get_colorimetry_config(struct msm_dp_panel *msm_dp_panel)
{
	u8 colorimetry;
	u32 colorspace;
	u32 cc;
	struct msm_dp_panel_private *panel;

	panel = container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);

	cc = msm_dp_link_get_colorimetry_config(panel->link);
	/*
	 * If there is a non-zero value then compliance test-case
	 * is going on, otherwise we can honor the colorspace setting
	 */
	if (cc)
		return cc;

	colorspace = msm_dp_panel->connector->state->colorspace;
	switch (colorspace) {
	case DRM_MODE_COLORIMETRY_DCI_P3_RGB_D65:
	case DRM_MODE_COLORIMETRY_DCI_P3_RGB_THEATER:
		colorimetry = 0x7;
		break;
	case DRM_MODE_COLORIMETRY_RGB_WIDE_FIXED:
		colorimetry = 0x3;
		break;
	case DRM_MODE_COLORIMETRY_RGB_WIDE_FLOAT:
		colorimetry = 0xb;
		break;
	case DRM_MODE_COLORIMETRY_OPRGB:
		colorimetry = 0xc;
		break;
	default:
		colorimetry = 0;
	}

	return colorimetry;
}

void msm_dp_panel_config_dsc_dto(struct msm_dp_panel *msm_dp_panel, bool enable)
{
	struct msm_dp_panel_private *panel;
	struct msm_dp_dsc_cfg *msm_dp_dsc;
	u32 reg = 0;

	bool dto_en = enable;
	u32 dto_n;
	u32 dto_d;
	u32 dto_count = 0;

	if (!msm_dp_panel) {
		DRM_ERROR("invalid input\n");
		return;
	}

	panel = container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);

	if (dto_en) {
		msm_dp_dsc = &msm_dp_panel->msm_dp_mode.msm_dp_dsc;

		dto_count = msm_dp_dsc->pclk_per_line;
		dto_n = msm_dp_dsc->dto.dto_n;
		dto_d = msm_dp_dsc->dto.dto_d;

		msm_dp_read_p0(panel, MMSS_DP_DSC_DTO);
		reg |= BIT(0);
		reg |= BIT(3);
		reg |= (dto_n << 8);
		reg |= (dto_d << 16);
	}
	msm_dp_write_p0(panel, MMSS_DP_DSC_DTO_COUNT, dto_count);
	msm_dp_write_p0(panel, MMSS_DP_DSC_DTO, reg);
}

void msm_dp_panel_override_ack_dto(struct msm_dp_panel *msm_dp_panel, bool not_ack)
{
	struct msm_dp_panel_private *panel;
	u32 dsc_dto;

	if (!msm_dp_panel) {
		DRM_ERROR("invalid input\n");
		return;
	}

	panel = container_of(msm_dp_panel, struct msm_dp_panel_private, msm_dp_panel);

	dsc_dto = msm_dp_read_p0(panel, MMSS_DP_DSC_DTO);
	if (not_ack)
		dsc_dto &= ~BIT(1);
	else
		dsc_dto = BIT(1);

	msm_dp_write_p0(panel, MMSS_DP_DSC_DTO, dsc_dto);
}

struct msm_dp_panel *msm_dp_panel_get(struct device *dev, struct drm_dp_aux *aux,
			      struct msm_dp_link *link,
			      void __iomem *link_base,
			      void __iomem *p0_base)
{
	struct msm_dp_panel_private *panel;
	struct msm_dp_panel *msm_dp_panel;

	if (!dev || !aux || !link) {
		DRM_ERROR("invalid input\n");
		return ERR_PTR(-EINVAL);
	}

	panel = devm_kzalloc(dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return ERR_PTR(-ENOMEM);

	panel->dev = dev;
	panel->aux = aux;
	panel->link = link;
	panel->link_base = link_base;
	panel->p0_base = p0_base;

	msm_dp_panel = &panel->msm_dp_panel;
	msm_dp_panel->max_bw_code = DP_LINK_BW_8_1;

	return msm_dp_panel;
}

void msm_dp_panel_put(struct msm_dp_panel *msm_dp_panel)
{
	if (!msm_dp_panel)
		return;

	drm_edid_free(msm_dp_panel->drm_edid);
}
