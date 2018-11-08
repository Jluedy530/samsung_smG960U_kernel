/*
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"[drm-dp] %s: " fmt, __func__

#include <linux/delay.h>
#include <drm/drm_dp_helper.h>

#include "dp_catalog.h"
#include "dp_reg.h"
#ifdef CONFIG_SEC_DISPLAYPORT
#include "secdp.h"
#endif

#define DP_GET_MSB(x)	(x >> 8)
#define DP_GET_LSB(x)	(x & 0xff)

#define dp_read(offset) readl_relaxed((offset))
#define dp_write(offset, data) writel_relaxed((data), (offset))

#define dp_catalog_get_priv(x) { \
	struct dp_catalog *dp_catalog; \
	dp_catalog = container_of(x, struct dp_catalog, x); \
	catalog = container_of(dp_catalog, struct dp_catalog_private, \
				dp_catalog); \
}

#define DP_INTERRUPT_STATUS1 \
	(DP_INTR_AUX_I2C_DONE| \
	DP_INTR_WRONG_ADDR | DP_INTR_TIMEOUT | \
	DP_INTR_NACK_DEFER | DP_INTR_WRONG_DATA_CNT | \
	DP_INTR_I2C_NACK | DP_INTR_I2C_DEFER | \
	DP_INTR_PLL_UNLOCKED | DP_INTR_AUX_ERROR)

#define DP_INTR_MASK1		(DP_INTERRUPT_STATUS1 << 2)

#define DP_INTERRUPT_STATUS2 \
	(DP_INTR_READY_FOR_VIDEO | DP_INTR_IDLE_PATTERN_SENT | \
	DP_INTR_FRAME_END | DP_INTR_CRC_UPDATED)

#define DP_INTR_MASK2		(DP_INTERRUPT_STATUS2 << 2)

static u8 const vm_pre_emphasis[4][4] = {
	{0x00, 0x0B, 0x12, 0xFF},       /* pe0, 0 db */
	{0x00, 0x0A, 0x12, 0xFF},       /* pe1, 3.5 db */
	{0x00, 0x0C, 0xFF, 0xFF},       /* pe2, 6.0 db */
	{0xFF, 0xFF, 0xFF, 0xFF}        /* pe3, 9.5 db */
};

/* voltage swing, 0.2v and 1.0v are not support */
static u8 const vm_voltage_swing[4][4] = {
	{0x07, 0x0F, 0x14, 0xFF}, /* sw0, 0.4v  */
	{0x11, 0x1D, 0x1F, 0xFF}, /* sw1, 0.6 v */
	{0x18, 0x1F, 0xFF, 0xFF}, /* sw1, 0.8 v */
	{0xFF, 0xFF, 0xFF, 0xFF}  /* sw1, 1.2 v, optional */
};

/* audio related catalog functions */
struct dp_catalog_private {
	struct device *dev;
	struct dp_io *io;

	u32 (*audio_map)[DP_AUDIO_SDP_HEADER_MAX];
	struct dp_catalog dp_catalog;
};

/* aux related catalog functions */
static u32 dp_catalog_aux_read_data(struct dp_catalog_aux *aux)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!aux) {
		pr_err("invalid input\n");
		goto end;
	}

	dp_catalog_get_priv(aux);
	base = catalog->io->dp_aux.base;

	return dp_read(base + DP_AUX_DATA);
end:
	return 0;
}

static int dp_catalog_aux_write_data(struct dp_catalog_aux *aux)
{
	int rc = 0;
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!aux) {
		pr_err("invalid input\n");
		rc = -EINVAL;
		goto end;
	}

	dp_catalog_get_priv(aux);
	base = catalog->io->dp_aux.base;

	dp_write(base + DP_AUX_DATA, aux->data);
end:
	return rc;
}

static int dp_catalog_aux_write_trans(struct dp_catalog_aux *aux)
{
	int rc = 0;
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!aux) {
		pr_err("invalid input\n");
		rc = -EINVAL;
		goto end;
	}

	dp_catalog_get_priv(aux);
	base = catalog->io->dp_aux.base;

	dp_write(base + DP_AUX_TRANS_CTRL, aux->data);
end:
	return rc;
}

static int dp_catalog_aux_clear_trans(struct dp_catalog_aux *aux, bool read)
{
	int rc = 0;
	u32 data = 0;
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!aux) {
		pr_err("invalid input\n");
		rc = -EINVAL;
		goto end;
	}

	dp_catalog_get_priv(aux);
	base = catalog->io->dp_aux.base;

	if (read) {
		data = dp_read(base + DP_AUX_TRANS_CTRL);
#ifdef CONFIG_SEC_DISPLAYPORT
		/* Prevent_CXX Major defect.
		 * Invalid Assignment: The type size of both side variables are different:
		 * "data" is 4 ( unsigned int ) and "data & 0xfffffffffffffdffUL" is 8 ( unsigned long )
		 */
		data &= ((u32)~BIT(9));
#else
		data &= ~BIT(9);
#endif
		dp_write(base + DP_AUX_TRANS_CTRL, data);
	} else {
		dp_write(base + DP_AUX_TRANS_CTRL, 0);
	}
end:
	return rc;
}

static void dp_catalog_aux_clear_hw_interrupts(struct dp_catalog_aux *aux)
{
	struct dp_catalog_private *catalog;
	void __iomem *phy_base;
	u32 data = 0;

	if (!aux) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(aux);
	phy_base = catalog->io->phy_io.base;

	data = dp_read(phy_base + DP_PHY_AUX_INTERRUPT_STATUS);
#ifdef CONFIG_SEC_DISPLAYPORT
	if (data)
#endif
	pr_debug("PHY_AUX_INTERRUPT_STATUS=0x%08x\n", data);

	dp_write(phy_base + DP_PHY_AUX_INTERRUPT_CLEAR, 0x1f);
	wmb(); /* make sure 0x1f is written before next write */
	dp_write(phy_base + DP_PHY_AUX_INTERRUPT_CLEAR, 0x9f);
	wmb(); /* make sure 0x9f is written before next write */
	dp_write(phy_base + DP_PHY_AUX_INTERRUPT_CLEAR, 0);
	wmb(); /* make sure register is cleared */
}

static void dp_catalog_aux_reset(struct dp_catalog_aux *aux)
{
	u32 aux_ctrl;
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!aux) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(aux);
	base = catalog->io->dp_aux.base;

	aux_ctrl = dp_read(base + DP_AUX_CTRL);

	aux_ctrl |= BIT(1);
	dp_write(base + DP_AUX_CTRL, aux_ctrl);
	usleep_range(1000, 1010); /* h/w recommended delay */

	aux_ctrl &= ~BIT(1);
	dp_write(base + DP_AUX_CTRL, aux_ctrl);
	wmb(); /* make sure AUX reset is done here */
}

