// SPDX-License-Identifier: GPL-2.0

#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/utsname.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

#define BFLB_INT_TICKS_PER_SEC 1024
#define BFLB_TICK_DIV 256
#define BFLB_TICKS_PER_SEC ( BFLB_INT_TICKS_PER_SEC / BFLB_TICK_DIV )
#define BFLB_MAX_SECS ( 65535 / BFLB_TICKS_PER_SEC )

#define BFLB_DEFAULT_TIMEOUT 60

#define BFLB_REG_BASE 0x2000A500

#define BFLB_REG_WFAR 0x9C
#define BFLB_VAL_WFAR 0xBABA

#define BFLB_REG_WSAR 0xA0
#define BFLB_VAL_WSAR 0xEB10

#define BFLB_REG_WVR 0x6C
#define BFLB_REG_WCR 0x98
#define BFLB_VAL_WCR 0x1

#define BFLB_REG_WMER 0x64
#define BFLB_VAL_WE 0x1
#define BFLB_VAL_WRIE 0x2

#define BFLB_REG_TCCR 0x00
#define BFLB_MASK_CS_WDT 0xF00
#define BFLB_VAL_CS_1K 0x200

#define BFLB_REG_TCDR 0xbc
#define BFLB_MASK_TCDR 0xFF000000
#define BFLB_VAL_TCDR 0xFF000000

#define BFLB_REG_WMR 0x68
#define BFLB_MASK_WMR 0xffff
#define BFLB_VAL_WMR 240


struct bflb_watchdog_device {
	struct watchdog_device wdd;
	struct device *dev;
	void __iomem *regs;
};

static inline
struct bflb_watchdog_device *to_bflb_wdd(struct watchdog_device *wdd)
{
	return container_of(wdd, struct bflb_watchdog_device, wdd);
}

// Access key registers must be written before write
// operations presumably to prevent accidentally enabling
// the watchdog and killing the machine.
static inline int bflb_unlock_watchdog(struct bflb_watchdog_device *bflb_wdd)
{
	writew(BFLB_VAL_WFAR, bflb_wdd->regs + BFLB_REG_WFAR);
	writew(BFLB_VAL_WSAR, bflb_wdd->regs + BFLB_REG_WSAR);

	return 0;
}

static int bflb_wdt_ping(struct watchdog_device *wdd)
{
	uint32_t reg_val;
	struct bflb_watchdog_device *bflb_wdd = to_bflb_wdd(wdd);

	dev_dbg(wdd->parent, "bflb_wdt_ping");
	bflb_unlock_watchdog(bflb_wdd);
	reg_val = readl(bflb_wdd->regs + BFLB_REG_WCR);
	reg_val |= BFLB_VAL_WCR;
	writel(reg_val, bflb_wdd->regs + BFLB_REG_WCR);

	return 0;
};

static inline void bflb_wdt_update_timeout_reg(struct watchdog_device *wdd)
{
	unsigned int timeout_ticks;
	struct bflb_watchdog_device *bflb_wdd = to_bflb_wdd(wdd);

	bflb_unlock_watchdog(bflb_wdd);
	timeout_ticks = wdd->timeout * BFLB_TICKS_PER_SEC;
	writew(timeout_ticks, bflb_wdd->regs + BFLB_REG_WMR);
}

static int bflb_wdt_set_timeout(struct watchdog_device *wdd,
				unsigned int timeout)
{
	if (timeout >= wdd->max_timeout) {
		dev_warn(wdd->parent,
			"timeout %i > max_timeout %i, using max_timeout...",
			timeout, wdd->max_timeout);
		timeout = wdd->max_timeout;
	}

	wdd->timeout = timeout;

	bflb_wdt_update_timeout_reg(wdd);

	dev_dbg(wdd->parent, "bflb_wdt_set_timeout (s=%i tps=%i)",
		timeout, BFLB_TICKS_PER_SEC);

	return 0;
}

static int bflb_wdt_start(struct watchdog_device *wdd)
{
	uint32_t reg_val;
	struct bflb_watchdog_device *bflb_wdd = to_bflb_wdd(wdd);


	// And enable the watchdog
	bflb_unlock_watchdog(bflb_wdd);
	reg_val = readl(bflb_wdd->regs + BFLB_REG_WMER);
	reg_val |= BFLB_VAL_WE;
	writel(reg_val, bflb_wdd->regs + BFLB_REG_WMER);

	dev_info(wdd->parent, "bflb_wdt_start started...");

	return 0;
}

static int bflb_wdt_stop(struct watchdog_device *wdd)
{
	uint32_t reg_val;
	struct bflb_watchdog_device *bflb_wdd = to_bflb_wdd(wdd);

	// disable
	bflb_unlock_watchdog(bflb_wdd);
	reg_val = readl(bflb_wdd->regs + BFLB_REG_WMER);
	reg_val &= ~BFLB_VAL_WE;
	writel(reg_val, bflb_wdd->regs + BFLB_REG_WMER);

	dev_info(wdd->parent, "bflb_wdt_stopped...");

	return 0;
};


