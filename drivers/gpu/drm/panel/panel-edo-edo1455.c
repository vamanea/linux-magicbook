// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021 Google Inc.
 * Copyright 2026 Valentin Manea
 *
 * This driver supports the EDO 14.55 eDP panel. This panel can't be handled
 * by the DRM_PANEL_SIMPLE driver because the only system where it's present
 * enables DSC in BIOS but the drm driver doesn't support DSC.
 * Based on Samsung ATNA33XC20 driver
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

/* Standard panel-edp timing delays (in milliseconds) */
#define EDO1455_POWER_ON_DELAY_MS	200	/* T1+T2: Power on to enable */
#define EDO1455_ENABLE_DELAY_MS		20	/* T3: Enable to ready */
#define EDO1455_DISABLE_DELAY_MS	50	/* T4: Disable to power off */
#define EDO1455_POWER_OFF_DELAY_MS	100	/* T12: Power off to power on */

/* T3 VCC to HPD high is max 200 ms */
#define HPD_MAX_MS	200
#define HPD_MAX_US	(HPD_MAX_MS * 1000)

struct edo1455_panel {
	struct drm_panel base;

	struct regulator *supply;
	struct drm_dp_aux *aux;

	const struct drm_edid *drm_edid;

	ktime_t powered_off_time;
	ktime_t powered_on_time;
};

static inline struct edo1455_panel *to_edo1455(struct drm_panel *panel)
{
	return container_of(panel, struct edo1455_panel, base);
}

static int
write_dsc_decompression_flag(struct drm_dp_aux *aux, bool set)
{
	int err;
	u8 val;
	u8 flag = DP_DECOMPRESSION_EN;

	err = drm_dp_dpcd_readb(aux, DP_DSC_ENABLE, &val);
	if (err < 0)
		return err;

	if (set)
		val |= flag;
	else
		val &= ~flag;

	return drm_dp_dpcd_writeb(aux, DP_DSC_ENABLE, val);
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

	drm_dp_dpcd_set_powered(p->aux, false);
	ret = regulator_disable(p->supply);
	if (ret)
		return ret;
	p->powered_off_time = ktime_get_boottime();

	return 0;
}

static int edo1455_resume(struct device *dev)
{
	struct edo1455_panel *p = dev_get_drvdata(dev);
	int ret;

	/* T12 (Power off time) is min 100 ms */
	edo1455_wait(p->powered_off_time, EDO1455_POWER_OFF_DELAY_MS);

	ret = regulator_enable(p->supply);
	if (ret)
		return ret;

	/* T2: Wait for power to stabilize */
	msleep(EDO1455_ENABLE_DELAY_MS);

	drm_dp_dpcd_set_powered(p->aux, true);
	p->powered_on_time = ktime_get_boottime();

	if (p->aux->wait_hpd_asserted) {
		ret = p->aux->wait_hpd_asserted(p->aux, HPD_MAX_US);

		if (ret) {
			dev_warn(dev, "Controller error waiting for HPD: %d\n", ret);
			goto error;
		}
	}

	return 0;

error:
	drm_dp_dpcd_set_powered(p->aux, false);
	regulator_disable(p->supply);

	return ret;
}

static int edo1455_disable(struct drm_panel *panel)
{
	msleep(EDO1455_DISABLE_DELAY_MS);

	return 0;
}

static int edo1455_enable(struct drm_panel *panel)
{
	struct edo1455_panel *p = to_edo1455(panel);

	edo1455_wait(p->powered_on_time, EDO1455_POWER_ON_DELAY_MS);

	return 0;
}

static int edo1455_unprepare(struct drm_panel *panel)
{
	int ret;

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

	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

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

	write_dsc_decompression_flag(panel->aux, false);

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
	{ .compatible = "edo,edo1455" },
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
		.name		= "edo1455-panel",
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

MODULE_DESCRIPTION("EDO14.55 eDP Panel Driver");
MODULE_AUTHOR("Valentin Manea<valentin.manea@mrs.ro>");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL v2");
