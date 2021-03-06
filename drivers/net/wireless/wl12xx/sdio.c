/*
 * This file is part of wl1271
 *
 * Copyright (C) 2009-2010 Nokia Corporation
 *
 * Contact: Luciano Coelho <luciano.coelho@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <linux/irq.h>
#include <linux/module.h>
#include <linux/crc7.h>
#include <linux/vmalloc.h>
#include <linux/platform_device.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/gpio.h>
#include <linux/wl12xx.h>
#include <linux/pm_runtime.h>

#include "wl12xx.h"
#include "wl12xx_80211.h"
#include "io.h"

#ifndef SDIO_VENDOR_ID_TI
#define SDIO_VENDOR_ID_TI		0x0097
#endif

#ifndef SDIO_DEVICE_ID_TI_WL1271
#define SDIO_DEVICE_ID_TI_WL1271	0x4076
#endif

struct wl12xx_sdio_glue {
	struct device *dev;
	struct platform_device *core;
};

static const struct sdio_device_id wl1271_devices[] = {
	{ SDIO_DEVICE(SDIO_VENDOR_ID_TI, SDIO_DEVICE_ID_TI_WL1271) },
	{}
};
MODULE_DEVICE_TABLE(sdio, wl1271_devices);

static void wl1271_sdio_set_block_size(struct device *child,
				       unsigned int blksz)
{
	struct wl12xx_sdio_glue *glue = dev_get_drvdata(child->parent);
	struct sdio_func *func = dev_to_sdio_func(glue->dev);

	sdio_claim_host(func);
	sdio_set_block_size(func, blksz);
	sdio_release_host(func);
}

static void wl12xx_sdio_raw_read(struct device *child, int addr, void *buf,
				 size_t len, bool fixed)
{
	int ret;
	struct wl12xx_sdio_glue *glue = dev_get_drvdata(child->parent);
	struct sdio_func *func = dev_to_sdio_func(glue->dev);

	if (unlikely(addr == HW_ACCESS_ELP_CTRL_REG_ADDR)) {
		((u8 *)buf)[0] = sdio_f0_readb(func, addr, &ret);
		wl1271_debug(DEBUG_SDIO, "sdio read 52 addr 0x%x, byte 0x%02x",
			     addr, ((u8 *)buf)[0]);
	} else {
		if (fixed)
			ret = sdio_readsb(func, buf, addr, len);
		else
			ret = sdio_memcpy_fromio(func, buf, addr, len);

		wl1271_debug(DEBUG_SDIO, "sdio read 53 addr 0x%x, %zu bytes",
			     addr, len);
		wl1271_dump_ascii(DEBUG_SDIO, "data: ", buf, len);
	}

	if (ret)
		wl1271_error("sdio read failed (%d)", ret);
}

static void wl12xx_sdio_raw_write(struct device *child, int addr, void *buf,
				  size_t len, bool fixed)
{
	int ret;
	struct wl12xx_sdio_glue *glue = dev_get_drvdata(child->parent);
	struct sdio_func *func = dev_to_sdio_func(glue->dev);

	if (unlikely(addr == HW_ACCESS_ELP_CTRL_REG_ADDR)) {
		sdio_f0_writeb(func, ((u8 *)buf)[0], addr, &ret);
		wl1271_debug(DEBUG_SDIO, "sdio write 52 addr 0x%x, byte 0x%02x",
			     addr, ((u8 *)buf)[0]);
	} else {
		wl1271_debug(DEBUG_SDIO, "sdio write 53 addr 0x%x, %zu bytes",
			     addr, len);
		wl1271_dump_ascii(DEBUG_SDIO, "data: ", buf, len);

		if (fixed)
			ret = sdio_writesb(func, addr, buf, len);
		else
			ret = sdio_memcpy_toio(func, addr, buf, len);
	}

	if (ret)
		wl1271_error("sdio write failed (%d)", ret);
}

static int wl12xx_sdio_power_on(struct wl12xx_sdio_glue *glue)
{
	int ret;
	struct sdio_func *func = dev_to_sdio_func(glue->dev);

	/* Make sure the card will not be powered off by runtime PM */
	ret = pm_runtime_get_sync(&func->dev);
	if (ret < 0)
		goto out;

	/* Runtime PM might be disabled, so power up the card manually */
	ret = mmc_power_restore_host(func->card->host);
	if (ret < 0)
		goto out;

	sdio_claim_host(func);
	sdio_enable_func(func);

