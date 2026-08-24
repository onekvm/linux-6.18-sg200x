// SPDX-License-Identifier: GPL-2.0
/* Cvitek SG2002 / CV181x DWC2 glue: clocks, ID pin, /proc/cviusb/otg_role */

#include <linux/clk.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/usb/gadget.h>

#include "core.h"
#include "cviusb.h"

#define CVIUSB_PIN_REGS		0x03000048
#define CVIUSB_ROLE_PROC_NAME	"cviusb/otg_role"

static const char * const sel_role[] = { "host", "device" };

bool dwc2_is_cviusb(struct device *dev)
{
	return dev && dev->of_node &&
	       of_device_is_compatible(dev->of_node, "cvitek,cv182x-usb");
}

void dwc2_set_cv182x_params(struct dwc2_hsotg *hsotg)
{
	struct dwc2_core_params *p = &hsotg->params;

	p->otg_caps.hnp_support = false;
	p->otg_caps.srp_support = false;
	p->speed = DWC2_SPEED_PARAM_HIGH;
	p->phy_type = DWC2_PHY_TYPE_PARAM_UTMI;
	p->ahbcfg = GAHBCFG_HBSTLEN_INCR16 << GAHBCFG_HBSTLEN_SHIFT;
	p->phy_utmi_width = 16;
	p->g_dma = true;
	p->g_dma_desc = true;
	p->lpm = false;
	p->lpm_clock_gating = false;
	p->besl = false;
	p->hird_threshold_en = false;
	p->max_packet_count = (1 << 10) - 1;
	p->max_transfer_size = (1 << 19) - 1;
	p->reload_ctl = false;
	p->enable_dynamic_fifo = true;
	p->en_multiple_tx_fifo = true;
	p->power_down = DWC2_POWER_DOWN_PARAM_NONE;
}

static void dwc2_set_hw_id(struct dwc2_hsotg *hsotg, int is_dev)
{
	void __iomem *regs = hsotg->cviusb.usb_pin_regs;
	u32 val;

	if (!regs)
		return;
	val = ioread32(regs) & ~0x0000C0;
	iowrite32(val | (is_dev ? 0xC0 : 0x40), regs);
}

static int proc_role_show(struct seq_file *m, void *v)
{
	struct dwc2_hsotg *hsotg = m->private;

	seq_printf(m, "%s\n", sel_role[hsotg->cviusb.id_override]);
	return 0;
}

static ssize_t role_proc_write(struct file *file, const char __user *user_buf,
			       size_t count, loff_t *ppos)
{
	char procdata[32] = { };
	char str[16] = { };
	struct dwc2_hsotg *hsotg = pde_data(file_inode(file));
	unsigned int i;

	if (!user_buf || count >= sizeof(procdata))
		return -EINVAL;
	if (copy_from_user(procdata, user_buf, count))
		return -EFAULT;
	if (sscanf(procdata, "%15s", str) != 1)
		return -EINVAL;
	for (i = 0; str[i]; i++)
		str[i] = tolower(str[i]);
	for (i = 0; i < ARRAY_SIZE(sel_role); i++) {
		if (!strcmp(str, sel_role[i])) {
			hsotg->cviusb.id_override = i;
			dwc2_set_hw_id(hsotg, i);
			return count;
		}
	}
	return -EINVAL;
}

static int proc_role_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_role_show, pde_data(inode));
}

