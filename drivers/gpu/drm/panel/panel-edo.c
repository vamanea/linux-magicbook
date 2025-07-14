// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021 Google Inc.
 *
 * Panel driver for the Samsung ATNA33XC20 panel. This panel can't be handled
 * by the DRM_PANEL_SIMPLE driver because its power sequencing is non-standard.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <drm/display/drm_dp_aux_bus.h>
#include <drm/display/drm_dp_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_panel.h>

/* T3 VCC to HPD high is max 200 ms */
#define HPD_MAX_MS	200
#define HPD_MAX_US	(HPD_MAX_MS * 1000)

struct edo1455_panel {
	struct drm_panel base;
	bool el3_was_on;

	bool no_hpd;
	struct gpio_desc *hpd_gpio;

	struct regulator *supply;
	struct gpio_desc *el_on3_gpio;
	struct drm_dp_aux *aux;

	const struct drm_edid *drm_edid;

	ktime_t powered_off_time;
	ktime_t powered_on_time;
	ktime_t el_on3_off_time;
};

static inline struct edo1455_panel *to_edo1455(struct drm_panel *panel)
{
	return container_of(panel, struct edo1455_panel, base);
}

/*
 * Helper to execute Vendor Specific AUX Write (Opcode 0x6)
 * XML: EDPPowerTimingCustomAuxCmd1
 */
static int auo_panel_aux_cmd_write_vendor(struct drm_dp_aux *aux, u8 addr, u8 data)
{
    struct drm_dp_aux_msg msg;
    int ret;

    memset(&msg, 0, sizeof(msg));
    msg.request = 0x6; /* Fallback to Native Write request */
    /* Note: If 0x6 is a custom request type, you may need to OR it here:
     * msg.request = 0x6 | DP_AUX_NATIVE_WRITE; 
     * But typically vendor commands use standard request types with custom data.
     */
    msg.address = addr;
    msg.buffer = &data;
    msg.size = 1;

    /* Locking is required when calling transfer() directly */
    ret = aux->transfer(aux, &msg);

    return ret < 0 ? ret : 0;
}

/*
 * Helper to execute Power Sequence Fixes
 * Based on XML Fields: EDPPowerTiming6, EDPPowerTimingCustomAuxCmd1/2/3
 */
static void auo_panel_power_sequence_fixes(struct edo1455_panel *p)
{
    int i, ret;
    struct device *dev = p->base.dev;
    u8 buf;

    if (!p->aux) {
        dev_warn(dev, "No AUX channel available, skipping power fixes\n");
        return;
    }

    /*
     * FIX 1: EDPPowerTiming6 (0x140014)
     * Interpretation: 20ms delay after power rail stabilization to prevent HDR flicker.
     * 0x14 = 20 (decimal)
     */
    dev_dbg(dev, "Applying EDPPowerTiming6 delay (20ms)\n");
    usleep_range(20000, 25000);

    /*
     * FIX 2: EDPPowerTimingCustomAuxCmd1 (HDR Flicker)
     * Sequence: 0x6 0x0 0xA followed by (Delay 0x720, Repeat 0x2) x 10 times
     * 0x720 hex = 1824 decimal (microseconds)
     */
    dev_dbg(dev, "Applying CustomAuxCmd1 (HDR Flicker Fix)\n");
    for (i = 0; i < 10; i++) {
        ret = auo_panel_aux_cmd_write_vendor(p->aux, 0x0, 0xA);
        if (ret < 0)
            dev_err(dev, "Cmd1 Write failed iteration %d\n", i);

        /* Delay 0x720 us = 1824 us */
        usleep_range(1824, 2000);
    }

    /*
     * FIX 3: EDPPowerTimingCustomAuxCmd2 (HDR Flicker)
     * Sequence: 0x8 0x0 0x1 Delay 0x720
     * 0x8 = DP_AUX_NATIVE_WRITE
     */
    dev_dbg(dev, "Applying CustomAuxCmd2 (HDR Flicker Fix)\n");
    buf = 0x1;
    ret = drm_dp_dpcd_write(p->aux, 0x0, &buf, 1);
    if (ret < 0)
        dev_err(dev, "Cmd2 Write failed\n");

    usleep_range(1824, 2000);

    /*
     * FIX 4: EDPPowerTimingCustomAuxCmd3 (DSC Garbage Fix)
     * Sequence: 0x9 0x0 0xA followed by (Delay 0x720, Repeat 0x2) x 10 times
     * 0x9 = DP_AUX_NATIVE_READ (Polling)
     */
    dev_dbg(dev, "Applying CustomAuxCmd3 (DSC Garbage Fix)\n");
    for (i = 0; i < 10; i++) {
        ret = drm_dp_dpcd_read(p->aux, 0x0, &buf, 1);
        if (ret < 0)
            dev_err(dev, "Cmd3 Read failed iteration %d\n", i);

        /* Optional: Check if buf == 0xA if this is a status poll */

        /* Delay 0x720 us = 1824 us */
        usleep_range(1824, 2000);
    }
}