static void dp_catalog_aux_enable(struct dp_catalog_aux *aux, bool enable)
{
	u32 aux_ctrl;
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!aux) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(aux);
	base = catalog->io->dp_aux.base;

	aux_ctrl = dp_read(base + DP_AUX_CTRL);

	if (enable) {
		aux_ctrl |= BIT(0);
		dp_write(base + DP_AUX_CTRL, aux_ctrl);
		wmb(); /* make sure AUX module is enabled */
		dp_write(base + DP_TIMEOUT_COUNT, 0xffff);
		dp_write(base + DP_AUX_LIMITS, 0xffff);
	} else {
		aux_ctrl &= ~BIT(0);
		dp_write(base + DP_AUX_CTRL, aux_ctrl);
	}
}

static void dp_catalog_aux_update_cfg(struct dp_catalog_aux *aux,
		struct dp_aux_cfg *cfg, enum dp_phy_aux_config_type type)
{
	struct dp_catalog_private *catalog;
	u32 new_index = 0, current_index = 0;

	if (!aux || !cfg || (type >= PHY_AUX_CFG_MAX)) {
		pr_err("invalid input\n");
		return;
	}

#ifdef CONFIG_SEC_DISPLAYPORT
	if (!secdp_get_cable_status()) {
		pr_info("cable is out\n");
		return;
	}
#endif

	dp_catalog_get_priv(aux);

	current_index = cfg[type].current_index;
	new_index = (current_index + 1) % cfg[type].cfg_cnt;
	pr_debug("Updating %s from 0x%08x to 0x%08x\n",
		dp_phy_aux_config_type_to_string(type),
	cfg[type].lut[current_index], cfg[type].lut[new_index]);

	dp_write(catalog->io->phy_io.base + cfg[type].offset,
			cfg[type].lut[new_index]);
	cfg[type].current_index = new_index;
}

static void dp_catalog_aux_setup(struct dp_catalog_aux *aux,
		struct dp_aux_cfg *cfg)
{
	struct dp_catalog_private *catalog;
	int i = 0;

	if (!aux || !cfg) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(aux);

	dp_write(catalog->io->phy_io.base + DP_PHY_PD_CTL, 0x65);
	wmb(); /* make sure PD programming happened */

	/* Turn on BIAS current for PHY/PLL */
	dp_write(catalog->io->dp_pll_io.base +
		QSERDES_COM_BIAS_EN_CLKBUFLR_EN, 0x1b);

	/* DP AUX CFG register programming */
	for (i = 0; i < PHY_AUX_CFG_MAX; i++) {
		pr_debug("%s: offset=0x%08x, value=0x%08x\n",
			dp_phy_aux_config_type_to_string(i),
			cfg[i].offset, cfg[i].lut[cfg[i].current_index]);
		dp_write(catalog->io->phy_io.base + cfg[i].offset,
			cfg[i].lut[cfg[i].current_index]);
	}

	dp_write(catalog->io->phy_io.base + DP_PHY_AUX_INTERRUPT_MASK, 0x1F);
	wmb(); /* make sure AUX configuration is done before enabling it */
}

static void dp_catalog_aux_get_irq(struct dp_catalog_aux *aux, bool cmd_busy)
{
	u32 ack;
	struct dp_catalog_private *catalog;
	void __iomem *ahb_base;

	if (!aux) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(aux);
	ahb_base = catalog->io->dp_ahb.base;

	aux->isr = dp_read(ahb_base + DP_INTR_STATUS);
	aux->isr &= ~DP_INTR_MASK1;
	ack = aux->isr & DP_INTERRUPT_STATUS1;
	ack <<= 1;
	ack |= DP_INTR_MASK1;
	dp_write(ahb_base + DP_INTR_STATUS, ack);
}

/* controller related catalog functions */
static u32 dp_catalog_ctrl_read_hdcp_status(struct dp_catalog_ctrl *ctrl)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!ctrl) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp_catalog_get_priv(ctrl);
	base = catalog->io->dp_ahb.base;

	return dp_read(base + DP_HDCP_STATUS);
}

