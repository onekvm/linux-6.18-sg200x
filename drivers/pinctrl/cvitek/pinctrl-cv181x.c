// SPDX-License-Identifier: GPL-2.0
/*
 * Cvitek CV181x pinmux MMIO map. Probe does not rewrite pinmux;
 * Cube AUD pinmux stays as U-Boot left it.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/string.h>

/* Last CV181x pinctl register; do not pull in PINMUX_CONFIG (AUD). */
#define PINMUX_RANGE (0xC8C + 4)

struct cvitek_pinctrl {
	struct device *dev;
	void __iomem *regs;
	size_t regs_size;
	u32 *saved_regs;
};

static int cvi_pinctrl_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct cvitek_pinctrl *pinctrl;

	pinctrl = devm_kzalloc(&pdev->dev, sizeof(*pinctrl), GFP_KERNEL);
	if (!pinctrl)
		return -ENOMEM;

	pinctrl->saved_regs = devm_kzalloc(&pdev->dev, PINMUX_RANGE, GFP_KERNEL);
	if (!pinctrl->saved_regs)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "Failed to get pinctrl io resource.\n");
		return -EINVAL;
	}

	pinctrl->regs_size = resource_size(res);
	pinctrl->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pinctrl->regs))
		return PTR_ERR(pinctrl->regs);

	platform_set_drvdata(pdev, pinctrl);
	dev_info(&pdev->dev, "mapped pinmux %pR (no pin rewrite)\n", res);
	return 0;
}

static void cvi_pinctrl_remove(struct platform_device *pdev)
{
}

#ifdef CONFIG_PM_SLEEP
static int cvitek_pinctrl_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct cvitek_pinctrl *pinctrl = platform_get_drvdata(pdev);

	memcpy_fromio(pinctrl->saved_regs, pinctrl->regs, PINMUX_RANGE);
	return 0;
}

static int cvitek_pinctrl_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct cvitek_pinctrl *pinctrl = platform_get_drvdata(pdev);

	memcpy_toio(pinctrl->regs, pinctrl->saved_regs, PINMUX_RANGE);
	return 0;
}
#endif

static const struct of_device_id cvi_pinctrl_of_match[] = {
	{ .compatible = "cvitek,pinctrl-cv181x" },
	{},
};
MODULE_DEVICE_TABLE(of, cvi_pinctrl_of_match);

static const struct dev_pm_ops cvitek_pinctrl_pm_ops = {
	SET_LATE_SYSTEM_SLEEP_PM_OPS(cvitek_pinctrl_suspend,
				     cvitek_pinctrl_resume)
};

static struct platform_driver cvi_pinctrl_driver = {
	.probe = cvi_pinctrl_probe,
	.remove = cvi_pinctrl_remove,
	.driver = {
		.name = "cvitek,pinctrl-cv181x",
		.of_match_table = cvi_pinctrl_of_match,
#ifdef CONFIG_PM_SLEEP
		.pm = &cvitek_pinctrl_pm_ops,
#endif
	},
};

module_platform_driver(cvi_pinctrl_driver);

MODULE_DESCRIPTION("Cvitek CV181x pinctrl MMIO map");
MODULE_LICENSE("GPL v2");