static int panel_edp_honor_magicbook_fixup(struct edo1455_panel *p)
{
	struct drm_dp_aux *aux = p->aux;
	int ret = 0;
	u8 val;

	/* ACPI Cmd1: Address 0x720, writing 0x02 repeatedly (HDR/DSC Sync) */
	val = 0x02;
	for (int i = 0; i < 10; i++) {
		ret = drm_dp_dpcd_writeb(aux, 0x0720, val);
		if (ret < 0) return ret;
	}

	/* ACPI Cmd2: Address 0x720, writing 0x01 (Finalizing Mode Set) */
	val = 0x01;
	ret = drm_dp_dpcd_writeb(aux, 0x0720, val);
	if (ret < 0) return ret;

	/* ACPI Cmd3: Address 0x720, repeating 0x02 (Post-DSC Stabilization) */
	val = 0x02;
	for (int i = 0; i < 10; i++) {
		//ret = drm_dp_dpcd_writeb(aux, 0x0720, val);
		if (ret < 0) return ret;
	}

	dev_info(p->base.dev, "Honor MagicBook Art 14 OLED AUX fixup applied.\n");
	return 0;
}

static void edo1455_wait(ktime_t start_ktime, unsigned int min_ms)
{
	ktime_t now_ktime, min_ktime;

	min_ktime = ktime_add(start_ktime, ms_to_ktime(min_ms));
	now_ktime = ktime_get_boottime();

	if (ktime_before(now_ktime, min_ktime))
		msleep(ktime_to_ms(ktime_sub(min_ktime, now_ktime)) + 1);
}

static int edo1455_suspend(struct device *dev)
{
	struct edo1455_panel *p = dev_get_drvdata(dev);
	int ret;

	/*
	 * Note 3 (Example of power off sequence in detail) in spec
	 * specifies to wait 150 ms after deasserting EL3_ON before
	 * powering off.
	 */
	if (p->el3_was_on)
		edo1455_wait(p->el_on3_off_time, 150);

	drm_dp_dpcd_set_powered(p->aux, false);
	ret = regulator_disable(p->supply);
	if (ret)
		return ret;
	p->powered_off_time = ktime_get_boottime();
	p->el3_was_on = false;

	return 0;
}

static int edo1455_resume(struct device *dev)
{
	struct edo1455_panel *p = dev_get_drvdata(dev);
	int hpd_asserted;
	int ret;

	/* T12 (Power off time) is min 500 ms */
	edo1455_wait(p->powered_off_time, 500);

	ret = regulator_enable(p->supply);
	if (ret)
		return ret;
	drm_dp_dpcd_set_powered(p->aux, true);
	p->powered_on_time = ktime_get_boottime();

	if (p->no_hpd) {
		msleep(HPD_MAX_MS);
		return 0;
	}

	if (p->hpd_gpio) {
		ret = readx_poll_timeout(gpiod_get_value_cansleep, p->hpd_gpio,
					 hpd_asserted, hpd_asserted,
					 1000, HPD_MAX_US);
		if (hpd_asserted < 0)
			ret = hpd_asserted;

		if (ret) {
			dev_warn(dev, "Error waiting for HPD GPIO: %d\n", ret);
			goto error;
		}
	} else if (p->aux->wait_hpd_asserted) {
		ret = p->aux->wait_hpd_asserted(p->aux, HPD_MAX_US);

		if (ret) {
			dev_warn(dev, "Controller error waiting for HPD: %d\n", ret);
			goto error;
		}
	}

	/*
	 * Note that it's possible that no_hpd is false, hpd_gpio is
	 * NULL, and wait_hpd_asserted is NULL. This is because
	 * wait_hpd_asserted() is optional even if HPD is hooked up to
	 * a dedicated pin on the eDP controller. In this case we just
	 * assume that the controller driver will wait for HPD at the
	 * right times.
	 */
	return 0;

error:
	drm_dp_dpcd_set_powered(p->aux, false);
	regulator_disable(p->supply);

	return ret;
}