static void dp_catalog_panel_setup_infoframe_sdp(struct dp_catalog_panel *panel)
{
	struct dp_catalog_private *catalog;
	struct drm_msm_ext_hdr_metadata *hdr;
	void __iomem *base;
	u32 header, parity, data;
	u8 buf[SZ_128], off = 0;

	if (!panel) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(panel);
	hdr = &panel->hdr_data.hdr_meta;
	base = catalog->io->dp_link.base;

	/* HEADER BYTE 1 */
	header = panel->hdr_data.vscext_header_byte1;
	parity = dp_header_get_parity(header);
	data   = ((header << HEADER_BYTE_1_BIT)
			| (parity << PARITY_BYTE_1_BIT));
	dp_write(base + MMSS_DP_VSCEXT_0, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	/* HEADER BYTE 2 */
	header = panel->hdr_data.vscext_header_byte2;
	parity = dp_header_get_parity(header);
	data   = ((header << HEADER_BYTE_2_BIT)
			| (parity << PARITY_BYTE_2_BIT));
	dp_write(base + MMSS_DP_VSCEXT_1, data);

	/* HEADER BYTE 3 */
	header = panel->hdr_data.vscext_header_byte3;
	parity = dp_header_get_parity(header);
	data   = ((header << HEADER_BYTE_3_BIT)
			| (parity << PARITY_BYTE_3_BIT));
	data |= dp_read(base + MMSS_DP_VSCEXT_1);
	dp_write(base + MMSS_DP_VSCEXT_1, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	data = panel->hdr_data.version;
	data |= panel->hdr_data.length << 8;
	data |= hdr->eotf << 16;
	dp_write(base + MMSS_DP_VSCEXT_2, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	data = (DP_GET_LSB(hdr->display_primaries_x[0]) |
		(DP_GET_MSB(hdr->display_primaries_x[0]) << 8) |
		(DP_GET_LSB(hdr->display_primaries_y[0]) << 16) |
		(DP_GET_MSB(hdr->display_primaries_y[0]) << 24));
	dp_write(base + MMSS_DP_VSCEXT_3, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	data = (DP_GET_LSB(hdr->display_primaries_x[1]) |
		(DP_GET_MSB(hdr->display_primaries_x[1]) << 8) |
		(DP_GET_LSB(hdr->display_primaries_y[1]) << 16) |
		(DP_GET_MSB(hdr->display_primaries_y[1]) << 24));
	dp_write(base + MMSS_DP_VSCEXT_4, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	data = (DP_GET_LSB(hdr->display_primaries_x[2]) |
		(DP_GET_MSB(hdr->display_primaries_x[2]) << 8) |
		(DP_GET_LSB(hdr->display_primaries_y[2]) << 16) |
		(DP_GET_MSB(hdr->display_primaries_y[2]) << 24));
	dp_write(base + MMSS_DP_VSCEXT_5, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	data = (DP_GET_LSB(hdr->white_point_x) |
		(DP_GET_MSB(hdr->white_point_x) << 8) |
		(DP_GET_LSB(hdr->white_point_y) << 16) |
		(DP_GET_MSB(hdr->white_point_y) << 24));
	dp_write(base + MMSS_DP_VSCEXT_6, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	data = (DP_GET_LSB(hdr->max_luminance) |
		(DP_GET_MSB(hdr->max_luminance) << 8) |
		(DP_GET_LSB(hdr->min_luminance) << 16) |
		(DP_GET_MSB(hdr->min_luminance) << 24));
	dp_write(base + MMSS_DP_VSCEXT_7, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	data = (DP_GET_LSB(hdr->max_content_light_level) |
		(DP_GET_MSB(hdr->max_content_light_level) << 8) |
		(DP_GET_LSB(hdr->max_average_light_level) << 16) |
		(DP_GET_MSB(hdr->max_average_light_level) << 24));
	dp_write(base + MMSS_DP_VSCEXT_8, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	data = 0;
	dp_write(base + MMSS_DP_VSCEXT_9, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	print_hex_dump(KERN_DEBUG, "[drm-dp] VSCEXT: ",
			DUMP_PREFIX_NONE, 16, 4, buf, off, false);
}

static void dp_catalog_panel_setup_vsc_sdp(struct dp_catalog_panel *panel)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;
	u32 header, parity, data;
	u8 bpc, off = 0;
	u8 buf[SZ_128];

	if (!panel) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(panel);
	base = catalog->io->dp_link.base;

	/* HEADER BYTE 1 */
	header = panel->hdr_data.vsc_header_byte1;
	parity = dp_header_get_parity(header);
	data   = ((header << HEADER_BYTE_1_BIT)
			| (parity << PARITY_BYTE_1_BIT));
	dp_write(base + MMSS_DP_GENERIC0_0, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	/* HEADER BYTE 2 */
	header = panel->hdr_data.vsc_header_byte2;
	parity = dp_header_get_parity(header);
	data   = ((header << HEADER_BYTE_2_BIT)
			| (parity << PARITY_BYTE_2_BIT));
	dp_write(base + MMSS_DP_GENERIC0_1, data);

	/* HEADER BYTE 3 */
	header = panel->hdr_data.vsc_header_byte3;
	parity = dp_header_get_parity(header);
	data   = ((header << HEADER_BYTE_3_BIT)
			| (parity << PARITY_BYTE_3_BIT));
	data |= dp_read(base + MMSS_DP_GENERIC0_1);
	dp_write(base + MMSS_DP_GENERIC0_1, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	data = 0;
	dp_write(base + MMSS_DP_GENERIC0_2, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	dp_write(base + MMSS_DP_GENERIC0_3, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	dp_write(base + MMSS_DP_GENERIC0_4, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	dp_write(base + MMSS_DP_GENERIC0_5, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	switch (panel->hdr_data.bpc) {
	default:
	case 10:
		bpc = BIT(1);
		break;
	case 8:
		bpc = BIT(0);
		break;
	case 6:
		bpc = 0;
		break;
	}

	data = (panel->hdr_data.colorimetry & 0xF) |
		((panel->hdr_data.pixel_encoding & 0xF) << 4) |
		(bpc << 8) |
		((panel->hdr_data.dynamic_range & 0x1) << 15) |
		((panel->hdr_data.content_type & 0x7) << 16);

	dp_write(base + MMSS_DP_GENERIC0_6, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	data = 0;
	dp_write(base + MMSS_DP_GENERIC0_7, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	dp_write(base + MMSS_DP_GENERIC0_8, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	dp_write(base + MMSS_DP_GENERIC0_9, data);
	memcpy(buf + off, &data, sizeof(data));
	off += sizeof(data);

	print_hex_dump(KERN_DEBUG, "[drm-dp] VSC: ",
			DUMP_PREFIX_NONE, 16, 4, buf, off, false);
}

static void dp_catalog_panel_config_hdr(struct dp_catalog_panel *panel, bool en)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;
	u32 cfg, cfg2, misc;

	if (!panel) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(panel);
	base = catalog->io->dp_link.base;

	cfg = dp_read(base + MMSS_DP_SDP_CFG);
	cfg2 = dp_read(base + MMSS_DP_SDP_CFG2);
	misc = dp_read(base + DP_MISC1_MISC0);

	if (en) {
		/* VSCEXT_SDP_EN, GEN0_SDP_EN */
		cfg |= BIT(16) | BIT(17);
		dp_write(base + MMSS_DP_SDP_CFG, cfg);

		/* EXTN_SDPSIZE GENERIC0_SDPSIZE */
		cfg2 |= BIT(15) | BIT(16);
		dp_write(base + MMSS_DP_SDP_CFG2, cfg2);

		dp_catalog_panel_setup_vsc_sdp(panel);
		dp_catalog_panel_setup_infoframe_sdp(panel);

		/* indicates presence of VSC (BIT(6) of MISC1) */
		misc |= BIT(14);

		if (panel->hdr_data.hdr_meta.eotf)
			pr_debug("Enabled\n");
		else
			pr_debug("Reset\n");
	} else {
		/* VSCEXT_SDP_EN, GEN0_SDP_EN */
		cfg &= ~BIT(16) & ~BIT(17);
		dp_write(base + MMSS_DP_SDP_CFG, cfg);

		/* EXTN_SDPSIZE GENERIC0_SDPSIZE */
		cfg2 &= ~BIT(15) & ~BIT(16);
		dp_write(base + MMSS_DP_SDP_CFG2, cfg2);

		/* switch back to MSA */
		misc &= ~BIT(14);

		pr_debug("Disabled\n");
	}

	dp_write(base + DP_MISC1_MISC0, misc);

	dp_write(base + MMSS_DP_SDP_CFG3, 0x01);
	dp_write(base + MMSS_DP_SDP_CFG3, 0x00);
}

static void dp_catalog_ctrl_update_transfer_unit(struct dp_catalog_ctrl *ctrl)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);
	base = catalog->io->dp_link.base;

	dp_write(base + DP_VALID_BOUNDARY, ctrl->valid_boundary);
	dp_write(base + DP_TU, ctrl->dp_tu);
	dp_write(base + DP_VALID_BOUNDARY_2, ctrl->valid_boundary2);
}

static void dp_catalog_ctrl_state_ctrl(struct dp_catalog_ctrl *ctrl, u32 state)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	pr_debug("+++\n");

	dp_catalog_get_priv(ctrl);
	base = catalog->io->dp_link.base;

	dp_write(base + DP_STATE_CTRL, state);
}

static void dp_catalog_ctrl_config_ctrl(struct dp_catalog_ctrl *ctrl, u32 cfg)
{
	struct dp_catalog_private *catalog;
	void __iomem *link_base;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);
	link_base = catalog->io->dp_link.base;

	pr_debug("DP_CONFIGURATION_CTRL=0x%x\n", cfg);

	dp_write(link_base + DP_CONFIGURATION_CTRL, cfg);
}

static void dp_catalog_ctrl_lane_mapping(struct dp_catalog_ctrl *ctrl)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);
	base = catalog->io->dp_link.base;

	dp_write(base + DP_LOGICAL2PHYSICAL_LANE_MAPPING, 0xe4);
}

static void dp_catalog_ctrl_mainlink_ctrl(struct dp_catalog_ctrl *ctrl,
						bool enable)
{
	u32 mainlink_ctrl;
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	pr_debug("+++\n");

	dp_catalog_get_priv(ctrl);
	base = catalog->io->dp_link.base;

	if (enable) {
		dp_write(base + DP_MAINLINK_CTRL, 0x02000000);
		wmb(); /* make sure mainlink is turned off before reset */
		dp_write(base + DP_MAINLINK_CTRL, 0x02000002);
		wmb(); /* make sure mainlink entered reset */
		dp_write(base + DP_MAINLINK_CTRL, 0x02000000);
		wmb(); /* make sure mainlink reset done */
		dp_write(base + DP_MAINLINK_CTRL, 0x02000001);
		wmb(); /* make sure mainlink turned on */
	} else {
		mainlink_ctrl = dp_read(base + DP_MAINLINK_CTRL);
		mainlink_ctrl &= ~BIT(0);
		dp_write(base + DP_MAINLINK_CTRL, mainlink_ctrl);
	}
}

static void dp_catalog_ctrl_config_misc(struct dp_catalog_ctrl *ctrl,
					u32 cc, u32 tb)
{
	u32 misc_val = cc;
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);
	base = catalog->io->dp_link.base;

	misc_val |= (tb << 5);
	misc_val |= BIT(0); /* Configure clock to synchronous mode */

	pr_debug("misc settings = 0x%x\n", misc_val);
	dp_write(base + DP_MISC1_MISC0, misc_val);
}

static void dp_catalog_ctrl_config_msa(struct dp_catalog_ctrl *ctrl,
					u32 rate, u32 stream_rate_khz,
					bool fixed_nvid)
{
	u32 pixel_m, pixel_n;
	u32 mvid, nvid;
	u64 mvid_calc;
	u32 const nvid_fixed = 0x8000;
	u32 const link_rate_hbr2 = 540000;
	u32 const link_rate_hbr3 = 810000;
	struct dp_catalog_private *catalog;
	void __iomem *base_cc, *base_ctrl;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);
	if (fixed_nvid) {
		pr_debug("use fixed NVID=0x%x\n", nvid_fixed);
		nvid = nvid_fixed;

		pr_debug("link rate=%dkbps, stream_rate_khz=%uKhz",
			rate, stream_rate_khz);

		/*
		 * For intermediate results, use 64 bit arithmetic to avoid
		 * loss of precision.
		 */
		mvid_calc = (u64) stream_rate_khz * nvid;
		mvid_calc = div_u64(mvid_calc, rate);

		/*
		 * truncate back to 32 bits as this final divided value will
		 * always be within the range of a 32 bit unsigned int.
		 */
		mvid = (u32) mvid_calc;
	} else {
		base_cc = catalog->io->dp_cc_io.base;

		pixel_m = dp_read(base_cc + MMSS_DP_PIXEL_M);
		pixel_n = dp_read(base_cc + MMSS_DP_PIXEL_N);
		pr_debug("pixel_m=0x%x, pixel_n=0x%x\n", pixel_m, pixel_n);

		mvid = (pixel_m & 0xFFFF) * 5;
		nvid = (0xFFFF & (~pixel_n)) + (pixel_m & 0xFFFF);

		pr_debug("rate = %d\n", rate);

		if (link_rate_hbr2 == rate)
			nvid *= 2;

		if (link_rate_hbr3 == rate)
			nvid *= 3;
	}

	base_ctrl = catalog->io->dp_link.base;
	pr_debug("mvid=0x%x, nvid=0x%x\n", mvid, nvid);
	dp_write(base_ctrl + DP_SOFTWARE_MVID, mvid);
	dp_write(base_ctrl + DP_SOFTWARE_NVID, nvid);
}

static void dp_catalog_ctrl_set_pattern(struct dp_catalog_ctrl *ctrl,
					u32 pattern)
{
	int bit, cnt = 10;
	u32 data;
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);
	base = catalog->io->dp_link.base;

	bit = 1;
	bit <<= (pattern - 1);
	pr_debug("hw: bit=%d train=%d\n", bit, pattern);
	dp_write(base + DP_STATE_CTRL, bit);

	bit = 8;
	bit <<= (pattern - 1);

	while (cnt--) {
		data = dp_read(base + DP_MAINLINK_READY);
		if (data & bit)
			break;
	}

	if (cnt == 0)
		pr_err("set link_train=%d failed\n", pattern);
}

static void dp_catalog_ctrl_usb_reset(struct dp_catalog_ctrl *ctrl, bool flip)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);

	base = catalog->io->usb3_dp_com.base;

	dp_write(base + USB3_DP_COM_RESET_OVRD_CTRL, 0x0a);
	dp_write(base + USB3_DP_COM_PHY_MODE_CTRL, 0x02);
	dp_write(base + USB3_DP_COM_SW_RESET, 0x01);
	/* make sure usb3 com phy software reset is done */
	wmb();

	if (!flip) /* CC1 */
		dp_write(base + USB3_DP_COM_TYPEC_CTRL, 0x02);
	else /* CC2 */
		dp_write(base + USB3_DP_COM_TYPEC_CTRL, 0x03);

	dp_write(base + USB3_DP_COM_SWI_CTRL, 0x00);
	dp_write(base + USB3_DP_COM_SW_RESET, 0x00);
	/* make sure the software reset is done */
	wmb();

	dp_write(base + USB3_DP_COM_POWER_DOWN_CTRL, 0x01);
	dp_write(base + USB3_DP_COM_RESET_OVRD_CTRL, 0x00);
	/* make sure phy is brought out of reset */
	wmb();
}

static void dp_catalog_panel_tpg_cfg(struct dp_catalog_panel *panel,
	bool enable)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!panel) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(panel);
	base = catalog->io->dp_p0.base;

	if (!enable) {
		dp_write(base + MMSS_DP_TPG_MAIN_CONTROL, 0x0);
		dp_write(base + MMSS_DP_BIST_ENABLE, 0x0);
		dp_write(base + MMSS_DP_TIMING_ENGINE_EN, 0x0);
		wmb(); /* ensure Timing generator is turned off */
		return;
	}

	dp_write(base + MMSS_DP_INTF_CONFIG, 0x0);
	dp_write(base + MMSS_DP_INTF_HSYNC_CTL, panel->hsync_ctl);
	dp_write(base + MMSS_DP_INTF_VSYNC_PERIOD_F0, panel->vsync_period *
			panel->hsync_period);
	dp_write(base + MMSS_DP_INTF_VSYNC_PULSE_WIDTH_F0, panel->v_sync_width *
			panel->hsync_period);
	dp_write(base + MMSS_DP_INTF_VSYNC_PERIOD_F1, 0);
	dp_write(base + MMSS_DP_INTF_VSYNC_PULSE_WIDTH_F1, 0);
	dp_write(base + MMSS_DP_INTF_DISPLAY_HCTL, panel->display_hctl);
	dp_write(base + MMSS_DP_INTF_ACTIVE_HCTL, 0);
	dp_write(base + MMSS_INTF_DISPLAY_V_START_F0, panel->display_v_start);
	dp_write(base + MMSS_DP_INTF_DISPLAY_V_END_F0, panel->display_v_end);
	dp_write(base + MMSS_INTF_DISPLAY_V_START_F1, 0);
	dp_write(base + MMSS_DP_INTF_DISPLAY_V_END_F1, 0);
	dp_write(base + MMSS_DP_INTF_ACTIVE_V_START_F0, 0);
	dp_write(base + MMSS_DP_INTF_ACTIVE_V_END_F0, 0);
	dp_write(base + MMSS_DP_INTF_ACTIVE_V_START_F1, 0);
	dp_write(base + MMSS_DP_INTF_ACTIVE_V_END_F1, 0);
	dp_write(base + MMSS_DP_INTF_POLARITY_CTL, 0);
	wmb(); /* ensure TPG registers are programmed */

	dp_write(base + MMSS_DP_TPG_MAIN_CONTROL, 0x100);
	dp_write(base + MMSS_DP_TPG_VIDEO_CONFIG, 0x5);
	wmb(); /* ensure TPG config is programmed */
	dp_write(base + MMSS_DP_BIST_ENABLE, 0x1);
	dp_write(base + MMSS_DP_TIMING_ENGINE_EN, 0x1);
	wmb(); /* ensure Timing generator is turned on */
}

static void dp_catalog_ctrl_reset(struct dp_catalog_ctrl *ctrl)
{
	u32 sw_reset;
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);
	base = catalog->io->dp_ahb.base;

	sw_reset = dp_read(base + DP_SW_RESET);

	sw_reset |= BIT(0);
	dp_write(base + DP_SW_RESET, sw_reset);
	usleep_range(1000, 1010); /* h/w recommended delay */

	sw_reset &= ~BIT(0);
	dp_write(base + DP_SW_RESET, sw_reset);
}

static bool dp_catalog_ctrl_mainlink_ready(struct dp_catalog_ctrl *ctrl)
{
	u32 data;
	int cnt = 10;
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!ctrl) {
		pr_err("invalid input\n");
		goto end;
	}

	dp_catalog_get_priv(ctrl);
	base = catalog->io->dp_link.base;

	while (--cnt) {
		/* DP_MAINLINK_READY */
		data = dp_read(base + DP_MAINLINK_READY);
		if (data & BIT(0))
			return true;

		usleep_range(1000, 1010); /* 1ms wait before next reg read */
	}
	pr_err("mainlink not ready\n");
end:
	return false;
}

static void dp_catalog_ctrl_enable_irq(struct dp_catalog_ctrl *ctrl,
						bool enable)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);
	base = catalog->io->dp_ahb.base;

	if (enable) {
		dp_write(base + DP_INTR_STATUS, DP_INTR_MASK1);
		dp_write(base + DP_INTR_STATUS2, DP_INTR_MASK2);
	} else {
		dp_write(base + DP_INTR_STATUS, 0x00);
		dp_write(base + DP_INTR_STATUS2, 0x00);
	}
}

