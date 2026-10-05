// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2022-2023 CVITEK
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <crypto/aes.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/version.h>
#include <linux/jiffies.h>
#include <linux/init.h>
#include <linux/dma-buf.h>
#include <linux/dma-map-ops.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/interrupt.h>
#include <linux/scatterlist.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/cvitek_spacc.h>
#include "cvitek-spacc-regs.h"
#include <linux/delay.h>
#define DEVICE_NAME    "spacc"
#define CVITEK_SPACC_KERNEL_BUFFER_SIZE (32 * 1024)

static unsigned long spacc_ctr_inplace;
static unsigned long spacc_ctr_bounce;
module_param_named(ctr_inplace, spacc_ctr_inplace, ulong, 0444);
MODULE_PARM_DESC(ctr_inplace,
		 "AES-CTR jobs that DMA in-place (no kernel_buffer bounce)");
module_param_named(ctr_bounce, spacc_ctr_bounce, ulong, 0444);
MODULE_PARM_DESC(ctr_bounce,
		 "AES-CTR jobs that bounce through kernel_buffer");

struct cvi_spacc {
	struct device *dev;
	struct cdev cdev;
	dev_t tdev;
	void __iomem *spacc_base;
	struct class *spacc_class;
	void *buffer;
	u32 buffer_size;
	u32 used_size;

	// for sha256/sha1
	u32 state[8];
	u32 result_size;
	struct mutex engine_lock;
	void *kernel_buffer;
	u32 kernel_buffer_size;
	u32 *descriptor;

#ifdef CONFIG_PM_SLEEP
	struct clk *efuse_clk;
#endif
};

static struct cvi_spacc *cvitek_spacc_device;

#ifdef CONFIG_PM_SLEEP
static int cvitek_spacc_suspend(struct device *dev)
{
	struct cvi_spacc *spacc = dev_get_drvdata(dev);
	void __iomem *sec_top;

	clk_prepare_enable(spacc->efuse_clk);

	sec_top = ioremap(0x020b0000, 4);
	iowrite32(0x3, sec_top);
	iounmap(sec_top);

	clk_disable_unprepare(spacc->efuse_clk);
	return 0;
}

