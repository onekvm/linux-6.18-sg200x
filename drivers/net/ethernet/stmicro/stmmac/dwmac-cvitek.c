// SPDX-License-Identifier: GPL-2.0
/* dwmac-cvitek.c - CVITEK DWMAC glue for SG2002 / CV181x */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/stmmac.h>

#include "stmmac_platform.h"

struct cvitek_mac {
	struct device *dev;
	struct clk *gate_clk_500m;
	struct clk *gate_clk_axi4;
	bool clk_500m_devm;
	bool clk_axi4_devm;
};

static void bm_eth_reset_phy(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	int phy_reset_gpio;
	u32 ephy_addr = 0;
	void __iomem *ephy_reg;

	if (!np)
		return;

	of_property_read_u32(np, "ephy_ctl_reg", &ephy_addr);
	if (ephy_addr) {
		ephy_reg = ioremap(ephy_addr, 0x10);
		if (ephy_reg) {
			writel(readl(ephy_reg) & 0xFFFFFFFC, ephy_reg);
			mdelay(2);
			iounmap(ephy_reg);
		}
	}

	phy_reset_gpio = of_get_named_gpio(np, "phy-reset-gpios", 0);
	if (!gpio_is_valid(phy_reset_gpio))
		return;

	if (gpio_request(phy_reset_gpio, "eth-phy-reset"))
		return;

	gpio_direction_output(phy_reset_gpio, 0);
	mdelay(20);
	gpio_direction_output(phy_reset_gpio, 1);
	mdelay(60);
}

static struct clk *cvitek_optional_clk(struct device *dev, const char *dt_name,
				       const char *sys_name, bool *devm)
{
	struct clk *clk;

	clk = devm_clk_get_optional(dev, dt_name);
	if (!IS_ERR_OR_NULL(clk)) {
		*devm = true;
		return clk;
	}

	clk = clk_get_sys(NULL, sys_name);
	if (IS_ERR(clk))
		return NULL;

	*devm = false;
	return clk;
}

static int bm_dwmac_init(struct platform_device *pdev, void *priv)
{
	struct cvitek_mac *bsp_priv = priv;
	int ret;

	if (!bsp_priv)
		return 0;

	if (bsp_priv->gate_clk_500m) {
		ret = clk_prepare_enable(bsp_priv->gate_clk_500m);
		if (ret)
			return ret;
	}

	if (bsp_priv->gate_clk_axi4) {
		ret = clk_prepare_enable(bsp_priv->gate_clk_axi4);
		if (ret) {
			clk_disable_unprepare(bsp_priv->gate_clk_500m);
			return ret;
		}
	}

	return 0;
}

static void bm_dwmac_exit(struct platform_device *pdev, void *priv)
{
	struct cvitek_mac *bsp_priv = priv;

	if (!bsp_priv)
		return;

	if (bsp_priv->gate_clk_axi4)
		clk_disable_unprepare(bsp_priv->gate_clk_axi4);
	if (bsp_priv->gate_clk_500m)
		clk_disable_unprepare(bsp_priv->gate_clk_500m);
}

static int bm_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res = { };
	struct cvitek_mac *bsp_priv;
	int ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (ret)
		dev_warn(&pdev->dev, "cannot set 40-bit DMA mask: %d\n", ret);

	bm_eth_reset_phy(pdev);

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	bsp_priv = devm_kzalloc(&pdev->dev, sizeof(*bsp_priv), GFP_KERNEL);
	if (!bsp_priv) {
		ret = -ENOMEM;
		return -ENOMEM;
	}

	bsp_priv->dev = &pdev->dev;
	bsp_priv->gate_clk_500m = cvitek_optional_clk(&pdev->dev, "clk_500m_eth",
						      "clk_500m_eth0",
						      &bsp_priv->clk_500m_devm);
	bsp_priv->gate_clk_axi4 = cvitek_optional_clk(&pdev->dev, "clk_axi4_eth",
						      "clk_axi4_eth0",
						      &bsp_priv->clk_axi4_devm);

	plat_dat->bsp_priv = bsp_priv;
	plat_dat->init = bm_dwmac_init;
	plat_dat->exit = bm_dwmac_exit;
	/* DT compatible is "cvitek,ethernet", not snps,dwmac*. Without an
	 * explicit GMAC core type, stmmac uses DWMAC100 and the MDIO offsets
	 * are wrong. Do not force enh_desc: 3.70 HW cap is 0 and 5.10
	 * glue also leaves it unset (normal 4-word descriptors).
	 */
	plat_dat->core_type = DWMAC_CORE_GMAC;
	plat_dat->pmt = 1;
	plat_dat->force_sf_dma_mode = true;
	/* Internal EPHY is on address 0; empty MDIO slots read as 0 and
	 * 5.15 would otherwise bind dummy PHYs on 0x00-0x1f.
	 */
	plat_dat->phy_addr = 0;
	if (plat_dat->mdio_bus_data)
		plat_dat->mdio_bus_data->phy_mask = (u32)~BIT(0);

	ret = bm_dwmac_init(pdev, bsp_priv);
	if (ret)
		return ret;

	ret = stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
	if (ret) {
		bm_dwmac_exit(pdev, bsp_priv);
		return ret;
	}

	return 0;
}

static const struct of_device_id bm_dwmac_match[] = {
	{ .compatible = "cvitek,ethernet" },
	{ }
};
MODULE_DEVICE_TABLE(of, bm_dwmac_match);

static struct platform_driver bm_dwmac_driver = {
	.probe  = bm_dwmac_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name           = "bm-dwmac",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = bm_dwmac_match,
	},
};
module_platform_driver(bm_dwmac_driver);

MODULE_AUTHOR("Wei Huang<wei.huang01@bitmain.com>");
MODULE_DESCRIPTION("Cvitek DWMAC specific glue layer");
MODULE_LICENSE("GPL");