static void dp_catalog_ctrl_hpd_config(struct dp_catalog_ctrl *ctrl, bool en)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);
	base = catalog->io->dp_aux.base;

	if (en) {
		u32 reftimer = dp_read(base + DP_DP_HPD_REFTIMER);

		dp_write(base + DP_DP_HPD_INT_ACK, 0xF);
		dp_write(base + DP_DP_HPD_INT_MASK, 0xF);

		/* Enabling REFTIMER */
		reftimer |= BIT(16);
		dp_write(base + DP_DP_HPD_REFTIMER, 0xF);
		/* Enable HPD */
		dp_write(base + DP_DP_HPD_CTRL, 0x1);
	} else {
		/*Disable HPD */
		dp_write(base + DP_DP_HPD_CTRL, 0x0);
	}
}

static void dp_catalog_ctrl_get_interrupt(struct dp_catalog_ctrl *ctrl)
{
	u32 ack = 0;
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);
	base = catalog->io->dp_ahb.base;

	ctrl->isr = dp_read(base + DP_INTR_STATUS2);
	ctrl->isr &= ~DP_INTR_MASK2;
	ack = ctrl->isr & DP_INTERRUPT_STATUS2;
	ack <<= 1;
	ack |= DP_INTR_MASK2;
	dp_write(base + DP_INTR_STATUS2, ack);
}