static int cvitek_spacc_resume(struct device *dev)
{
	struct cvi_spacc *spacc = dev_get_drvdata(dev);
	void __iomem *sec_top;

	clk_prepare_enable(spacc->efuse_clk);

	sec_top = ioremap(0x020b0000, 4);
	iowrite32(0x0, sec_top);
	iounmap(sec_top);

	clk_disable_unprepare(spacc->efuse_clk);
	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static SIMPLE_DEV_PM_OPS(cvitek_spacc_pm_ops, cvitek_spacc_suspend, cvitek_spacc_resume);

static inline void cvi_sha256_init(struct cvi_spacc *spacc)
{
	spacc->state[0] = cpu_to_be32(0x6A09E667);
	spacc->state[1] = cpu_to_be32(0xBB67AE85);
	spacc->state[2] = cpu_to_be32(0x3C6EF372);
	spacc->state[3] = cpu_to_be32(0xA54FF53A);
	spacc->state[4] = cpu_to_be32(0x510E527F);
	spacc->state[5] = cpu_to_be32(0x9B05688C);
	spacc->state[6] = cpu_to_be32(0x1F83D9AB);
	spacc->state[7] = cpu_to_be32(0x5BE0CD19);
}
static inline void cvi_sm3_init(struct cvi_spacc *spacc)
{
	spacc->state[0] = 0x7380166f;
	spacc->state[1] = 0x4914b2b9;
	spacc->state[2] = 0x172442d7;
	spacc->state[3] = 0xda8a0600;
	spacc->state[4] = 0xa96f30bc;
	spacc->state[5] = 0x163138aa;
	spacc->state[6] = 0xe38dee4d;
	spacc->state[7] = 0xb0fb0e4e;
}
static inline void cvi_sha1_init(struct cvi_spacc *spacc)
{
	spacc->state[0] = cpu_to_be32(0x67452301);
	spacc->state[1] = cpu_to_be32(0xEFCDAB89);
	spacc->state[2] = cpu_to_be32(0x98BADCFE);
	spacc->state[3] = cpu_to_be32(0x10325476);
	spacc->state[4] = cpu_to_be32(0xC3D2E1F0);
}

static inline void abort_cryptodma_engine(struct cvi_spacc *spacc)
{
	iowrite32(0, spacc->spacc_base + CRYPTODMA_DMA_CTRL);
	/* Flush the stop before clearing any completion latched while stopping. */
	ioread32(spacc->spacc_base + CRYPTODMA_DMA_CTRL);
	iowrite32(0x7, spacc->spacc_base + CRYPTODMA_WR_INT);
	ioread32(spacc->spacc_base + CRYPTODMA_WR_INT);
}

static inline int trigger_cryptodma_engine_and_wait_finish(struct cvi_spacc *spacc)
{
	u32 status;
	unsigned long deadline;

	iowrite32(0x7, spacc->spacc_base + CRYPTODMA_INT_MASK);
	/* Important!!! must do this */
	iowrite32(0x7, spacc->spacc_base + CRYPTODMA_WR_INT);

	iowrite32(DMA_WRITE_MAX_BURST << 24 |
			  DMA_READ_MAX_BURST << 16 |
			  DMA_DESCRIPTOR_MODE << 1 | DMA_ENABLE, spacc->spacc_base + CRYPTODMA_DMA_CTRL);

	/*
	 * Completion is WR_INT, not the DT IRQ. 107's DTB wires PLIC 59
	 * (IRQ_TYPE_LEVEL_HIGH) but that line never fires, so wait_event
	 * for 1 s stalled every SRTP batch at 1 FPS. Always poll WR_INT.
	 * Do not wait_event: a late/wrong IRQ handler would clear WR_INT
	 * and lose the completion we are polling.
	 */
	deadline = jiffies + msecs_to_jiffies(20);
	do {
		status = ioread32(spacc->spacc_base + CRYPTODMA_WR_INT);
		if (status)
			return 0;
		cpu_relax();
	} while (time_before(jiffies, deadline));
	abort_cryptodma_engine(spacc);
	return -ETIMEDOUT;
}

static inline void get_hash_result(struct cvi_spacc *spacc, int count)
{
	u32 i;
	u32 *result = (u32 *)spacc->buffer;

	for (i = 0; i < count; i++)
		result[i] = ioread32(spacc->spacc_base + CRYPTODMA_SHA_PARA + i * 4);
}
static inline void get_sm3_result(struct cvi_spacc *spacc, int count)
{
	u32 i;
	u32 *result = (u32 *)spacc->buffer;

	for (i = 0; i < count; i++)
		result[i] = ioread32(spacc->spacc_base + CRYPTODMA_SM3_PARA + i * 4);
}

static inline void setup_dma_descriptor(struct cvi_spacc *spacc, uint32_t *dma_descriptor)
{
	phys_addr_t descriptor_phys;

	/*
	 * Cube CryptoDMA is programmed with virt_to_phys(). Keep the
	 * descriptor in a GFP_DMA linear page, not on the stack: 5.15
	 * defaults CONFIG_VMAP_STACK=y and a vmapped stack is not in the
	 * linear map (5.10 Cube left it off). Bring-up config disables
	 * VMAP_STACK to match 5.10; this page is the hardening if it
	 * comes back on.
	 */
	memcpy(spacc->descriptor, dma_descriptor, 4 * 22);
	descriptor_phys = virt_to_phys(spacc->descriptor);

	dma_sync_single_for_device(spacc->dev, (dma_addr_t)descriptor_phys,
				   4 * 22, DMA_TO_DEVICE);

	// set dma descriptor addr
	iowrite32((uint32_t)((uint64_t)descriptor_phys & 0xFFFFFFFF), spacc->spacc_base + CRYPTODMA_DES_BASE_L);
	iowrite32((uint32_t)((uint64_t)descriptor_phys >> 32), spacc->spacc_base + CRYPTODMA_DES_BASE_H);
}

static inline void setup_src(struct cvi_spacc *spacc, u32 *dma_descriptor,
			     uintptr_t src, u32 len)
{
	phys_addr_t src_phys;

	src_phys = virt_to_phys((void *)src);
	dma_sync_single_for_device(spacc->dev, (dma_addr_t)src_phys, len,
				   DMA_TO_DEVICE);

	dma_descriptor[CRYPTODMA_SRC_LEN] = len;
	dma_descriptor[CRYPTODMA_SRC_ADDR_L] = (uint32_t)((uint64_t)src_phys & 0xFFFFFFFF);
	dma_descriptor[CRYPTODMA_SRC_ADDR_H] = (uint32_t)((uint64_t)src_phys >> 32);
}

static void setup_src_dst(u32 *dma_descriptor, phys_addr_t buffer, u32 len)
{
	dma_descriptor[CRYPTODMA_SRC_LEN] = len;
	dma_descriptor[CRYPTODMA_SRC_ADDR_L] = (uint32_t)((uint64_t)buffer & 0xFFFFFFFF);
	dma_descriptor[CRYPTODMA_SRC_ADDR_H] = (uint32_t)((uint64_t)buffer >> 32);

	dma_descriptor[CRYPTODMA_DST_ADDR_L] = (uint32_t)((uint64_t)buffer & 0xFFFFFFFF);
	dma_descriptor[CRYPTODMA_DST_ADDR_H] = (uint32_t)((uint64_t)buffer >> 32);
}

#define setup_dst(dst)\
do {\
	phys_addr_t dst_phys = virt_to_phys((void *)dst);\
	dma_descriptor[CRYPTODMA_DST_ADDR_L] = (uint32_t)((uint64_t)dst_phys & 0xFFFFFFFF);\
	dma_descriptor[CRYPTODMA_DST_ADDR_H] = (uint32_t)((uint64_t)dst_phys >> 32);\
} while (0)

static inline void setup_mode(u32 *dma_descriptor, SPACC_ALGO_MODE_E mode,
			      uintptr_t iv)
{
	const void __user *user_iv = (const void __user *)iv;

	switch (mode) {
	case SPACC_ALGO_MODE_CBC:
		dma_descriptor[CRYPTODMA_CTRL] |= DES_USE_DESCRIPTOR_IV;
		dma_descriptor[CRYPTODMA_CIPHER] = CBC_ENABLE << 1;
		copy_from_user(&dma_descriptor[CRYPTODMA_IV], user_iv, 16);
		break;
	case SPACC_ALGO_MODE_CTR:
		dma_descriptor[CRYPTODMA_CTRL] |= DES_USE_DESCRIPTOR_IV;
		dma_descriptor[CRYPTODMA_CIPHER] = 0x1 << 2;
		copy_from_user(&dma_descriptor[CRYPTODMA_IV], user_iv, 16);
		break;
	case SPACC_ALGO_MODE_ECB:
	default:
		break;
	}
}

static inline void setup_key_size(u32 *dma_descriptor, SPACC_KEY_SIZE_E size,
				  uintptr_t key)
{
	const void __user *user_key = (const void __user *)key;

	switch (size) {
	case SPACC_KEY_SIZE_64BITS:
		copy_from_user(&dma_descriptor[CRYPTODMA_KEY], user_key, 8);
		break;
	case SPACC_KEY_SIZE_128BITS:
		dma_descriptor[CRYPTODMA_CIPHER] |= (0x1 << 5);
		copy_from_user(&dma_descriptor[CRYPTODMA_KEY], user_key, 16);
		break;
	case SPACC_KEY_SIZE_192BITS:
		dma_descriptor[CRYPTODMA_CIPHER] |= (0x1 << 4);
		copy_from_user(&dma_descriptor[CRYPTODMA_KEY], user_key, 24);
		break;
	case SPACC_KEY_SIZE_256BITS:
		dma_descriptor[CRYPTODMA_CIPHER] |= (0x1 << 3);
		copy_from_user(&dma_descriptor[CRYPTODMA_KEY], user_key, 32);
		break;
	default:
		break;
	}
}

static inline void setup_action(u32 *dma_descriptor, SPACC_ACTION_E action)
{
	if (action == SPACC_ACTION_ENCRYPTION)
		dma_descriptor[CRYPTODMA_CIPHER] |= 0x1;
}

int spacc_sha256(struct cvi_spacc *spacc, uintptr_t src, uint32_t len)
{
	__aligned(32) u32 dma_descriptor[22] = {0};
	u32 i;

	// must mark DES_USE_DESCRIPTOR_KEY flag
	dma_descriptor[CRYPTODMA_CTRL] = DES_USE_DESCRIPTOR_KEY | DES_USE_SHA | 0xF;
	dma_descriptor[CRYPTODMA_CIPHER] = (0x1 << 1) | 0x1;

	for (i = 0; i < 8; i++)
		dma_descriptor[CRYPTODMA_KEY + i] = spacc->state[i];

	setup_src(spacc, dma_descriptor, src, len);
	setup_dma_descriptor(spacc, dma_descriptor);

	return trigger_cryptodma_engine_and_wait_finish(spacc);
}
int spacc_sm3(struct cvi_spacc *spacc, uintptr_t src, uint32_t len){
	__aligned(64) u32 dma_descriptor[22] = {0};
	u32 i;
	phys_addr_t src_phys;
	src_phys = virt_to_phys(spacc->buffer);
	dma_sync_single_for_device(spacc->dev, (dma_addr_t)src_phys,
				   spacc->used_size, DMA_TO_DEVICE);
	// must mark DES_USE_DESCRIPTOR_KEY flag
	dma_descriptor[CRYPTODMA_CTRL] = DES_USE_DESCRIPTOR_KEY|DES_USE_DESCRIPTOR_IV | DES_USE_SM3 | 0xF;
	dma_descriptor[CRYPTODMA_CIPHER] =  0x1;

	for (i = 0; i < 8; i++)
		dma_descriptor[CRYPTODMA_KEY + i] = spacc->state[i];

	setup_src_dst(dma_descriptor, src_phys, len);
	setup_dma_descriptor(spacc, dma_descriptor);

	return trigger_cryptodma_engine_and_wait_finish(spacc);
}

int spacc_sha1(struct cvi_spacc *spacc, uintptr_t src, uint32_t len)
{
	__aligned(32) u32 dma_descriptor[22] = {0};
	u32 i;

	// must mark DES_USE_DESCRIPTOR_KEY flag
	dma_descriptor[CRYPTODMA_CTRL] = DES_USE_DESCRIPTOR_KEY | DES_USE_SHA | 0xF;
	dma_descriptor[CRYPTODMA_CIPHER] = 0x1;

	for (i = 0; i < 5; i++)
		dma_descriptor[CRYPTODMA_KEY + i] = spacc->state[i];

	setup_src(spacc, dma_descriptor, src, len);
	setup_dma_descriptor(spacc, dma_descriptor);

	return trigger_cryptodma_engine_and_wait_finish(spacc);
}

int spacc_base64(struct cvi_spacc *spacc, phys_addr_t src, uint32_t len, u32 ation)
{
	int i;
	__aligned(64) u32 dma_descriptor[22] = {0};
	dma_descriptor[CRYPTODMA_CTRL] = DES_USE_BASE64 | 0xF;

	if (ation == 1) {
		dma_descriptor[CRYPTODMA_CIPHER] = 0x1;
		spacc->result_size = (len + (3 - 1)) / 3 * 4;
		dma_descriptor[CRYPTODMA_DST_LEN] = spacc->result_size;
	} else {
		spacc->result_size = (len / 4) * 3;
		dma_descriptor[CRYPTODMA_DST_LEN] = spacc->result_size;
	}

	setup_src_dst(dma_descriptor, src, len);
	setup_dma_descriptor(spacc, dma_descriptor);
	return trigger_cryptodma_engine_and_wait_finish(spacc);
}

int spacc_aes(struct cvi_spacc *spacc, phys_addr_t src, uint32_t len, spacc_aes_config_s config)
{
	__aligned(32) u32 dma_descriptor[22] = {0};

	spacc->result_size = len;
	dma_descriptor[CRYPTODMA_CTRL] = DES_USE_DESCRIPTOR_KEY | DES_USE_AES | 0xF;

	setup_mode(dma_descriptor, config.mode, config.iv);
	setup_key_size(dma_descriptor, config.key_mode, config.key);
	setup_action(dma_descriptor, config.action);

	setup_src_dst(dma_descriptor, src, len);
	setup_dma_descriptor(spacc, dma_descriptor);

	return trigger_cryptodma_engine_and_wait_finish(spacc);
}

static int spacc_aes_ctr_kernel(struct cvi_spacc *spacc, phys_addr_t buffer,
				unsigned int len, const u8 *key,
				unsigned int key_len, const u8 *iv)
{
	__aligned(32) u32 dma_descriptor[22] = { 0 };
	int error;

	dma_descriptor[CRYPTODMA_CTRL] = DES_USE_DESCRIPTOR_KEY |
		DES_USE_DESCRIPTOR_IV | DES_USE_AES | 0xF;
	dma_descriptor[CRYPTODMA_CIPHER] = (0x1 << 2) | 0x1;
	switch (key_len) {
	case 16:
		dma_descriptor[CRYPTODMA_CIPHER] |= 0x1 << 5;
		break;
	case 24:
		dma_descriptor[CRYPTODMA_CIPHER] |= 0x1 << 4;
		break;
	case 32:
		dma_descriptor[CRYPTODMA_CIPHER] |= 0x1 << 3;
		break;
	default:
		return -EINVAL;
	}
	memcpy(&dma_descriptor[CRYPTODMA_KEY], key, key_len);
	memcpy(&dma_descriptor[CRYPTODMA_IV], iv, AES_BLOCK_SIZE);
	setup_src_dst(dma_descriptor, buffer, len);
	setup_dma_descriptor(spacc, dma_descriptor);
	error = trigger_cryptodma_engine_and_wait_finish(spacc);
	memzero_explicit(dma_descriptor, sizeof(dma_descriptor));
	return error;
}

bool cvitek_spacc_kernel_api_ready(void)
{
	return READ_ONCE(cvitek_spacc_device) != NULL;
}
EXPORT_SYMBOL_GPL(cvitek_spacc_kernel_api_ready);

static bool spacc_can_ctr_inplace(struct scatterlist *source,
				  struct scatterlist *destination,
				  unsigned int engine_length)
{
	void *addr;

	if (!source || !destination)
		return false;
	if (sg_next(source) || sg_next(destination))
		return false;
	if (source->length < engine_length || destination->length < engine_length)
		return false;
	if (sg_page(source) != sg_page(destination) ||
	    source->offset != destination->offset)
		return false;
	addr = sg_virt(source);
	if (!addr || is_vmalloc_addr(addr) || !virt_addr_valid(addr))
		return false;
	if (!IS_ALIGNED((unsigned long)addr, AES_BLOCK_SIZE))
		return false;
	return true;
}

int cvitek_spacc_aes_ctr_encrypt_sg(struct scatterlist *source,
				    struct scatterlist *destination,
				    unsigned int length, const u8 *key,
				    unsigned int key_len, const u8 *iv)
{
	struct cvi_spacc *spacc = READ_ONCE(cvitek_spacc_device);
	phys_addr_t buffer_phys;
	unsigned int engine_length;
	int source_entries;
	int destination_entries;
	int error;
	bool inplace;

	if (!spacc)
		return -ENODEV;
	if (!length)
		return 0;
	if (!source || !destination || !key || !iv)
		return -EINVAL;
	engine_length = ALIGN(length, AES_BLOCK_SIZE);
	if (engine_length > spacc->kernel_buffer_size)
		return -EMSGSIZE;
	source_entries = sg_nents_for_len(source, length);
	destination_entries = sg_nents_for_len(destination, length);
	if (source_entries < 0 || destination_entries < 0)
		return -EINVAL;

	inplace = spacc_can_ctr_inplace(source, destination, engine_length);
	mutex_lock(&spacc->engine_lock);
	if (inplace) {
		void *addr = sg_virt(source);

		if (engine_length != length)
			memset(addr + length, 0, engine_length - length);
		buffer_phys = virt_to_phys(addr);
		dma_sync_single_for_device(spacc->dev, (dma_addr_t)buffer_phys,
					   engine_length, DMA_BIDIRECTIONAL);
		error = spacc_aes_ctr_kernel(spacc, buffer_phys, engine_length,
					     key, key_len, iv);
		dma_sync_single_for_cpu(spacc->dev, (dma_addr_t)buffer_phys,
					engine_length, DMA_BIDIRECTIONAL);
		spacc_ctr_inplace++;
		goto unlock;
	}
	spacc_ctr_bounce++;
	if (sg_copy_to_buffer(source, source_entries, spacc->kernel_buffer,
			      length) != length) {
		error = -EFAULT;
		goto unlock;
	}
	if (engine_length != length)
		memset(spacc->kernel_buffer + length, 0,
		       engine_length - length);
	buffer_phys = virt_to_phys(spacc->kernel_buffer);
	dma_sync_single_for_device(spacc->dev, (dma_addr_t)buffer_phys,
				   engine_length, DMA_BIDIRECTIONAL);
	error = spacc_aes_ctr_kernel(spacc, buffer_phys, engine_length, key,
				     key_len, iv);
	dma_sync_single_for_cpu(spacc->dev, (dma_addr_t)buffer_phys,
				engine_length, DMA_BIDIRECTIONAL);
	if (error)
		goto clear_buffer;
	if (sg_copy_from_buffer(destination, destination_entries,
				spacc->kernel_buffer, length) != length)
		error = -EFAULT;
clear_buffer:
	memzero_explicit(spacc->kernel_buffer, engine_length);
unlock:
	mutex_unlock(&spacc->engine_lock);
	return error;
}
EXPORT_SYMBOL_GPL(cvitek_spacc_aes_ctr_encrypt_sg);

int cvitek_spacc_aes_ctr_encrypt_phys(phys_addr_t buf, unsigned int length,
				      const u8 *key, unsigned int key_len,
				      const u8 *iv)
{
	struct cvi_spacc *spacc = READ_ONCE(cvitek_spacc_device);
	unsigned int engine_length;
	int error;

	if (!spacc)
		return -ENODEV;
	if (!length)
		return 0;
	if (!key || !iv)
		return -EINVAL;
	engine_length = ALIGN(length, AES_BLOCK_SIZE);
	if (engine_length > spacc->kernel_buffer_size)
		return -EMSGSIZE;
	mutex_lock(&spacc->engine_lock);
	dma_sync_single_for_device(spacc->dev, (dma_addr_t)buf, engine_length,
				   DMA_BIDIRECTIONAL);
	error = spacc_aes_ctr_kernel(spacc, buf, engine_length, key, key_len,
				     iv);
	dma_sync_single_for_cpu(spacc->dev, (dma_addr_t)buf, engine_length,
				DMA_BIDIRECTIONAL);
	mutex_unlock(&spacc->engine_lock);
	return error;
}
EXPORT_SYMBOL_GPL(cvitek_spacc_aes_ctr_encrypt_phys);

struct cvitek_spacc_ctr_job {
	void *buf;
	unsigned int length;
	const u8 *iv;
};

#define CVITEK_SPACC_DESC_WORDS 22
#define CVITEK_SPACC_DESC_STRIDE 24
#define CVITEK_SPACC_MAX_CHAIN 32

int cvitek_spacc_aes_ctr_encrypt_many(struct cvitek_spacc_ctr_job *jobs,
				      unsigned int count, const u8 *key,
				      unsigned int key_len)
{
	struct cvi_spacc *spacc = READ_ONCE(cvitek_spacc_device);
	unsigned int i;
	unsigned int cipher_bits;
	int error = 0;
	phys_addr_t desc_phys;
	size_t desc_bytes;

	if (!spacc)
		return -ENODEV;
	if (!count)
		return 0;
	if (!jobs || !key)
		return -EINVAL;
	if (count > CVITEK_SPACC_MAX_CHAIN)
		return -EINVAL;
	switch (key_len) {
	case 16:
		cipher_bits = 0x1 << 5;
		break;
	case 24:
		cipher_bits = 0x1 << 4;
		break;
	case 32:
		cipher_bits = 0x1 << 3;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&spacc->engine_lock);
	desc_bytes = count * CVITEK_SPACC_DESC_STRIDE * sizeof(u32);
	if (desc_bytes > PAGE_SIZE) {
		error = -ENOMEM;
		goto unlock;
	}
	memset(spacc->descriptor, 0, desc_bytes);

	for (i = 0; i < count; i++) {
		u32 *d = spacc->descriptor + i * CVITEK_SPACC_DESC_STRIDE;
		phys_addr_t buf_phys;
		phys_addr_t next_phys;
		unsigned int engine_length;

		if (!jobs[i].buf || !jobs[i].length || !jobs[i].iv) {
			error = -EINVAL;
			goto unlock;
		}
		engine_length = ALIGN(jobs[i].length, AES_BLOCK_SIZE);
		buf_phys = virt_to_phys(jobs[i].buf);
		d[CRYPTODMA_CTRL] = DES_USE_DESCRIPTOR_KEY |
			DES_USE_DESCRIPTOR_IV | DES_USE_AES | 0xF;
		d[CRYPTODMA_CIPHER] = (0x1 << 2) | 0x1 | cipher_bits;
		memcpy(&d[CRYPTODMA_KEY], key, key_len);
		memcpy(&d[CRYPTODMA_IV], jobs[i].iv, AES_BLOCK_SIZE);
		setup_src_dst(d, buf_phys, engine_length);
		if (i + 1 < count) {
			next_phys = virt_to_phys(spacc->descriptor +
						 (i + 1) * CVITEK_SPACC_DESC_STRIDE);
			d[CRYPTODMA_NEXT_PTR_ADDR_L] =
				(u32)((u64)next_phys & 0xFFFFFFFF);
			d[CRYPTODMA_NEXT_PTR_ADDR_H] =
				(u32)((u64)next_phys >> 32);
		}
		dma_sync_single_for_device(spacc->dev, (dma_addr_t)buf_phys,
					   engine_length, DMA_BIDIRECTIONAL);
	}

	desc_phys = virt_to_phys(spacc->descriptor);
	dma_sync_single_for_device(spacc->dev, (dma_addr_t)desc_phys,
				   desc_bytes, DMA_TO_DEVICE);
	iowrite32((u32)((u64)desc_phys & 0xFFFFFFFF),
		  spacc->spacc_base + CRYPTODMA_DES_BASE_L);
	iowrite32((u32)((u64)desc_phys >> 32),
		  spacc->spacc_base + CRYPTODMA_DES_BASE_H);
	error = trigger_cryptodma_engine_and_wait_finish(spacc);
	for (i = 0; i < count; i++) {
		unsigned int engine_length = ALIGN(jobs[i].length, AES_BLOCK_SIZE);
		phys_addr_t buf_phys = virt_to_phys(jobs[i].buf);

		dma_sync_single_for_cpu(spacc->dev, (dma_addr_t)buf_phys,
					engine_length, DMA_BIDIRECTIONAL);
	}
	memzero_explicit(spacc->descriptor, desc_bytes);
unlock:
	mutex_unlock(&spacc->engine_lock);
	return error;
}
EXPORT_SYMBOL_GPL(cvitek_spacc_aes_ctr_encrypt_many);

int spacc_sm4(struct cvi_spacc *spacc, phys_addr_t src, uint32_t len, spacc_sm4_config_s config)
{
	__aligned(32) u32 dma_descriptor[22] = {0};

	spacc->result_size = len;
	dma_descriptor[CRYPTODMA_CTRL] = DES_USE_DESCRIPTOR_KEY | DES_USE_SM4 | 0xF;

	setup_mode(dma_descriptor, config.mode, config.iv);
	setup_key_size(dma_descriptor, config.key_mode, config.key);
	setup_action(dma_descriptor, config.action);

	setup_src_dst(dma_descriptor, src, len);
	setup_dma_descriptor(spacc, dma_descriptor);

	return trigger_cryptodma_engine_and_wait_finish(spacc);
}

int spacc_des(struct cvi_spacc *spacc, phys_addr_t src, uint32_t len, spacc_des_config_s config, int is_tdes)
{
	__aligned(32) u32 dma_descriptor[22] = {0};

	spacc->result_size = len;
	dma_descriptor[CRYPTODMA_CTRL] = DES_USE_DESCRIPTOR_KEY | DES_USE_DES | 0xF;

	setup_mode(dma_descriptor, config.mode, config.iv);
	if (is_tdes) {
		dma_descriptor[CRYPTODMA_CIPHER] |= (0x1 << 3);
		copy_from_user(&dma_descriptor[CRYPTODMA_KEY],
			       (const void __user *)config.key, 24);
	} else {
		copy_from_user(&dma_descriptor[CRYPTODMA_KEY],
			       (const void __user *)config.key, 8);
	}
	setup_action(dma_descriptor, config.action);

	setup_src_dst(dma_descriptor, src, len);
	setup_dma_descriptor(spacc, dma_descriptor);

	return trigger_cryptodma_engine_and_wait_finish(spacc);
}

static int cvi_spacc_init_buffer(struct cvi_spacc *spacc, size_t size)
{
	unsigned int order = get_order(size);
	struct page *page;

	if (size == spacc->buffer_size) {
		return 0;
	} else if (spacc->buffer_size) {
		free_pages((unsigned long)spacc->buffer, get_order(spacc->buffer_size));
		spacc->buffer_size = 0;
	}

	page = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!page)
		return -ENOMEM;

	spacc->buffer = page_address(page);
	spacc->buffer_size = size;
	return 0;
}