static int edo1455_disable(struct drm_panel *panel)
{
	struct edo1455_panel *p = to_edo1455(panel);

	gpiod_set_value_cansleep(p->el_on3_gpio, 0);
	p->el_on3_off_time = ktime_get_boottime();

	/*
	 * Keep track of the fact that EL_ON3 was on but we haven't power
	 * cycled yet. This lets us know that "el_on3_off_time" is recent (we
	 * don't need to worry about ktime wraparounds) and also makes it
	 * obvious if we try to enable again without a power cycle (see the
	 * warning in edo1455_enable()).
	 */
	p->el3_was_on = true;

	/*
	 * Sleeping 20 ms here (after setting the GPIO) avoids a glitch when
	 * powering off.
	 */
	msleep(20);

	return 0;
}

static int edo1455_enable(struct drm_panel *panel)
{
	struct edo1455_panel *p = to_edo1455(panel);

	/*
	 * Once EL_ON3 drops we absolutely need a power cycle before the next
	 * enable or the backlight will never come on again. The code ensures
	 * this because disable() is _always_ followed by unprepare() and
	 * unprepare() forces a suspend with pm_runtime_put_sync_suspend(),
	 * but let's track just to make sure since the requirement is so
	 * non-obvious.
	 */
	if (WARN_ON(p->el3_was_on))
		return -EIO;

	/*
	 * Note 2 (Example of power on sequence in detail) in spec specifies
	 * to wait 400 ms after powering on before asserting EL3_on.
	 */
	edo1455_wait(p->powered_on_time, 400);

	pr_info("apply EDO panel fixup");

	panel_edp_honor_magicbook_fixup(p);
	/* 3. Apply XML Power Timing Fixes */
	/* This must happen after power is stable but before Link Training */
	//auo_panel_power_sequence_fixes(p);



	gpiod_set_value_cansleep(p->el_on3_gpio, 1);

	return 0;
}

static int edo1455_unprepare(struct drm_panel *panel)
{
	int ret;

	/*
	 * Purposely do a put_sync, don't use autosuspend. The panel's tcon
	 * seems to sometimes crash when you stop giving it data and this is
	 * the best way to ensure it will come back.
	 *
	 * NOTE: we still want autosuspend for cases where we only turn on
	 * to get the EDID or otherwise send DP AUX commands to the panel.
	 */
	ret = pm_runtime_put_sync_suspend(panel->dev);
	if (ret < 0)
		return ret;

	return 0;
}

static int edo1455_prepare(struct drm_panel *panel)
{
	int ret;

	ret = pm_runtime_get_sync(panel->dev);
	if (ret < 0) {
		pm_runtime_put_autosuspend(panel->dev);
		return ret;
	}

	return 0;
}

static int edo1455_get_modes(struct drm_panel *panel,
				 struct drm_connector *connector)
{
	struct edo1455_panel *p = to_edo1455(panel);
	struct dp_aux_ep_device *aux_ep = to_dp_aux_ep_dev(panel->dev);
	int num = 0;

	pm_runtime_get_sync(panel->dev);

	if (!p->drm_edid)
		p->drm_edid = drm_edid_read_ddc(connector, &aux_ep->aux->ddc);

	drm_edid_connector_update(connector, p->drm_edid);

	num = drm_edid_connector_add_modes(connector);