static void dp_catalog_ctrl_phy_reset(struct dp_catalog_ctrl *ctrl)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);
	base = catalog->io->dp_ahb.base;

	dp_write(base + DP_PHY_CTRL, 0x5); /* bit 0 & 2 */
	usleep_range(1000, 1010); /* h/w recommended delay */
	dp_write(base + DP_PHY_CTRL, 0x0);
	wmb(); /* make sure PHY reset done */
}

static void dp_catalog_ctrl_phy_lane_cfg(struct dp_catalog_ctrl *ctrl,
		bool flipped, u8 ln_cnt)
{
	u32 info = 0x0;
	struct dp_catalog_private *catalog;
	u8 orientation = BIT(!!flipped);

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);

	info |= (ln_cnt & 0x0F);
	info |= ((orientation & 0x0F) << 4);
	pr_debug("Shared Info = 0x%x\n", info);

	dp_write(catalog->io->phy_io.base + DP_PHY_SPARE0, info);
}

static void dp_catalog_ctrl_update_vx_px(struct dp_catalog_ctrl *ctrl,
		u8 v_level, u8 p_level)
{
	struct dp_catalog_private *catalog;
	void __iomem *base0, *base1;
	u8 value0, value1;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);
	base0 = catalog->io->ln_tx0_io.base;
	base1 = catalog->io->ln_tx1_io.base;

	pr_debug("hw: v=%d p=%d\n", v_level, p_level);

	value0 = vm_voltage_swing[v_level][p_level];
	value1 = vm_pre_emphasis[v_level][p_level];

	/* program default setting first */
	dp_write(base0 + TXn_TX_DRV_LVL, 0x2A);
	dp_write(base1 + TXn_TX_DRV_LVL, 0x2A);
	dp_write(base0 + TXn_TX_EMP_POST1_LVL, 0x20);
	dp_write(base1 + TXn_TX_EMP_POST1_LVL, 0x20);

	/* Enable MUX to use Cursor values from these registers */
	value0 |= BIT(5);
	value1 |= BIT(5);

	/* Configure host and panel only if both values are allowed */
	if (value0 != 0xFF && value1 != 0xFF) {
		dp_write(base0 + TXn_TX_DRV_LVL, value0);
		dp_write(base1 + TXn_TX_DRV_LVL, value0);
		dp_write(base0 + TXn_TX_EMP_POST1_LVL, value1);
		dp_write(base1 + TXn_TX_EMP_POST1_LVL, value1);

		pr_debug("hw: vx_value=0x%x px_value=0x%x\n",
			value0, value1);
	} else {
		pr_err("invalid vx (0x%x=0x%x), px (0x%x=0x%x\n",
			v_level, value0, p_level, value1);
	}
}