out:
	return ret;
}

static int wl12xx_sdio_power_off(struct wl12xx_sdio_glue *glue)
{
	int ret;
	struct sdio_func *func = dev_to_sdio_func(glue->dev);

	sdio_disable_func(func);
	sdio_release_host(func);

	/* Runtime PM might be disabled, so power off the card manually */
	ret = mmc_power_save_host(func->card->host);
	if (ret < 0)
		return ret;

	/* Let runtime PM know the card is powered off */
	return pm_runtime_put_sync(&func->dev);
}

static int wl12xx_sdio_set_power(struct device *child, bool enable)
{
	struct wl12xx_sdio_glue *glue = dev_get_drvdata(child->parent);

	if (enable)
		return wl12xx_sdio_power_on(glue);
	else
		return wl12xx_sdio_power_off(glue);
}

static struct wl1271_if_operations sdio_ops = {
	.read		= wl12xx_sdio_raw_read,
	.write		= wl12xx_sdio_raw_write,
	.power		= wl12xx_sdio_set_power,
	.set_block_size = wl1271_sdio_set_block_size,
};

static int __devinit wl1271_probe(struct sdio_func *func,
				  const struct sdio_device_id *id)
{
	struct wl12xx_platform_data *wlan_data;
	struct wl12xx_sdio_glue *glue;
	struct resource res[1];
	mmc_pm_flag_t mmcflags;
	int ret = -ENOMEM;

	/* We are only able to handle the wlan function */
	if (func->num != 0x02)
		return -ENODEV;

	glue = kzalloc(sizeof(*glue), GFP_KERNEL);
	if (!glue) {
		wl1271_error("can't allocate glue");
		goto out;
	}

	glue->dev = &func->dev;

	/* Grab access to FN0 for ELP reg. */
	func->card->quirks |= MMC_QUIRK_LENIENT_FN0;

	/* Use block mode for transferring over one block size of data */
	func->card->quirks |= MMC_QUIRK_BLKSZ_FOR_BYTE_MODE;

	wlan_data = wl12xx_get_platform_data();
	if (IS_ERR(wlan_data)) {
		ret = PTR_ERR(wlan_data);
		wl1271_error("missing wlan platform data: %d", ret);
		goto out_free_glue;
	}

	/* if sdio can keep power while host is suspended, enable wow */
	mmcflags = sdio_get_host_pm_caps(func);
	wl1271_debug(DEBUG_SDIO, "sdio PM caps = 0x%x", mmcflags);

	if (mmcflags & MMC_PM_KEEP_POWER)
		wlan_data->pwr_in_suspend = true;

	wlan_data->ops = &sdio_ops;

	sdio_set_drvdata(func, glue);

	/* Tell PM core that we don't need the card to be powered now */
	pm_runtime_put_noidle(&func->dev);

	glue->core = platform_device_alloc("wl12xx-sdio", -1);
	if (!glue->core) {
		wl1271_error("can't allocate platform_device");
		ret = -ENOMEM;
		goto out_free_glue;
	}

	glue->core->dev.parent = &func->dev;

	memset(res, 0x00, sizeof(res));

	res[0].start = wlan_data->irq;
	res[0].flags = IORESOURCE_IRQ;
	res[0].name = "irq";

	ret = platform_device_add_resources(glue->core, res, ARRAY_SIZE(res));
	if (ret) {
		wl1271_error("can't add resources");
		goto out_dev_put;
	}

	ret = platform_device_add_data(glue->core, wlan_data,
				       sizeof(*wlan_data));
	if (ret) {
		wl1271_error("can't add platform data");
		goto out_dev_put;
	}

	ret = platform_device_add(glue->core);
	if (ret) {
		wl1271_error("can't add platform device");
		goto out_dev_put;
	}

	wl1271_notice("initialized");

	return 0;

out_dev_put:
	platform_device_put(glue->core);

out_free_glue:
	kfree(glue);

out:
	return ret;
}