static int spacc_open(struct inode *inode, struct file *file)
{
	struct cvi_spacc *spacc;

	spacc = container_of(inode->i_cdev, struct cvi_spacc, cdev);

	spacc->used_size = 0;
	spacc->result_size = 0;
	file->private_data = spacc;
	return 0;
}

static ssize_t spacc_read(struct file *filp, char *buf, size_t count, loff_t *f_pos)
{
	struct cvi_spacc *spacc = filp->private_data;
	int ret = 0;

	if (spacc->result_size == 0) {
		pr_err("spacc result is 0\n");
		return -1;
	}

	if (count < spacc->result_size)
		return -1;

	ret = copy_to_user(buf, spacc->buffer, spacc->result_size);
	if (ret != 0)
		return -1;

	return spacc->result_size;
}

static ssize_t spacc_write(struct file *filp, const char *buf, size_t count, loff_t *f_pos)
{
	struct cvi_spacc *spacc = filp->private_data;
	int ret = spacc->buffer_size - spacc->used_size;

	if (ret <= 0)
		return spacc->used_size;

	if (count > ret)
		count = ret;

	ret = copy_from_user(((unsigned char *)spacc->buffer + spacc->used_size), buf, count);
	if (ret != 0)
		return -1;

	spacc->used_size += count;
	return spacc->used_size;
}