static void dp_catalog_ctrl_send_phy_pattern(struct dp_catalog_ctrl *ctrl,
			u32 pattern)
{
	struct dp_catalog_private *catalog;
	u32 value = 0x0;
	void __iomem *base = NULL;

	if (!ctrl) {
		pr_err("invalid input\n");
		return;
	}

	dp_catalog_get_priv(ctrl);

	base = catalog->io->dp_link.base;

	dp_write(base + DP_STATE_CTRL, 0x0);

	switch (pattern) {
	case DP_TEST_PHY_PATTERN_D10_2_NO_SCRAMBLING:
		dp_write(base + DP_STATE_CTRL, 0x1);
		break;
	case DP_TEST_PHY_PATTERN_SYMBOL_ERR_MEASUREMENT_CNT:
		value &= ~(1 << 16);
		dp_write(base + DP_HBR2_COMPLIANCE_SCRAMBLER_RESET, value);
		value |= 0xFC;
		dp_write(base + DP_HBR2_COMPLIANCE_SCRAMBLER_RESET, value);
		dp_write(base + DP_MAINLINK_LEVELS, 0x2);
		dp_write(base + DP_STATE_CTRL, 0x10);
		break;
	case DP_TEST_PHY_PATTERN_PRBS7:
		dp_write(base + DP_STATE_CTRL, 0x20);
		break;
	case DP_TEST_PHY_PATTERN_80_BIT_CUSTOM_PATTERN:
		dp_write(base + DP_STATE_CTRL, 0x40);
		/* 00111110000011111000001111100000 */
		dp_write(base + DP_TEST_80BIT_CUSTOM_PATTERN_REG0, 0x3E0F83E0);
		/* 00001111100000111110000011111000 */
		dp_write(base + DP_TEST_80BIT_CUSTOM_PATTERN_REG1, 0x0F83E0F8);
		/* 1111100000111110 */
		dp_write(base + DP_TEST_80BIT_CUSTOM_PATTERN_REG2, 0x0000F83E);
		break;
	case DP_TEST_PHY_PATTERN_CP2520_PATTERN_1:
		value = BIT(16);
		dp_write(base + DP_HBR2_COMPLIANCE_SCRAMBLER_RESET, value);
		value |= 0xFC;
		dp_write(base + DP_HBR2_COMPLIANCE_SCRAMBLER_RESET, value);
		dp_write(base + DP_MAINLINK_LEVELS, 0x2);
		dp_write(base + DP_STATE_CTRL, 0x10);
		break;
	case DP_TEST_PHY_PATTERN_CP2520_PATTERN_3:
		dp_write(base + DP_MAINLINK_CTRL, 0x11);
		dp_write(base + DP_STATE_CTRL, 0x8);
		break;
	default:
		pr_debug("No valid test pattern requested: 0x%x\n", pattern);
		return;
	}

	/* Make sure the test pattern is programmed in the hardware */
	wmb();
}

static u32 dp_catalog_ctrl_read_phy_pattern(struct dp_catalog_ctrl *ctrl)
{
	struct dp_catalog_private *catalog;
	void __iomem *base = NULL;

	if (!ctrl) {
		pr_err("invalid input\n");
		return 0;
	}

	dp_catalog_get_priv(ctrl);

	base = catalog->io->dp_link.base;

	return dp_read(base + DP_MAINLINK_READY);
}

/* panel related catalog functions */
static int dp_catalog_panel_timing_cfg(struct dp_catalog_panel *panel)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;

	if (!panel) {
		pr_err("invalid input\n");
		goto end;
	}

	dp_catalog_get_priv(panel);
	base = catalog->io->dp_link.base;

	dp_write(base + DP_TOTAL_HOR_VER, panel->total);
	dp_write(base + DP_START_HOR_VER_FROM_SYNC, panel->sync_start);
	dp_write(base + DP_HSYNC_VSYNC_WIDTH_POLARITY, panel->width_blanking);
	dp_write(base + DP_ACTIVE_HOR_VER, panel->dp_active);
end:
	return 0;
}

static void dp_catalog_audio_init(struct dp_catalog_audio *audio)
{
	struct dp_catalog_private *catalog;
	static u32 sdp_map[][DP_AUDIO_SDP_HEADER_MAX] = {
		{
			MMSS_DP_AUDIO_STREAM_0,
			MMSS_DP_AUDIO_STREAM_1,
			MMSS_DP_AUDIO_STREAM_1,
		},
		{
			MMSS_DP_AUDIO_TIMESTAMP_0,
			MMSS_DP_AUDIO_TIMESTAMP_1,
			MMSS_DP_AUDIO_TIMESTAMP_1,
		},
		{
			MMSS_DP_AUDIO_INFOFRAME_0,
			MMSS_DP_AUDIO_INFOFRAME_1,
			MMSS_DP_AUDIO_INFOFRAME_1,
		},
		{
			MMSS_DP_AUDIO_COPYMANAGEMENT_0,
			MMSS_DP_AUDIO_COPYMANAGEMENT_1,
			MMSS_DP_AUDIO_COPYMANAGEMENT_1,
		},
		{
			MMSS_DP_AUDIO_ISRC_0,
			MMSS_DP_AUDIO_ISRC_1,
			MMSS_DP_AUDIO_ISRC_1,
		},
	};

	if (!audio)
		return;

	dp_catalog_get_priv(audio);

	catalog->audio_map = sdp_map;
}