static void __devexit wl1271_remove(struct sdio_func *func)
{
	struct wl12xx_sdio_glue *glue = sdio_get_drvdata(func);

	/* Undo decrement done above in wl1271_probe */
	pm_runtime_get_noresume(&func->dev);

	platform_device_del(glue->core);
	platform_device_put(glue->core);
	kfree(glue);
}

#ifdef CONFIG_PM
static int wl1271_suspend(struct device *dev)
{
	/* Tell MMC/SDIO core it's OK to power down the card
	 * (if it isn't already), but not to remove it completely */
	struct sdio_func *func = dev_to_sdio_func(dev);
	struct wl1271 *wl = sdio_get_drvdata(func);
	mmc_pm_flag_t sdio_flags;
	int ret = 0;

	wl1271_debug(DEBUG_MAC80211, "wl1271 suspend. wow_enabled: %d",
		     wl->wow_enabled);

	/* check whether sdio should keep power */
	if (wl->wow_enabled) {
		sdio_flags = sdio_get_host_pm_caps(func);

		if (!(sdio_flags & MMC_PM_KEEP_POWER)) {
			wl1271_error("can't keep power while host "
				     "is suspended");
			ret = -EINVAL;
			goto out;
		}

		/* keep power while host suspended */
		ret = sdio_set_host_pm_flags(func, MMC_PM_KEEP_POWER);
		if (ret) {
			wl1271_error("error while trying to keep power");
			goto out;
		}

		/* release host */
		sdio_release_host(func);
	}
out:
	return ret;
}

static int wl1271_resume(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);
	struct wl1271 *wl = sdio_get_drvdata(func);

	wl1271_debug(DEBUG_MAC80211, "wl1271 resume");
	if (wl->wow_enabled) {
		/* claim back host */
		sdio_claim_host(func);
	}

	return 0;
}

static const struct dev_pm_ops wl1271_sdio_pm_ops = {
	.suspend	= wl1271_suspend,
	.resume		= wl1271_resume,
};
#endif

static struct sdio_driver wl1271_sdio_driver = {
	.name		= "wl1271_sdio",
	.id_table	= wl1271_devices,
	.probe		= wl1271_probe,
	.remove		= __devexit_p(wl1271_remove),
#ifdef CONFIG_PM
	.drv = {
		.pm = &wl1271_sdio_pm_ops,
	},
#endif
};

static int __init wl1271_init(void)
{
	int ret;

	ret = sdio_register_driver(&wl1271_sdio_driver);
	if (ret < 0) {
		wl1271_error("failed to register sdio driver: %d", ret);
		goto out;
	}

out:
	return ret;
}

static void __exit wl1271_exit(void)
{
	sdio_unregister_driver(&wl1271_sdio_driver);

	wl1271_notice("unloaded");
}

module_init(wl1271_init);
module_exit(wl1271_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Luciano Coelho <coelho@ti.com>");
MODULE_AUTHOR("Juuso Oikarinen <juuso.oikarinen@nokia.com>");
MODULE_FIRMWARE(WL1271_FW_NAME);
MODULE_FIRMWARE(WL128X_FW_NAME);
MODULE_FIRMWARE(WL127X_AP_FW_NAME);
MODULE_FIRMWARE(WL128X_AP_FW_NAME);