static int spacc_release(struct inode *inode, struct file *file)
{
	return 0;
}

static long spacc_ioctl_impl(struct file *filp, unsigned int cmd,
			     unsigned long arg)
{
	struct cvi_spacc *spacc = filp->private_data;
	int ret;
	switch (cmd) {
	case IOCTL_SPACC_CREATE_MEMPOOL: {
		unsigned int size = 0;

		ret = copy_from_user((unsigned char *)&size, (unsigned char *)arg, sizeof(size));
		if (ret != 0)
			return -1;

		ret = cvi_spacc_init_buffer(spacc, size);
		if (ret != 0)
			return -1;

		break;
	}
	case IOCTL_SPACC_GET_MEMPOOL_SIZE: {
		ret = copy_to_user((unsigned char *)arg, (unsigned char *)&spacc->buffer_size,
				   sizeof(spacc->buffer_size));
		if (ret != 0)
			return -1;

		break;
	}
	case IOCTL_SPACC_SHA256_ACTION: {
		if (spacc->used_size & 0x3F) {
			pr_err("used_size : %d\n", spacc->used_size);
			return -1;
		}

		cvi_sha256_init(spacc);

		ret = spacc_sha256(spacc, (uintptr_t)spacc->buffer, spacc->used_size);
		if (ret < 0) {
			pr_err("plat_cryptodma_do failed\n");
			return -1;
		}

		get_hash_result(spacc, 8);
		spacc->result_size = 32;
		spacc->used_size = 0;
		break;
	}
	case IOCTL_SPACC_BASE64_ACTION: {
		spacc_base64_config_s b64 = {0};
		phys_addr_t src_phys = 0;
		ret = copy_from_user((unsigned char *)&b64, (unsigned char *)arg, sizeof(b64));
		if (ret != 0)
			return -1;
		src_phys = virt_to_phys(spacc->buffer);
		dma_sync_single_for_device(spacc->dev, (dma_addr_t)src_phys,
					   spacc->used_size, DMA_TO_DEVICE);
		ret = spacc_base64(spacc, src_phys, spacc->used_size, b64.action);
		if (ret < 0) {
			pr_err("plat_cryptodma_do failed\n");
			return -1;
		}

		dma_sync_single_for_cpu(spacc->dev, (dma_addr_t)src_phys,
					spacc->result_size, DMA_FROM_DEVICE);
		spacc->used_size = 0;
		break;
	}
	case IOCTL_SPACC_AES_ACTION: {
		spacc_aes_config_s config = {0};
		phys_addr_t src_phys;
		uint32_t len;
		ret = copy_from_user((unsigned char *)&config, (unsigned char *)arg, sizeof(config));
		if (ret != 0)
			return -1;

		if (config.src) {
			if ((config.len == 0) ||
				(config.len & 0xF)) {
				pr_err("src len [%d] invailed\n", config.len);
				return -1;
			}

			src_phys = (phys_addr_t)config.src;
			len = config.len;
		} else {
			if ((spacc->used_size == 0) ||
				(spacc->used_size & 0xF)) {
				pr_err("used_size : %d\n", spacc->used_size);
				return -1;
			}

			src_phys = virt_to_phys(spacc->buffer);
			len = spacc->used_size;
		}

		dma_sync_single_for_device(spacc->dev, (dma_addr_t)src_phys, len,
					   DMA_TO_DEVICE);
		ret = spacc_aes(spacc, src_phys, len, config);
		if (ret < 0) {
			pr_err("plat_cryptodma_do failed\n");
			return -1;
		}

		dma_sync_single_for_cpu(spacc->dev, (dma_addr_t)src_phys,
					spacc->result_size, DMA_FROM_DEVICE);
		spacc->used_size = 0;
		break;
	}
	case IOCTL_SPACC_SM4_ACTION: {
		spacc_sm4_config_s action = {0};
		phys_addr_t src_phys;

		if (spacc->used_size & 0xF) {
			pr_err("used_size : %d\n", spacc->used_size);
			return -1;
		}

		ret = copy_from_user((unsigned char *)&action, (unsigned char *)arg, sizeof(action));
		if (ret != 0)
			return -1;

		src_phys = virt_to_phys(spacc->buffer);
		dma_sync_single_for_device(spacc->dev, (dma_addr_t)src_phys,
					   spacc->used_size, DMA_TO_DEVICE);
		ret = spacc_sm4(spacc, src_phys, spacc->used_size, action);
		if (ret < 0) {
			pr_err("plat_cryptodma_do failed\n");
			return -1;
		}

		dma_sync_single_for_cpu(spacc->dev, (dma_addr_t)src_phys,
					spacc->result_size, DMA_FROM_DEVICE);
		spacc->used_size = 0;
		break;
	}
	case IOCTL_SPACC_DES_ACTION: {
		spacc_des_config_s action = {0};
		phys_addr_t src_phys;

		if (spacc->used_size & 0x7) {
			pr_err("spacc_dev->used_size : %d\n", spacc->used_size);
			return -1;
		}

		ret = copy_from_user((unsigned char *)&action, (unsigned char *)arg, sizeof(action));
		if (ret != 0)
			return -1;

		src_phys = virt_to_phys(spacc->buffer);
		dma_sync_single_for_device(spacc->dev, (dma_addr_t)src_phys,
					   spacc->used_size, DMA_TO_DEVICE);
		ret = spacc_des(spacc, src_phys, spacc->used_size, action, 0);
		if (ret < 0) {
			pr_err("plat_cryptodma_do failed\n");
			return -1;
		}

		dma_sync_single_for_cpu(spacc->dev, (dma_addr_t)src_phys,
					spacc->result_size, DMA_FROM_DEVICE);
		spacc->used_size = 0;
		break;
	}
	case IOCTL_SPACC_TDES_ACTION: {
		spacc_tdes_config_s action = {0};
		phys_addr_t src_phys;

		if (spacc->used_size & 0x7) {
			pr_err("spacc_dev->used_size : %d\n", spacc->used_size);
			return -EINVAL;
		}

		ret = copy_from_user((unsigned char *)&action, (unsigned char *)arg, sizeof(action));
		if (ret != 0)
			return -1;

		src_phys = virt_to_phys(spacc->buffer);
		dma_sync_single_for_device(spacc->dev, (dma_addr_t)src_phys,
					   spacc->used_size, DMA_TO_DEVICE);
		ret = spacc_des(spacc, src_phys, spacc->used_size, action, 1);
		if (ret < 0) {
			pr_err("plat_cryptodma_do failed\n");
			return -1;
		}

		dma_sync_single_for_cpu(spacc->dev, (dma_addr_t)src_phys,
					spacc->result_size, DMA_FROM_DEVICE);
		spacc->used_size = 0;
		break;
	}
	case IOCTL_SPACC_SM3_ACTION: {
		if (spacc->used_size & 0x3F) {
			pr_err("used_size : %d\n", spacc->used_size);
			return -1;
		}
		cvi_sm3_init(spacc);

		ret = spacc_sm3(spacc, (uintptr_t)spacc->buffer, spacc->used_size);
		if (ret < 0) {
			pr_err("plat_cryptodma_do failed\n");
			return -1;
		}

		get_sm3_result(spacc, 8);
		pr_err("get_sm3_result done\n");
		spacc->result_size = 32;
		spacc->used_size = 0;
		break;
	}
	default:
		return -EINVAL;
	}

	return 0;
}