static const struct proc_ops role_proc_ops = {
	.proc_open = proc_role_open,
	.proc_read = seq_read,
	.proc_write = role_proc_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int vbus_is_present(struct cviusb_dev *cviusb)
{
	if (cviusb->vbus_gpio)
		return gpiod_get_value_cansleep(cviusb->vbus_gpio);
	return 1;
}

static irqreturn_t vbus_irq_handler(int irq, void *devid)
{
	return IRQ_WAKE_THREAD;
}

static irqreturn_t vbus_irq_thread(int irq, void *devid)
{
	struct cviusb_dev *cviusb = devid;
	struct dwc2_hsotg *hsotg = container_of(cviusb, struct dwc2_hsotg, cviusb);
	struct usb_gadget *gadget = &hsotg->gadget;
	int vbus;

	if (!gadget->udc)
		return IRQ_HANDLED;
	udelay(10);
	vbus = vbus_is_present(cviusb);
	if (cviusb->pre_vbus_status != vbus) {
		usb_udc_vbus_handler(gadget, vbus != 0);
		cviusb->pre_vbus_status = vbus;
	}
	return IRQ_HANDLED;
}

static void cviusb_get_clk(struct device *dev, struct cvi_usb_clk *clk,
			   const char *name)
{
	clk->clk_o = devm_clk_get(dev, name);
	if (IS_ERR(clk->clk_o)) {
		dev_warn(dev, "Clock %s not found\n", name);
		clk->clk_o = NULL;
	}
}

void dwc2_cviusb_clk_enable(struct dwc2_hsotg *hsotg)
{
	struct cviusb_dev *c = &hsotg->cviusb;
	struct cvi_usb_clk *clks[] = {
		&c->clk_axi, &c->clk_apb, &c->clk_125m, &c->clk_33k, &c->clk_12m
	};
	int i;

	if (!c->present)
		return;
	for (i = 0; i < ARRAY_SIZE(clks); i++) {
		if (!clks[i]->clk_o || clks[i]->is_on)
			continue;
		if (!clk_prepare_enable(clks[i]->clk_o))
			clks[i]->is_on = 1;
	}
	dwc2_set_hw_id(hsotg, c->id_override);
}

void dwc2_cviusb_clk_disable(struct dwc2_hsotg *hsotg)
{
	struct cviusb_dev *c = &hsotg->cviusb;
	struct cvi_usb_clk *clks[] = {
		&c->clk_axi, &c->clk_apb, &c->clk_125m, &c->clk_33k, &c->clk_12m
	};
	int i;

	if (!c->present)
		return;
	for (i = 0; i < ARRAY_SIZE(clks); i++) {
		if (!clks[i]->clk_o || !clks[i]->is_on)
			continue;
		clk_disable_unprepare(clks[i]->clk_o);
		clks[i]->is_on = 0;
	}
}

int dwc2_cviusb_probe(struct dwc2_hsotg *hsotg, struct platform_device *pdev)
{
	struct cviusb_dev *c = &hsotg->cviusb;
	struct resource *res;
	int vbus_irq;
	int retval;

	if (!dwc2_is_cviusb(&pdev->dev))
		return 0;

	c->present = true;
	c->usb_pin_regs = ioremap(CVIUSB_PIN_REGS, 0x4);
	c->id_override = 0;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res) {
		c->phy_regs = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(c->phy_regs))
			c->phy_regs = NULL;
	}

	cviusb_get_clk(&pdev->dev, &c->clk_axi, "clk_axi");
	cviusb_get_clk(&pdev->dev, &c->clk_apb, "clk_apb");
	cviusb_get_clk(&pdev->dev, &c->clk_125m, "clk_125m");
	cviusb_get_clk(&pdev->dev, &c->clk_33k, "clk_33k");
	cviusb_get_clk(&pdev->dev, &c->clk_12m, "clk_12m");

	c->vbus_gpio = devm_gpiod_get_optional(&pdev->dev, "vbus", GPIOD_IN);
	if (IS_ERR(c->vbus_gpio))
		return dev_err_probe(&pdev->dev, PTR_ERR(c->vbus_gpio),
				     "failed to get VBUS GPIO\n");
	if (c->vbus_gpio) {
		vbus_irq = gpiod_to_irq(c->vbus_gpio);
		if (vbus_irq < 0)
			return dev_err_probe(&pdev->dev, vbus_irq,
					     "failed to map VBUS IRQ\n");
		irq_set_status_flags(vbus_irq, IRQ_NOAUTOEN);
		retval = devm_request_threaded_irq(&pdev->dev,
						   vbus_irq,
						   vbus_irq_handler,
						   vbus_irq_thread,
						   IRQF_TRIGGER_RISING |
						   IRQF_TRIGGER_FALLING,
						   "cviusb-otg", c);
		if (retval) {
			dev_err(&pdev->dev, "failed to request vbus irq\n");
			c->vbus_gpio = NULL;
		} else {
			c->pre_vbus_status = vbus_is_present(c);
			enable_irq(vbus_irq);
		}
	}

	if (!proc_mkdir("cviusb", NULL))
		dev_warn(&pdev->dev, "cviusb: proc_mkdir failed\n");
	if (!proc_create_data(CVIUSB_ROLE_PROC_NAME, 0644, NULL,
			      &role_proc_ops, hsotg))
		dev_err(&pdev->dev, "cviusb: can't create otg_role\n");

	dev_info(&pdev->dev, "cviusb glue ready, default role host\n");
	return 0;
}

void dwc2_cviusb_remove(struct dwc2_hsotg *hsotg)
{
	if (!hsotg->cviusb.present)
		return;
	remove_proc_entry(CVIUSB_ROLE_PROC_NAME, NULL);
	remove_proc_entry("cviusb", NULL);
	if (hsotg->cviusb.usb_pin_regs)
		iounmap(hsotg->cviusb.usb_pin_regs);
	dwc2_cviusb_clk_disable(hsotg);
	hsotg->cviusb.present = false;
}