static void dp_catalog_audio_config_sdp(struct dp_catalog_audio *audio)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;
	u32 sdp_cfg = 0;
	u32 sdp_cfg2 = 0;

	if (!audio)
		return;

	dp_catalog_get_priv(audio);
	base = catalog->io->dp_link.base;

	sdp_cfg = dp_read(base + MMSS_DP_SDP_CFG);

	/* AUDIO_TIMESTAMP_SDP_EN */
	sdp_cfg |= BIT(1);
	/* AUDIO_STREAM_SDP_EN */
	sdp_cfg |= BIT(2);
	/* AUDIO_COPY_MANAGEMENT_SDP_EN */
	sdp_cfg |= BIT(5);
	/* AUDIO_ISRC_SDP_EN  */
	sdp_cfg |= BIT(6);
	/* AUDIO_INFOFRAME_SDP_EN  */
	sdp_cfg |= BIT(20);

	pr_debug("sdp_cfg = 0x%x\n", sdp_cfg);
	dp_write(base + MMSS_DP_SDP_CFG, sdp_cfg);

	sdp_cfg2 = dp_read(base + MMSS_DP_SDP_CFG2);
	/* IFRM_REGSRC -> Do not use reg values */
	sdp_cfg2 &= ~BIT(0);
	/* AUDIO_STREAM_HB3_REGSRC-> Do not use reg values */
	sdp_cfg2 &= ~BIT(1);

	pr_debug("sdp_cfg2 = 0x%x\n", sdp_cfg2);
	dp_write(base + MMSS_DP_SDP_CFG2, sdp_cfg2);
}

static void dp_catalog_audio_get_header(struct dp_catalog_audio *audio)
{
	struct dp_catalog_private *catalog;
	u32 (*sdp_map)[DP_AUDIO_SDP_HEADER_MAX];
	void __iomem *base;
	enum dp_catalog_audio_sdp_type sdp;
	enum dp_catalog_audio_header_type header;

	if (!audio)
		return;

	dp_catalog_get_priv(audio);

	base    = catalog->io->dp_link.base;
	sdp_map = catalog->audio_map;
	sdp     = audio->sdp_type;
	header  = audio->sdp_header;

	audio->data = dp_read(base + sdp_map[sdp][header]);
}

static void dp_catalog_audio_set_header(struct dp_catalog_audio *audio)
{
	struct dp_catalog_private *catalog;
	u32 (*sdp_map)[DP_AUDIO_SDP_HEADER_MAX];
	void __iomem *base;
	enum dp_catalog_audio_sdp_type sdp;
	enum dp_catalog_audio_header_type header;
	u32 data;

	if (!audio)
		return;

	dp_catalog_get_priv(audio);

	base    = catalog->io->dp_link.base;
	sdp_map = catalog->audio_map;
	sdp     = audio->sdp_type;
	header  = audio->sdp_header;
	data    = audio->data;

	dp_write(base + sdp_map[sdp][header], data);
}

static void dp_catalog_audio_config_acr(struct dp_catalog_audio *audio)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;
	u32 acr_ctrl, select;

	dp_catalog_get_priv(audio);

	select = audio->data;
	base   = catalog->io->dp_link.base;

	acr_ctrl = select << 4 | BIT(31) | BIT(8) | BIT(14);

	pr_debug("select = 0x%x, acr_ctrl = 0x%x\n", select, acr_ctrl);

	dp_write(base + MMSS_DP_AUDIO_ACR_CTRL, acr_ctrl);
}

static void dp_catalog_audio_safe_to_exit_level(struct dp_catalog_audio *audio)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;
	u32 mainlink_levels, safe_to_exit_level;

	dp_catalog_get_priv(audio);

	base   = catalog->io->dp_link.base;
	safe_to_exit_level = audio->data;

	mainlink_levels = dp_read(base + DP_MAINLINK_LEVELS);
	mainlink_levels &= 0xFE0;
	mainlink_levels |= safe_to_exit_level;

	pr_debug("mainlink_level = 0x%x, safe_to_exit_level = 0x%x\n",
			mainlink_levels, safe_to_exit_level);

	dp_write(base + DP_MAINLINK_LEVELS, mainlink_levels);
}

static void dp_catalog_audio_enable(struct dp_catalog_audio *audio)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;
	bool enable;
	u32 audio_ctrl;

	dp_catalog_get_priv(audio);

	base   = catalog->io->dp_link.base;
	enable = !!audio->data;

	audio_ctrl = dp_read(base + MMSS_DP_AUDIO_CFG);

	if (enable)
		audio_ctrl |= BIT(0);
	else
		audio_ctrl &= ~BIT(0);

	pr_debug("dp_audio_cfg = 0x%x\n", audio_ctrl);
	dp_write(base + MMSS_DP_AUDIO_CFG, audio_ctrl);

	/* make sure audio engine is disabled */
	wmb();
}

static void dp_catalog_config_spd_header(struct dp_catalog_panel *panel)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;
	u32 value, new_value;
	u8 parity_byte;

	if (!panel)
		return;

	dp_catalog_get_priv(panel);
	base = catalog->io->dp_link.base;

	/* Config header and parity byte 1 */
	value = dp_read(base + MMSS_DP_GENERIC1_0);

	new_value = 0x83;
	parity_byte = dp_header_get_parity(new_value);
	value |= ((new_value << HEADER_BYTE_1_BIT)
			| (parity_byte << PARITY_BYTE_1_BIT));
	pr_debug("Header Byte 1: value = 0x%x, parity_byte = 0x%x\n",
			value, parity_byte);
	dp_write(base + MMSS_DP_GENERIC1_0, value);

	/* Config header and parity byte 2 */
	value = dp_read(base + MMSS_DP_GENERIC1_1);

	new_value = 0x1b;
	parity_byte = dp_header_get_parity(new_value);
	value |= ((new_value << HEADER_BYTE_2_BIT)
			| (parity_byte << PARITY_BYTE_2_BIT));
	pr_debug("Header Byte 2: value = 0x%x, parity_byte = 0x%x\n",
			value, parity_byte);
	dp_write(base + MMSS_DP_GENERIC1_1, value);

	/* Config header and parity byte 3 */
	value = dp_read(base + MMSS_DP_GENERIC1_1);

	new_value = (0x0 | (0x12 << 2));
	parity_byte = dp_header_get_parity(new_value);
	value |= ((new_value << HEADER_BYTE_3_BIT)
			| (parity_byte << PARITY_BYTE_3_BIT));
	pr_debug("Header Byte 3: value = 0x%x, parity_byte = 0x%x\n",
			new_value, parity_byte);
	dp_write(base + MMSS_DP_GENERIC1_1, value);
}