static long spacc_ioctl(struct file *filp, unsigned int cmd,
			unsigned long arg)
{
	struct cvi_spacc *spacc = filp->private_data;
	long result;

	mutex_lock(&spacc->engine_lock);
	result = spacc_ioctl_impl(filp, cmd, arg);
	mutex_unlock(&spacc->engine_lock);
	return result;
}

const struct file_operations spacc_fops = {
	.owner  =   THIS_MODULE,
	.open   =   spacc_open,
	.read   =   spacc_read,
	.write  =   spacc_write,
	.release =  spacc_release,
	.unlocked_ioctl = spacc_ioctl,
};

static int cvitek_spacc_drv_probe(struct platform_device *pdev)
{
	struct cvi_spacc *spacc;
	struct device *dev = &pdev->dev;
	int ret = 0;

	spacc = devm_kzalloc(dev, sizeof(*spacc), GFP_KERNEL);
	if (!spacc)
		return -ENOMEM;

	spacc->dev = dev;
	mutex_init(&spacc->engine_lock);
	spacc->spacc_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spacc->spacc_base))
		return PTR_ERR(spacc->spacc_base);

	ret = platform_get_irq_optional(pdev, 0);
	if (ret == -EPROBE_DEFER)
		return ret;

#ifdef CONFIG_PM_SLEEP
	spacc->efuse_clk = clk_get_sys(NULL, "clk_efuse");
	if (IS_ERR(spacc->efuse_clk)) {
		pr_err("%s: efuse clock not found %ld\n", __func__
				, PTR_ERR(spacc->efuse_clk));
		return -1;
	}
