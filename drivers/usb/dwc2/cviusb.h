/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __DWC2_CVIUSB_H__
#define __DWC2_CVIUSB_H__

#include <linux/clk.h>
#include <linux/gpio/consumer.h>

struct dwc2_hsotg;
struct platform_device;

struct cvi_usb_clk {
	int is_on;
	struct clk *clk_o;
};

struct cviusb_dev {
	void __iomem *phy_regs;
	void __iomem *usb_pin_regs;
	struct cvi_usb_clk clk_axi;
	struct cvi_usb_clk clk_apb;
	struct cvi_usb_clk clk_125m;
	struct cvi_usb_clk clk_33k;
	struct cvi_usb_clk clk_12m;
	struct gpio_desc *vbus_gpio;
	int pre_vbus_status;
	int id_override;
	bool present;
};

bool dwc2_is_cviusb(struct device *dev);
int dwc2_cviusb_probe(struct dwc2_hsotg *hsotg, struct platform_device *pdev);
void dwc2_cviusb_remove(struct dwc2_hsotg *hsotg);
void dwc2_cviusb_clk_enable(struct dwc2_hsotg *hsotg);
void dwc2_cviusb_clk_disable(struct dwc2_hsotg *hsotg);
void dwc2_set_cv182x_params(struct dwc2_hsotg *hsotg);

#endif