static void dp_catalog_panel_config_spd(struct dp_catalog_panel *panel)
{
	struct dp_catalog_private *catalog;
	void __iomem *base;
	u32 spd_cfg = 0, spd_cfg2 = 0;
	u8 *vendor = NULL, *product = NULL;
	/*
	 * Source Device Information
	 * 00h unknown
	 * 01h Digital STB
	 * 02h DVD
	 * 03h D-VHS
	 * 04h HDD Video
	 * 05h DVC
	 * 06h DSC
	 * 07h Video CD
	 * 08h Game
	 * 09h PC general
	 * 0ah Bluray-Disc
	 * 0bh Super Audio CD
	 * 0ch HD DVD
	 * 0dh PMP
	 * 0eh-ffh reserved
	 */
	u32 device_type = 0;

	if (!panel)
		return;

	dp_catalog_get_priv(panel);
	base = catalog->io->dp_link.base;

	dp_catalog_config_spd_header(panel);

	vendor = panel->spd_vendor_name;
	product = panel->spd_product_description;

	dp_write(base + MMSS_DP_GENERIC1_2, ((vendor[0] & 0x7f) |
				((vendor[1] & 0x7f) << 8) |
				((vendor[2] & 0x7f) << 16) |
				((vendor[3] & 0x7f) << 24)));
	dp_write(base + MMSS_DP_GENERIC1_3, ((vendor[4] & 0x7f) |
				((vendor[5] & 0x7f) << 8) |
				((vendor[6] & 0x7f) << 16) |
				((vendor[7] & 0x7f) << 24)));
	dp_write(base + MMSS_DP_GENERIC1_4, ((product[0] & 0x7f) |
			((product[1] & 0x7f) << 8) |
			((product[2] & 0x7f) << 16) |
			((product[3] & 0x7f) << 24)));
	dp_write(base + MMSS_DP_GENERIC1_5, ((product[4] & 0x7f) |
			((product[5] & 0x7f) << 8) |
			((product[6] & 0x7f) << 16) |
			((product[7] & 0x7f) << 24)));
	dp_write(base + MMSS_DP_GENERIC1_6, ((product[8] & 0x7f) |
			((product[9] & 0x7f) << 8) |
			((product[10] & 0x7f) << 16) |
			((product[11] & 0x7f) << 24)));
	dp_write(base + MMSS_DP_GENERIC1_7, ((product[12] & 0x7f) |
			((product[13] & 0x7f) << 8) |
			((product[14] & 0x7f) << 16) |
			((product[15] & 0x7f) << 24)));
	dp_write(base + MMSS_DP_GENERIC1_8, device_type);
	dp_write(base + MMSS_DP_GENERIC1_9, 0x00);

	spd_cfg = dp_read(base + MMSS_DP_SDP_CFG);
	/* GENERIC1_SDP for SPD Infoframe */
	spd_cfg |= BIT(18);
	dp_write(base + MMSS_DP_SDP_CFG, spd_cfg);

	spd_cfg2 = dp_read(base + MMSS_DP_SDP_CFG2);
	/* 28 data bytes for SPD Infoframe with GENERIC1 set */
	spd_cfg2 |= BIT(17);
	dp_write(base + MMSS_DP_SDP_CFG2, spd_cfg2);

	dp_write(base + MMSS_DP_SDP_CFG3, 0x1);
	dp_write(base + MMSS_DP_SDP_CFG3, 0x0);
}

struct dp_catalog *dp_catalog_get(struct device *dev, struct dp_io *io)
{
	int rc = 0;
	struct dp_catalog *dp_catalog;
	struct dp_catalog_private *catalog;
	struct dp_catalog_aux aux = {
		.read_data     = dp_catalog_aux_read_data,
		.write_data    = dp_catalog_aux_write_data,
		.write_trans   = dp_catalog_aux_write_trans,
		.clear_trans   = dp_catalog_aux_clear_trans,
		.reset         = dp_catalog_aux_reset,
		.update_aux_cfg = dp_catalog_aux_update_cfg,
		.enable        = dp_catalog_aux_enable,
		.setup         = dp_catalog_aux_setup,
		.get_irq       = dp_catalog_aux_get_irq,
		.clear_hw_interrupts = dp_catalog_aux_clear_hw_interrupts,
	};
	struct dp_catalog_ctrl ctrl = {
		.state_ctrl     = dp_catalog_ctrl_state_ctrl,
		.config_ctrl    = dp_catalog_ctrl_config_ctrl,
		.lane_mapping   = dp_catalog_ctrl_lane_mapping,
		.mainlink_ctrl  = dp_catalog_ctrl_mainlink_ctrl,
		.config_misc    = dp_catalog_ctrl_config_misc,
		.config_msa     = dp_catalog_ctrl_config_msa,
		.set_pattern    = dp_catalog_ctrl_set_pattern,
		.reset          = dp_catalog_ctrl_reset,
		.usb_reset      = dp_catalog_ctrl_usb_reset,
		.mainlink_ready = dp_catalog_ctrl_mainlink_ready,
		.enable_irq     = dp_catalog_ctrl_enable_irq,
		.hpd_config     = dp_catalog_ctrl_hpd_config,
		.phy_reset      = dp_catalog_ctrl_phy_reset,
		.phy_lane_cfg   = dp_catalog_ctrl_phy_lane_cfg,
		.update_vx_px   = dp_catalog_ctrl_update_vx_px,
		.get_interrupt  = dp_catalog_ctrl_get_interrupt,
		.update_transfer_unit = dp_catalog_ctrl_update_transfer_unit,
		.read_hdcp_status     = dp_catalog_ctrl_read_hdcp_status,
		.send_phy_pattern    = dp_catalog_ctrl_send_phy_pattern,
		.read_phy_pattern = dp_catalog_ctrl_read_phy_pattern,
	};
	struct dp_catalog_audio audio = {
		.init       = dp_catalog_audio_init,
		.config_acr = dp_catalog_audio_config_acr,
		.enable     = dp_catalog_audio_enable,
		.config_sdp = dp_catalog_audio_config_sdp,
		.set_header = dp_catalog_audio_set_header,
		.get_header = dp_catalog_audio_get_header,
		.safe_to_exit_level = dp_catalog_audio_safe_to_exit_level,
	};
	struct dp_catalog_panel panel = {
		.timing_cfg = dp_catalog_panel_timing_cfg,
		.config_hdr = dp_catalog_panel_config_hdr,
		.tpg_config = dp_catalog_panel_tpg_cfg,
		.config_spd = dp_catalog_panel_config_spd,
	};

	if (!io) {
		pr_err("invalid input\n");
		rc = -EINVAL;
		goto error;
	}

	catalog  = devm_kzalloc(dev, sizeof(*catalog), GFP_KERNEL);
	if (!catalog) {
		rc = -ENOMEM;
		goto error;
	}

	catalog->dev = dev;
	catalog->io = io;

	dp_catalog = &catalog->dp_catalog;

	dp_catalog->aux   = aux;
	dp_catalog->ctrl  = ctrl;
	dp_catalog->audio = audio;
	dp_catalog->panel = panel;

	return dp_catalog;
error:
	return ERR_PTR(rc);
}

void dp_catalog_put(struct dp_catalog *dp_catalog)
{
	struct dp_catalog_private *catalog;

	if (!dp_catalog)
		return;

	catalog = container_of(dp_catalog, struct dp_catalog_private,
				dp_catalog);

	devm_kfree(catalog->dev, catalog);
}