#endif

	if (ret > 0) {
		/* DT IRQ 59 never completes on SG2002; requesting it would
		 * let a stray handler clear WR_INT while the engine polls. */
		dev_info(dev, "ignoring DT IRQ %d, polling WR_INT\n", ret);
	} else {
		dev_info(dev, "no routed IRQ, using completion polling\n");
	}
	spacc->kernel_buffer_size = CVITEK_SPACC_KERNEL_BUFFER_SIZE;
	spacc->kernel_buffer = (void *)__get_free_pages(
		GFP_KERNEL | GFP_DMA | __GFP_ZERO,
		get_order(spacc->kernel_buffer_size));
	if (!spacc->kernel_buffer)
		return -ENOMEM;
	spacc->descriptor = (u32 *)__get_free_page(GFP_KERNEL | GFP_DMA | __GFP_ZERO);
	if (!spacc->descriptor) {
		free_pages((unsigned long)spacc->kernel_buffer,
			   get_order(spacc->kernel_buffer_size));
		spacc->kernel_buffer = NULL;
		return -ENOMEM;
	}

	ret = alloc_chrdev_region(&spacc->tdev, 0, 1, DEVICE_NAME);
	if (ret)
		goto failed_mem;

	cdev_init(&spacc->cdev, &spacc_fops);
	spacc->cdev.owner = THIS_MODULE;

	ret = cdev_add(&spacc->cdev, spacc->tdev, 1);
	if (ret)
		goto failed_chrdev;

	spacc->spacc_class = class_create(DEVICE_NAME);
	if (IS_ERR(spacc->spacc_class)) {
		pr_err("Err: failed when create class.\n");
		cdev_del(&spacc->cdev);
		goto failed_chrdev;
	}

	device_create(spacc->spacc_class, NULL, spacc->tdev, spacc, DEVICE_NAME);
	platform_set_drvdata(pdev, spacc);
	WRITE_ONCE(cvitek_spacc_device, spacc);
	return 0;