	pm_runtime_mark_last_busy(panel->dev);
	pm_runtime_put_autosuspend(panel->dev);

	return num;
}

static const struct drm_panel_funcs edo1455_funcs = {
	.disable = edo1455_disable,
	.enable = edo1455_enable,
	.unprepare = edo1455_unprepare,
	.prepare = edo1455_prepare,
	.get_modes = edo1455_get_modes,
};

static void edo1455_runtime_disable(void *data)
{
	pm_runtime_disable(data);
}

static void edo1455_dont_use_autosuspend(void *data)
{
	pm_runtime_dont_use_autosuspend(data);
}

static int edo1455_probe(struct dp_aux_ep_device *aux_ep)
{
	struct edo1455_panel *panel;
	struct device *dev = &aux_ep->dev;
	int ret;

	panel = devm_drm_panel_alloc(dev, struct edo1455_panel, base,
				     &edo1455_funcs,
				     DRM_MODE_CONNECTOR_eDP);
	if (IS_ERR(panel))
		return PTR_ERR(panel);

	dev_set_drvdata(dev, panel);

	panel->aux = aux_ep->aux;

	panel->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(panel->supply))
		return dev_err_probe(dev, PTR_ERR(panel->supply),
				     "Failed to get power supply\n");

	panel->el_on3_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(panel->el_on3_gpio))
		return dev_err_probe(dev, PTR_ERR(panel->el_on3_gpio),
				     "Failed to get enable GPIO\n");

	panel->no_hpd = of_property_read_bool(dev->of_node, "no-hpd");
	if (!panel->no_hpd) {
		panel->hpd_gpio = devm_gpiod_get_optional(dev, "hpd", GPIOD_IN);
		if (IS_ERR(panel->hpd_gpio))
			return dev_err_probe(dev, PTR_ERR(panel->hpd_gpio),
					     "Failed to get HPD GPIO\n");
	}

	pm_runtime_enable(dev);
	ret = devm_add_action_or_reset(dev,  edo1455_runtime_disable, dev);
	if (ret)
		return ret;
	pm_runtime_set_autosuspend_delay(dev, 2000);
	pm_runtime_use_autosuspend(dev);
	ret = devm_add_action_or_reset(dev,  edo1455_dont_use_autosuspend, dev);
	if (ret)
		return ret;

	pm_runtime_get_sync(dev);
	ret = drm_panel_dp_aux_backlight(&panel->base, aux_ep->aux);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	/*
	 * Warn if we get an error, but don't consider it fatal. Having
	 * a panel where we can't control the backlight is better than
	 * no panel.
	 */
	if (ret)
		dev_warn(dev, "failed to register dp aux backlight: %d\n", ret);

	drm_panel_add(&panel->base);

	return 0;
}

static void edo1455_remove(struct dp_aux_ep_device *aux_ep)
{
	struct device *dev = &aux_ep->dev;
	struct edo1455_panel *panel = dev_get_drvdata(dev);

	drm_panel_remove(&panel->base);

	drm_edid_free(panel->drm_edid);
}

static const struct of_device_id edo1455_dt_match[] = {
	{ .compatible = "edo,edo1455", },
	{ /* sentinal */ }
};
MODULE_DEVICE_TABLE(of, edo1455_dt_match);

static const struct dev_pm_ops edo1455_pm_ops = {
	SET_RUNTIME_PM_OPS(edo1455_suspend, edo1455_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

static struct dp_aux_ep_driver edo1455_driver = {
	.driver = {
		.name		= "edo_edo1455",
		.of_match_table = edo1455_dt_match,
		.pm		= &edo1455_pm_ops,
	},
	.probe = edo1455_probe,
	.remove = edo1455_remove,
};

static int __init edo1455_init(void)
{
	return dp_aux_dp_driver_register(&edo1455_driver);
}
module_init(edo1455_init);

static void __exit edo1455_exit(void)
{
	dp_aux_dp_driver_unregister(&edo1455_driver);
}
module_exit(edo1455_exit);

MODULE_DESCRIPTION("Samsung edo1455 Panel Driver");
MODULE_LICENSE("GPL v2");