static unsigned int bflb_wdt_timeleft(struct watchdog_device *wdd)
{
	unsigned int used_seconds;
	unsigned int remaining_seconds;
	unsigned int ticks;
	struct bflb_watchdog_device *bflb_wdd = to_bflb_wdd(wdd);

	ticks = readw(bflb_wdd->regs + BFLB_REG_WVR);

	used_seconds = ticks / BFLB_TICKS_PER_SEC;
	remaining_seconds = wdd->max_timeout - used_seconds;
	dev_dbg(wdd->parent, "bflb_wdt_time left %i (elapsed tick %i, sec %i)",
		remaining_seconds, ticks, used_seconds);

	return remaining_seconds;
};

static const struct watchdog_info bflb_wdt_info = {
	.identity = "bflb_wdt",
	.options = WDIOF_SETTIMEOUT |
	WDIOF_KEEPALIVEPING |
	WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops bflb_wdt_ops = {
	.start = bflb_wdt_start,
	.stop = bflb_wdt_stop,
	.ping = bflb_wdt_ping,
	.set_timeout = bflb_wdt_set_timeout,
	.get_timeleft = bflb_wdt_timeleft,
};


static int __init bflb_wdt_probe(struct platform_device *pdev)
{
	struct bflb_watchdog_device *bflb_wdd;
	struct watchdog_device *wdd;
	struct resource *res;
	int err;
	uint32_t reg_val;

	dev_dbg(&pdev->dev, "bflb_wdt_probe started");

	bflb_wdd = devm_kzalloc(&pdev->dev, sizeof(*bflb_wdd), GFP_KERNEL);
	if (!bflb_wdd) return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	bflb_wdd->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(bflb_wdd->regs))
		return PTR_ERR(bflb_wdd->regs);

	wdd = &bflb_wdd->wdd;

	wdd->info = &bflb_wdt_info;
	wdd->ops = &bflb_wdt_ops;

	wdd->timeout = BFLB_DEFAULT_TIMEOUT;
	wdd->max_timeout = BFLB_MAX_SECS;
	wdd->min_timeout = 1;
	wdd->parent = &pdev->dev;

	watchdog_stop_on_reboot(wdd);
	watchdog_stop_on_unregister(wdd);
	watchdog_set_nowayout(wdd, WATCHDOG_NOWAYOUT);
	watchdog_init_timeout(wdd, BFLB_DEFAULT_TIMEOUT, &pdev->dev);

	// Setup registers

	// Set to reboot on watchdog, disable until we start
	bflb_unlock_watchdog(bflb_wdd);
	reg_val = readl(bflb_wdd->regs + BFLB_REG_WMER);
	reg_val &= ~BFLB_VAL_WE;
	reg_val |= BFLB_VAL_WRIE;
	writel(reg_val, bflb_wdd->regs + BFLB_REG_WMER);

	// Set to 1K per second clock
	reg_val = readl(bflb_wdd->regs + BFLB_REG_TCCR);
	reg_val &= ~BFLB_MASK_CS_WDT;
	reg_val |= BFLB_VAL_CS_1K;
	writel(reg_val, bflb_wdd->regs + BFLB_REG_TCCR);

	// Max out divider to 255, meaning actual
	// ticks will happen 4 times a second, or
	// every 250 ms.
	reg_val = readl(bflb_wdd->regs + BFLB_REG_TCDR);
	reg_val &= ~BFLB_MASK_TCDR;
	reg_val |= BFLB_VAL_TCDR;
	writel(reg_val, bflb_wdd->regs + BFLB_REG_TCDR);

	// Set last valid timeout value
	bflb_wdt_update_timeout_reg(wdd);

	err = devm_watchdog_register_device(&pdev->dev, wdd);
	if (err) return err;

	platform_set_drvdata(pdev, bflb_wdd);

	dev_info(&pdev->dev, "bflb_wdt_probe completed...");

	return 0;
}

static int __exit bflb_wdt_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "bflb_wdt_remove removed...");

	return 0;
}

static const struct of_device_id bflb_wdt_match[] = {
	{
		.compatible = "bflb,bflb808-wdt",
	},
	{},
};

MODULE_DEVICE_TABLE(of, bflb_wdt_match);

static struct platform_driver bflb_wdt_driver = {
	.probe = bflb_wdt_probe,
	.remove = bflb_wdt_remove,
	.driver = {
		.name = "bflb_wdt",
		.owner = THIS_MODULE,
		.of_match_table = bflb_wdt_match,
  },
};


static int __init bflb_wdt_init(void)
{
	int res;

	res = platform_driver_register(&bflb_wdt_driver);
	if(res)
		pr_alert("bflb_wdt_init FAILED (%i)\n", res);
	else
		pr_alert("bflb_wdt_init: %s\n", "success");

	return res;
}

arch_initcall(bflb_wdt_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BL808 Watchdog support");
MODULE_AUTHOR("Grant Olson <kgo@grant-olson.net");