failed_chrdev:
	unregister_chrdev_region(spacc->tdev, 1);
failed_mem:
	if (spacc->descriptor) {
		free_page((unsigned long)spacc->descriptor);
		spacc->descriptor = NULL;
	}
	if (spacc->kernel_buffer) {
		free_pages((unsigned long)spacc->kernel_buffer,
			   get_order(spacc->kernel_buffer_size));
		spacc->kernel_buffer = NULL;
	}
	return ret ? ret : -ENOMEM;
}

static void cvitek_spacc_drv_remove(struct platform_device *pdev)
{
	struct cvi_spacc *spacc = platform_get_drvdata(pdev);
	if (!spacc) {
           pr_err("spacc is NULL\n");
		return;
    }
	WRITE_ONCE(cvitek_spacc_device, NULL);
    if (spacc->spacc_class && !IS_ERR(spacc->spacc_class)) {
        device_destroy(spacc->spacc_class, spacc->tdev);
        class_destroy(spacc->spacc_class);
    }

	cdev_del(&spacc->cdev);
	unregister_chrdev_region(spacc->tdev, 1);
    if (spacc->buffer) {
        free_pages((unsigned long)spacc->buffer,
                  get_order(spacc->buffer_size));
        spacc->buffer = NULL;
        spacc->buffer_size = 0;
    }
	if (spacc->descriptor) {
		free_page((unsigned long)spacc->descriptor);
		spacc->descriptor = NULL;
	}
	if (spacc->kernel_buffer) {
		free_pages((unsigned long)spacc->kernel_buffer,
			   get_order(spacc->kernel_buffer_size));
		spacc->kernel_buffer = NULL;
		spacc->kernel_buffer_size = 0;
	}

    platform_set_drvdata(pdev, NULL);
}

#ifdef CONFIG_OF
static const struct of_device_id cvitek_spacc_of_match[] = {
	{ .compatible = "cvitek,spacc", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, cvitek_spacc_of_match);
#endif

static struct platform_driver cvitek_spacc_driver = {
	.probe		= cvitek_spacc_drv_probe,
	.remove		= cvitek_spacc_drv_remove,
	.driver		= {
		.name	= "cvitek_spacc",
		.of_match_table = of_match_ptr(cvitek_spacc_of_match),
		.pm     = &cvitek_spacc_pm_ops,
	},
};

module_platform_driver(cvitek_spacc_driver);

MODULE_DESCRIPTION("Cvitek Spacc Driver");
MODULE_LICENSE("GPL");
