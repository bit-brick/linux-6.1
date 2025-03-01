// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Spacemit Mobile Storage Host Controller
 *
 * Copyright (C) 2023 Spacemit
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_data/k1x_sdhci.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/reset.h>
#include <linux/cpufreq.h>

#include "sdhci.h"
#include "sdhci-pltfm.h"

#define CONFIG_K1X_MMC_DEBUG 		1
#define BOOTPART_NOACC_DEFAULT 		0

/* SDH registers define */
#define SDHC_OP_EXT_REG			0x108
#define OVRRD_CLK_OEN			0x0800
#define FORCE_CLK_ON			0x1000

#define SDHC_LEGACY_CTRL_REG		0x10C
#define GEN_PAD_CLK_ON			0x0040

#define SDHC_MMC_CTRL_REG		0x114
#define MISC_INT_EN			0x0002
#define MISC_INT			0x0004
#define ENHANCE_STROBE_EN		0x0100
#define MMC_HS400			0x0200
#define MMC_HS200			0x0400
#define MMC_CARD_MODE			0x1000

#define SDHC_TX_CFG_REG			0x11C
#define TX_INT_CLK_SEL			0x40000000
#define TX_MUX_SEL			0x80000000

#define SDHC_PHY_CTRL_REG		0x160
#define PHY_FUNC_EN			0x0001
#define PHY_PLL_LOCK			0x0002
#define HOST_LEGACY_MODE		0x80000000

#define SDHC_PHY_FUNC_REG		0x164
#define PHY_TEST_EN			0x0080
#define HS200_USE_RFIFO			0x8000

#define SDHC_PHY_DLLCFG			0x168
#define DLL_PREDLY_NUM			0x04
#define DLL_FULLDLY_RANGE		0x10
#define DLL_VREG_CTRL			0x40
#define DLL_ENABLE			0x80000000
#define DLL_REFRESH_SWEN_SHIFT		0x1C
#define DLL_REFRESH_SW_SHIFT		0x1D

#define SDHC_PHY_DLLCFG1		0x16C
#define DLL_REG2_CTRL			0x0C
#define DLL_REG3_CTRL_MASK		0xFF
#define DLL_REG3_CTRL_SHIFT		0x10
#define DLL_REG2_CTRL_MASK		0xFF
#define DLL_REG2_CTRL_SHIFT		0x08
#define DLL_REG1_CTRL			0x92
#define DLL_REG1_CTRL_MASK		0xFF
#define DLL_REG1_CTRL_SHIFT		0x00

#define SDHC_PHY_DLLSTS			0x170
#define DLL_LOCK_STATE			0x01

#define SDHC_PHY_DLLSTS1		0x174
#define DLL_MASTER_DELAY_MASK		0xFF
#define DLL_MASTER_DELAY_SHIFT		0x10

#define SDHC_PHY_PADCFG_REG		0x178
#define RX_BIAS_CTRL_SHIFT		0x5
#define PHY_DRIVE_SEL_SHIFT		0x0
#define PHY_DRIVE_SEL_MASK		0x7
#define PHY_DRIVE_SEL_DEFAULT		0x4

#define RPM_DELAY			50
#define MAX_74CLK_WAIT_COUNT		100

#define MMC1_IO_V18EN			0x04
#define AKEY_ASFAR			0xBABA
#define AKEY_ASSAR			0xEB10

#define SDHC_RX_CFG_REG			0x118
#define RX_SDCLK_SEL0_MASK		0x03
#define RX_SDCLK_SEL0_SHIFT		0x00
#define RX_SDCLK_SEL0			0x02
#define RX_SDCLK_SEL1_MASK		0x03
#define RX_SDCLK_SEL1_SHIFT		0x02
#define RX_SDCLK_SEL1			0x01

#define SDHC_DLINE_CTRL_REG		0x130
#define DLINE_PU			0x01
#define RX_DLINE_CODE_MASK		0xFF
#define RX_DLINE_CODE_SHIFT		0x10
#define TX_DLINE_CODE_MASK		0xFF
#define TX_DLINE_CODE_SHIFT		0x18

#define SDHC_DLINE_CFG_REG		0x134
#define RX_DLINE_REG_MASK		0xFF
#define RX_DLINE_REG_SHIFT		0x00
#define RX_DLINE_GAIN_MASK		0x1
#define RX_DLINE_GAIN_SHIFT		0x8
#define RX_DLINE_GAIN			0x1
#define TX_DLINE_REG_MASK		0xFF
#define TX_DLINE_REG_SHIFT		0x10

#define SDHC_RX_TUNE_DELAY_MIN		0x0
#define SDHC_RX_TUNE_DELAY_MAX		0xFF
#define SDHC_RX_TUNE_DELAY_STEP		0x1

static struct sdhci_host* sdio_host;

#define MMC_CAP2_QUIRK_BREAK_SDR104	(1 << 30)

struct sdhci_spacemit {
	struct clk *clk_core;
	struct clk *clk_io;
	struct clk *clk_aib;
	struct reset_control *reset;
	unsigned char power_mode;
	struct pinctrl_state *pin;
	struct pinctrl *pinctrl;
};

static int spacemit_reg[] = {
	0x100, 0x104, 0x108, 0x10c, 0x110, 0x114, 0x118, 0x11c,
	0x120, 0x124, 0x128, 0x12c, 0x130, 0x134, 0x160, 0x164,
	0x168, 0x16c, 0x170, 0x174, 0x178, 0x17c, 0x180, 0x184,
	0x188, 0x18c, 0x190, 0x1f0, 0x1f4, 0xFFF,
};

#ifdef CONFIG_K1X_MMC_DEBUG
static u8 cur_com_reg[960]; /* 8 line, 120  character  per line */
static u8 cur_pri_reg[960];
static u8 pre_com_reg[960];
static u8 pre_pri_reg[960];
#endif

#define spacemit_monitor_cmd(cmd) (((cmd) == MMC_READ_SINGLE_BLOCK) || \
				((cmd) == MMC_READ_MULTIPLE_BLOCK) || \
				((cmd) == MMC_WRITE_BLOCK) || \
				((cmd) == MMC_WRITE_MULTIPLE_BLOCK) || \
				((cmd) == MMC_SWITCH) || \
				((cmd) == MMC_ERASE))

static const u32 tuning_patten4[16] = {
	0x00ff0fff, 0xccc3ccff, 0xffcc3cc3, 0xeffefffe,
	0xddffdfff, 0xfbfffbff, 0xff7fffbf, 0xefbdf777,
	0xf0fff0ff, 0x3cccfc0f, 0xcfcc33cc, 0xeeffefff,
	0xfdfffdff, 0xffbfffdf, 0xfff7ffbb, 0xde7b7ff7,
};

static const u32 tuning_patten8[32] = {
	0xff00ffff, 0x0000ffff, 0xccccffff, 0xcccc33cc,
	0xcc3333cc, 0xffffcccc, 0xffffeeff, 0xffeeeeff,
	0xffddffff, 0xddddffff, 0xbbffffff, 0xbbffffff,
	0xffffffbb, 0xffffff77, 0x77ff7777, 0xffeeddbb,
	0x00ffffff, 0x00ffffff, 0xccffff00, 0xcc33cccc,
	0x3333cccc, 0xffcccccc, 0xffeeffff, 0xeeeeffff,
	0xddffffff, 0xddffffff, 0xffffffdd, 0xffffffbb,
	0xffffbbbb, 0xffff77ff, 0xff7777ff, 0xeeddbb77,
};

static int is_recovery_boot;
static int __init recovery_boot_mode(char *str)
{
	if ((str != NULL) && (str[0] == '1'))
		is_recovery_boot = 1;

	return 0;
}
#ifndef MODULE
__setup("recovery=", recovery_boot_mode);
#endif

static void __maybe_unused dump_sdh_regs(struct sdhci_host *host, u8 *com_reg, u8 *pri_reg)
{
	int val;
	int offset;
	int i;
	int len;
	u8 *buf;

	buf = com_reg;
	len = 0;
	i = 0;
	for (offset = 0; offset < 0x70; offset += 4) {
		val = sdhci_readl(host, offset);
		if (i % 4 == 0)
			len += sprintf(buf + len, "\n");
		len += sprintf(buf + len, "\toffset:0x%03x 0x%08x\t", offset, val);
		i++;
	}

	if (i % 4 == 0)
		len += sprintf(buf + len, "\n");
	val = sdhci_readl(host, 0xe0);
	len += sprintf(buf + len, "\toffset:0x%03x 0x%08x\t", 0xe0, val);
	val = sdhci_readl(host, 0xfc);
	len += sprintf(buf + len, "\toffset:0x%03x 0x%08x\t\n", 0xfc, val);

	buf = pri_reg;
	len = 0;
	i = 0;
	do {
		if (((host->mmc->caps2 & MMC_CAP2_NO_MMC) || (host->quirks2 & SDHCI_QUIRK2_BROKEN_PHY_MODULE)) &&
		    (spacemit_reg[i] > 0x134))
			break;
		val = sdhci_readl(host, spacemit_reg[i]);
		if (i % 4 == 0)
			len += sprintf(buf + len, "\n");
		len += sprintf(buf + len, "\toffset:0x%03x 0x%08x\t", spacemit_reg[i], val);
		i++;
	} while (spacemit_reg[i] != 0xFFF);
	len += sprintf(buf + len, "\n");
}

static void spacemit_reset_dllcfg1_reg(struct sdhci_host *host, u32 dllcfg1)
{
	u32 reg;

	sdhci_writel(host, dllcfg1, SDHC_PHY_DLLCFG1);

	reg = sdhci_readl(host, SDHC_PHY_DLLCFG);
	reg |= 1 << DLL_REFRESH_SWEN_SHIFT;
	sdhci_writel(host, reg, SDHC_PHY_DLLCFG);

	reg = sdhci_readl(host, SDHC_PHY_DLLCFG);
	reg |= 1 << DLL_REFRESH_SW_SHIFT;
	sdhci_writel(host, reg, SDHC_PHY_DLLCFG);

	reg = sdhci_readl(host, SDHC_PHY_DLLCFG);
	reg &= ~(1 << DLL_REFRESH_SW_SHIFT);
	sdhci_writel(host, reg, SDHC_PHY_DLLCFG);

	reg = sdhci_readl(host, SDHC_PHY_DLLCFG);
	reg &= ~(1 << DLL_REFRESH_SWEN_SHIFT);
	sdhci_writel(host, reg, SDHC_PHY_DLLCFG);

	udelay(1);
}

static __maybe_unused void spacemit_handle_emmc_read_crc(struct sdhci_host *host)
{
	struct mmc_host *mmc = host->mmc;
	struct k1x_sdhci_platdata *pdata = mmc->parent->platform_data;

	u32 data;
	u32 dllcfg1, dlldelay;
	u32 dll_reg3_ctrl, dll_master_delay;

	pdata->prev_dllcfg1 = pdata->curr_dllcfg1;
	pdata->curr_dllcfg1 = sdhci_readl(host, SDHC_PHY_DLLCFG1);

	dllcfg1 = pdata->curr_dllcfg1;
	dll_reg3_ctrl = (dllcfg1 >> DLL_REG3_CTRL_SHIFT) & DLL_REG3_CTRL_MASK;
	dlldelay = sdhci_readl(host, SDHC_PHY_DLLSTS1);
	dll_master_delay = (dlldelay >> DLL_MASTER_DELAY_SHIFT) & DLL_MASTER_DELAY_MASK;

	if (!dll_reg3_ctrl) {
		/* first time reset */
		data = dll_master_delay / 3;
		pdata->dllcfg1_odd_reset = 1;
	} else if (pdata->dllcfg1_odd_reset & 0x1) {
		/* odd time */
		data = 0xFF - (dll_master_delay - dll_reg3_ctrl) / 3;
		pdata->dllcfg1_odd_reset = 2;
	} else {
		/* even time */
		data = dll_master_delay + 0xFF - dll_reg3_ctrl;
		data = data / 3;
		pdata->dllcfg1_odd_reset = 1;
	}

	/* reset DLL_REG3_CTRL */
	dllcfg1 &= (~(DLL_REG3_CTRL_MASK << DLL_REG3_CTRL_SHIFT));
	dllcfg1 |= (data << DLL_REG3_CTRL_SHIFT);

	if (unlikely(dllcfg1 == pdata->prev_dllcfg1)) {
		/* reset the default: 0x0000000c */
		pr_warn("%s: maybe fail to fix crc!\n", mmc_hostname(mmc));
		/* debug purpose */
		BUG_ON(1);
		spacemit_reset_dllcfg1_reg(host, 0x0000000c);
		pdata->new_dllcfg1 = 0x0000000c;
		pdata->need_reset_dllcfg1 = 0;
	} else {
		spacemit_reset_dllcfg1_reg(host, dllcfg1);
		pdata->new_dllcfg1 = dllcfg1;
		pdata->need_reset_dllcfg1 = 1;
	}
}

static u32 spacemit_handle_interrupt(struct sdhci_host *host, u32 intmask)
{
	u32 cmd;

	/* handle sdio SDHCI_INT_CARD_INT */
	if ((intmask & SDHCI_INT_CARD_INT) && (host->ier & SDHCI_INT_CARD_INT)) {
		if (!(host->flags & SDHCI_DEVICE_DEAD)) {
			host->ier &= ~SDHCI_INT_CARD_INT;
			sdhci_writel(host, host->ier, SDHCI_INT_ENABLE);
			sdhci_writel(host, host->ier, SDHCI_SIGNAL_ENABLE);
		}

		/* wakeup ksdioirqd thread */
		host->mmc->sdio_irq_pending = true;
		if (host->mmc->sdio_irq_thread)
			wake_up_process(host->mmc->sdio_irq_thread);
	}

	/* handle error interrupts */
	if (intmask & SDHCI_INT_ERROR) {
		cmd = SDHCI_GET_CMD(sdhci_readw(host, SDHCI_COMMAND));
		if (spacemit_monitor_cmd(cmd)) {
			printk_ratelimited(KERN_ERR "%s: cmd%d error(INT status:0x%08x).\n",
				mmc_hostname(host->mmc), cmd, intmask);
#ifdef CONFIG_K1X_MMC_DEBUG
			/* dump host register */
			dump_sdh_regs(host, &cur_com_reg[0], &cur_pri_reg[0]);
			printk_ratelimited(KERN_INFO "%s", cur_com_reg);
			printk_ratelimited(KERN_INFO "%s", cur_pri_reg);
			//pr_err("register before cmd%d trigger %s", cmd, pre_com_reg);
			//pr_err("%s", pre_pri_reg);
#endif
		}

		if (intmask & (SDHCI_INT_CRC | SDHCI_INT_DATA_CRC | SDHCI_INT_DATA_END_BIT | SDHCI_INT_AUTO_CMD_ERR)) {
			/* handle crc error for sd device */
			if (host->mmc->caps2 & MMC_CAP2_NO_MMC) {
				host->mmc->caps2 |= MMC_CAP2_QUIRK_BREAK_SDR104;
			}
		}
	}

	return intmask;
}

#ifdef CONFIG_K1X_MMC_DEBUG
void __maybe_unused spacemit_save_sdhci_regs(struct sdhci_host *host, u32 cmd)
{
	if (host->mmc->card && spacemit_monitor_cmd(cmd))
		dump_sdh_regs(host, &pre_com_reg[0], &pre_pri_reg[0]);
}
EXPORT_SYMBOL(spacemit_save_sdhci_regs);
#endif

extern int __mmc_claim_host(struct mmc_host *host, struct mmc_ctx *ctx, atomic_t *abort);
extern void mmc_release_host(struct mmc_host *host);

void spacemit_sdio_detect_change(int enable_scan)
{
#define MMC_CARD_REMOVED	(1<<4) /* card has been removed */
	struct mmc_card	*sdio_card;

	if (sdio_host) {
		if (enable_scan) {
			sdio_card = sdio_host->mmc->card;
			if (sdio_card && (sdio_card->sdio_funcs)) {
				__mmc_claim_host(sdio_host->mmc, NULL, NULL);
				mmc_sw_reset(sdio_host->mmc->card);
				mmc_release_host(sdio_host->mmc);
			} else {
				/* first insmod */
				sdio_host->mmc->rescan_entered = 0;
				mmc_detect_change(sdio_host->mmc, 0);
			}
		} else {
			/* can not directly use the mmc_stop_host helper due to GKI restrictions.
			 * use the detect process to remove the card.
			 */
			if (!sdio_host->mmc || !sdio_host->mmc->card) {
				/* sdio card does not exist */
				return;
			}
			sdio_host->mmc->rescan_entered = 0;
			sdio_host->mmc->card->state |= MMC_CARD_REMOVED;
			mmc_detect_change(sdio_host->mmc, 0);
		}
	}
}
EXPORT_SYMBOL(spacemit_sdio_detect_change);

static void spacemit_sdhci_reset(struct sdhci_host *host, u8 mask)
{
	struct platform_device *pdev;
	struct k1x_sdhci_platdata *pdata;
	unsigned int reg;

	pdev = to_platform_device(mmc_dev(host->mmc));
	pdata = pdev->dev.platform_data;
	sdhci_reset(host, mask);

	if (mask != SDHCI_RESET_ALL)
		return;

	/* sd/sdio only be SDHCI_QUIRK2_BROKEN_PHY_MODULE */
	if (!(host->quirks2 & SDHCI_QUIRK2_BROKEN_PHY_MODULE)) {
		if (host->quirks2 & SDHCI_QUIRK2_SUPPORT_PHY_BYPASS) {
			/* use phy bypass */
			reg = sdhci_readl(host, SDHC_TX_CFG_REG);
			reg |= TX_INT_CLK_SEL;
			sdhci_writel (host, reg, SDHC_TX_CFG_REG);

			reg = sdhci_readl(host, SDHC_PHY_CTRL_REG);
			reg |= HOST_LEGACY_MODE;
			sdhci_writel (host, reg, SDHC_PHY_CTRL_REG);

			reg = sdhci_readl(host, SDHC_PHY_FUNC_REG);
			reg |= PHY_TEST_EN;
			sdhci_writel (host, reg, SDHC_PHY_FUNC_REG);
		} else {
			/* use phy func mode */
			reg = sdhci_readl(host, SDHC_PHY_CTRL_REG);
			reg |= (PHY_FUNC_EN | PHY_PLL_LOCK);
			sdhci_writel(host, reg, SDHC_PHY_CTRL_REG);

			reg = sdhci_readl(host, SDHC_PHY_PADCFG_REG);
			reg |= (1 << RX_BIAS_CTRL_SHIFT);

			reg &= ~(PHY_DRIVE_SEL_MASK);
			reg |= (pdata->phy_driver_sel & PHY_DRIVE_SEL_MASK) << PHY_DRIVE_SEL_SHIFT;
			sdhci_writel(host, reg, SDHC_PHY_PADCFG_REG);
		}
	} else {
		reg = sdhci_readl(host, SDHC_TX_CFG_REG);
		reg |= TX_INT_CLK_SEL;
		sdhci_writel (host, reg, SDHC_TX_CFG_REG);
	}

	/* for emmc */
	if (!(host->mmc->caps2 & MMC_CAP2_NO_MMC)) {
		/* mmc card mode */
		reg = sdhci_readl(host, SDHC_MMC_CTRL_REG);
		reg |= MMC_CARD_MODE;
		sdhci_writel(host, reg, SDHC_MMC_CTRL_REG);
	}
}

static void spacemit_sdhci_gen_init_74_clocks(struct sdhci_host *host, u8 power_mode)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_spacemit *spacemit = sdhci_pltfm_priv(pltfm_host);
	unsigned int reg;
	int count;

	if (!(host->mmc->caps2 & MMC_CAP2_NO_SDIO))
		return;

	if ((spacemit->power_mode == MMC_POWER_UP) &&
		(power_mode == MMC_POWER_ON)) {
		reg = sdhci_readl(host, SDHC_MMC_CTRL_REG);
		reg |= MISC_INT_EN;
		sdhci_writel(host, reg, SDHC_MMC_CTRL_REG);

		reg = sdhci_readl(host, SDHC_LEGACY_CTRL_REG);
		reg |= GEN_PAD_CLK_ON;
		sdhci_writel(host, reg, SDHC_LEGACY_CTRL_REG);

		count = 0;
		while (count++ < MAX_74CLK_WAIT_COUNT) {
			if (sdhci_readl(host, SDHC_MMC_CTRL_REG) & MISC_INT)
				break;
			udelay(10);
		}

		if (count == MAX_74CLK_WAIT_COUNT)
			pr_warn("%s: gen 74 clock interrupt timeout\n",
				mmc_hostname(host->mmc));

		reg = sdhci_readl(host, SDHC_MMC_CTRL_REG);
		reg |= MISC_INT;
		sdhci_writel(host, reg, SDHC_MMC_CTRL_REG);
	}
	spacemit->power_mode = power_mode;
}

static void __maybe_unused spacemit_sdhci_caps_disable(struct sdhci_host *host)
{
	struct platform_device *pdev;
	struct k1x_sdhci_platdata *pdata;

	pdev = to_platform_device(mmc_dev(host->mmc));
	pdata = pdev->dev.platform_data;

	if (pdata->host_caps_disable)
		host->mmc->caps &= ~(pdata->host_caps_disable);
	if (pdata->host_caps2_disable)
		host->mmc->caps2 &= ~(pdata->host_caps2_disable);
}

static void spacemit_sdhci_set_uhs_signaling(struct sdhci_host *host, unsigned timing)
{
	u16 reg;

	if ((timing == MMC_TIMING_MMC_HS200) ||
	    (timing == MMC_TIMING_MMC_HS400)) {
		reg = sdhci_readw(host, SDHC_MMC_CTRL_REG);
		reg |= (timing == MMC_TIMING_MMC_HS200) ? MMC_HS200 : MMC_HS400;
		sdhci_writew(host, reg, SDHC_MMC_CTRL_REG);
	}
	sdhci_set_uhs_signaling(host, timing);
	if (!(host->mmc->caps2 & MMC_CAP2_NO_SDIO)) {
		reg = sdhci_readw(host, SDHCI_HOST_CONTROL2);
		sdhci_writew(host, reg | SDHCI_CTRL_VDD_180, SDHCI_HOST_CONTROL2);
	}
}

static void spacemit_sdhci_set_clk_gate(struct sdhci_host *host, unsigned int auto_gate)
{
	unsigned int reg;

	reg = sdhci_readl(host, SDHC_OP_EXT_REG);
	if (auto_gate)
		reg &= ~(OVRRD_CLK_OEN | FORCE_CLK_ON);
	else
		reg |= (OVRRD_CLK_OEN | FORCE_CLK_ON);
	sdhci_writel(host, reg, SDHC_OP_EXT_REG);
}

static int spacemit_sdhci_card_busy(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);
	u32 present_state;
	u32 ret;
	u32 cmd;

	/* Check whether DAT[0] is 0 */
	present_state = sdhci_readl(host, SDHCI_PRESENT_STATE);
	ret = !(present_state & SDHCI_DATA_0_LVL_MASK);

	if (host->mmc->caps2 & MMC_CAP2_NO_MMC) {
		cmd = SDHCI_GET_CMD(sdhci_readw(host, SDHCI_COMMAND));
		if ((cmd == SD_SWITCH_VOLTAGE) && (host->mmc->ios.signal_voltage == MMC_SIGNAL_VOLTAGE_180)) {
			/* recover the auto clock */
			spacemit_sdhci_set_clk_gate(host, 1);
		}
	}

	return ret;
}

static void spacemit_init_card_quriks(struct mmc_host *mmc, struct mmc_card *card)
{
	if (mmc->caps2 & MMC_CAP2_NO_MMC) {
		/* break sdr104 */
		if (mmc->caps2 & MMC_CAP2_QUIRK_BREAK_SDR104) {
			mmc->caps &= ~MMC_CAP_UHS_SDR104;
			mmc->caps2 &= ~MMC_CAP2_QUIRK_BREAK_SDR104;
		 } else {
			struct k1x_sdhci_platdata *pdata = mmc->parent->platform_data;
			struct rx_tuning *rxtuning = &pdata->rxtuning;

			if (rxtuning->tuning_fail) {
				/* fallback bus speed */
				mmc->caps &= ~MMC_CAP_UHS_SDR104;
				rxtuning->tuning_fail = 0;
			} else {
				/* recovery sdr104 capability */
				mmc->caps |= MMC_CAP_UHS_SDR104;
			}
		 }
	}

	if (!(mmc->caps2 & MMC_CAP2_NO_SDIO)) {
		/* disable MMC_CAP2_SDIO_IRQ_NOTHREAD */
		mmc->caps2 &= ~MMC_CAP2_SDIO_IRQ_NOTHREAD;

		/* use the fake irq pending to avoid to read the SDIO_CCCR_INTx
		 * which sometimes return an abnormal value.
		 */
		mmc->sdio_irq_pending = true;
	}
}

static void spacemit_sdhci_enable_sdio_irq_nolock(struct sdhci_host *host, int enable)
{
	if (!(host->flags & SDHCI_DEVICE_DEAD)) {
		if (enable)
			host->ier |= SDHCI_INT_CARD_INT;
		else
			host->ier &= ~SDHCI_INT_CARD_INT;

		sdhci_writel(host, host->ier, SDHCI_INT_ENABLE);
		sdhci_writel(host, host->ier, SDHCI_SIGNAL_ENABLE);
	}
}

static void spacemit_sdhci_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct sdhci_host *host = mmc_priv(mmc);
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	spacemit_sdhci_enable_sdio_irq_nolock(host, enable);
	spin_unlock_irqrestore(&host->lock, flags);
}

static void spacemit_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct sdhci_host *host = mmc_priv(mmc);
	unsigned long flags;

	spacemit_sdhci_enable_sdio_irq(mmc, enable);

	/* avoid to read the SDIO_CCCR_INTx */
	spin_lock_irqsave(&host->lock, flags);
	mmc->sdio_irq_pending = true;
	spin_unlock_irqrestore(&host->lock, flags);
}

static void spacemit_sdhci_set_clock(struct sdhci_host *host, unsigned int clock)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_spacemit *spacemit = sdhci_pltfm_priv(pltfm_host);
	struct mmc_host *mmc = host->mmc;
	unsigned int reg;
	u32 cmd;

	/* according to the SDHC_TX_CFG_REG(0x11c<bit>),
	 * set TX_INT_CLK_SEL to gurantee the hold time
	 * at default speed mode or HS/SDR12/SDR25/SDR50 mode.
	 */
	reg = sdhci_readl(host, SDHC_TX_CFG_REG);
	if ((mmc->ios.timing == MMC_TIMING_LEGACY) ||
	    (mmc->ios.timing == MMC_TIMING_SD_HS) ||
	    (mmc->ios.timing == MMC_TIMING_UHS_SDR12) ||
	    (mmc->ios.timing == MMC_TIMING_UHS_SDR25) ||
	    (mmc->ios.timing == MMC_TIMING_UHS_SDR50) ||
	    (mmc->ios.timing == MMC_TIMING_MMC_HS)) {
		reg |= TX_INT_CLK_SEL;
	} else {
		reg &= ~TX_INT_CLK_SEL;
	}
	sdhci_writel(host, reg, SDHC_TX_CFG_REG);

	/* set pinctrl state */
	if (spacemit->pinctrl && !IS_ERR(spacemit->pinctrl)) {
		if (clock >= 200000000) {
			spacemit->pin = pinctrl_lookup_state(spacemit->pinctrl, "fast");
			if (IS_ERR(spacemit->pin))
				pr_warn("could not get sdhci pinctrl state.\n");
			else
				pinctrl_select_state(spacemit->pinctrl, spacemit->pin);

		} else {
			spacemit->pin = pinctrl_lookup_state(spacemit->pinctrl, "default");
			if (IS_ERR(spacemit->pin))
				pr_warn("could not get sdhci pinctrl state.\n");
			else
				pinctrl_select_state(spacemit->pinctrl, spacemit->pin);
		}
	}

	if (host->mmc->caps2 & MMC_CAP2_NO_MMC) {
		/*
		* according to the SD spec, during a signal voltage level switch,
		* the clock must be closed for 5 ms.
		* then, the host starts providing clk at 1.8 and the host checks whether
		* DAT[3:0] is high after 1ms clk.
		*
		* for the above goal, temporarily disable the auto clk and keep clk always on for 1ms.
		*/
		cmd = SDHCI_GET_CMD(sdhci_readw(host, SDHCI_COMMAND));
		if ((cmd == SD_SWITCH_VOLTAGE) && (host->mmc->ios.signal_voltage == MMC_SIGNAL_VOLTAGE_180)) {
			/* disable auto clock */
			spacemit_sdhci_set_clk_gate(host, 0);
		}
	}

	sdhci_set_clock(host, clock);
};

static void spacemit_sdhci_phy_dll_init(struct sdhci_host *host)
{
	u32 reg;
	int i;

	/* config dll_reg1 & dll_reg2 */
	reg = sdhci_readl(host, SDHC_PHY_DLLCFG);
	reg |= (DLL_PREDLY_NUM | DLL_FULLDLY_RANGE | DLL_VREG_CTRL);
	sdhci_writel(host, reg, SDHC_PHY_DLLCFG);

	reg = sdhci_readl(host, SDHC_PHY_DLLCFG1);
	reg |= (DLL_REG1_CTRL & DLL_REG1_CTRL_MASK);
	sdhci_writel(host, reg, SDHC_PHY_DLLCFG1);

	/* dll enable */
	reg = sdhci_readl(host, SDHC_PHY_DLLCFG);
	reg |= DLL_ENABLE;
	sdhci_writel(host, reg, SDHC_PHY_DLLCFG);

	/* wait dll lock */
	i = 0;
	while (i++ < 100) {
		if (sdhci_readl(host, SDHC_PHY_DLLSTS) & DLL_LOCK_STATE)
			break;
		udelay(10);
	}
	if (i == 100)
		pr_err("%s: dll lock timeout\n", mmc_hostname(host->mmc));
}

static void spacemit_sdhci_hs400_enhanced_strobe(struct mmc_host *mmc,
					struct mmc_ios *ios)
{
	u32 reg;
	struct sdhci_host *host = mmc_priv(mmc);

	reg = sdhci_readl(host, SDHC_MMC_CTRL_REG);
	if (ios->enhanced_strobe)
		reg |= ENHANCE_STROBE_EN;
	else
		reg &= ~ENHANCE_STROBE_EN;
	sdhci_writel(host, reg, SDHC_MMC_CTRL_REG);

	if (ios->enhanced_strobe)
		spacemit_sdhci_phy_dll_init(host);
}

static int spacemit_sdhci_start_signal_voltage_switch(struct mmc_host *mmc,
					struct mmc_ios *ios)
{
	struct sdhci_host *host = mmc_priv(mmc);
	u16 ctrl;
	int ret;

	/*
	 * Signal Voltage Switching is only applicable for Host Controllers
	 * v3.00 and above.
	 */
	if (host->version < SDHCI_SPEC_300)
		return 0;

	ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);

	switch (ios->signal_voltage) {
	case MMC_SIGNAL_VOLTAGE_330:
		if (!(host->flags & SDHCI_SIGNALING_330))
			return -EINVAL;
		/* Set 1.8V Signal Enable in the Host Control2 register to 0 */
		ctrl &= ~SDHCI_CTRL_VDD_180;
		sdhci_writew(host, ctrl, SDHCI_HOST_CONTROL2);

		/* Some controller need to do more when switching */
		if (host->ops->voltage_switch)
			host->ops->voltage_switch(host);

		if (!IS_ERR(mmc->supply.vqmmc)) {
			ret = mmc_regulator_set_vqmmc(mmc, ios);
			if (ret < 0) {
				pr_warn("%s: Switching to 3.3V signalling voltage failed\n",
					mmc_hostname(mmc));
				return -EIO;
			}
		}
		/* Wait for 5ms */
		usleep_range(5000, 5500);

		/* 3.3V regulator output should be stable within 5 ms */
		ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);
		if (!(ctrl & SDHCI_CTRL_VDD_180))
			return 0;

		pr_warn("%s: 3.3V regulator output did not become stable\n",
			mmc_hostname(mmc));

		return -EAGAIN;
	case MMC_SIGNAL_VOLTAGE_180:
		if (!(host->flags & SDHCI_SIGNALING_180))
			return -EINVAL;
		if (!IS_ERR(mmc->supply.vqmmc)) {
			ret = mmc_regulator_set_vqmmc(mmc, ios);
			if (ret < 0) {
				pr_warn("%s: Switching to 1.8V signalling voltage failed\n",
					mmc_hostname(mmc));
				return -EIO;
			}
		}

		/*
		 * Enable 1.8V Signal Enable in the Host Control2
		 * register
		 */
		ctrl |= SDHCI_CTRL_VDD_180;
		sdhci_writew(host, ctrl, SDHCI_HOST_CONTROL2);

		/* Some controller need to do more when switching */
		if (host->ops->voltage_switch)
			host->ops->voltage_switch(host);

		/* 1.8V regulator output should be stable within 5 ms */
		ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);
		if (ctrl & SDHCI_CTRL_VDD_180)
			return 0;

		pr_warn("%s: 1.8V regulator output did not become stable\n",
			mmc_hostname(mmc));

		return -EAGAIN;
	case MMC_SIGNAL_VOLTAGE_120:
		if (!(host->flags & SDHCI_SIGNALING_120))
			return -EINVAL;
		if (!IS_ERR(mmc->supply.vqmmc)) {
			ret = mmc_regulator_set_vqmmc(mmc, ios);
			if (ret < 0) {
				pr_warn("%s: Switching to 1.2V signalling voltage failed\n",
					mmc_hostname(mmc));
				return -EIO;
			}
		}
		return 0;
	default:
		/* No signal voltage switch required */
		return 0;
	}
}

static void spacemit_set_aib_mmc1_io(struct sdhci_host *host, int vol)
{
	void __iomem *aib_mmc1_io;
	void __iomem *apbc_asfar;
	void __iomem *apbc_assar;
	u32 reg;
	struct platform_device *pdev;
	struct k1x_sdhci_platdata *pdata;

	pdev = to_platform_device(mmc_dev(host->mmc));
	pdata = pdev->dev.platform_data;

	if (!pdata->aib_mmc1_io_reg ||
	    !pdata->apbc_asfar_reg ||
	    !pdata->apbc_assar_reg)
		return;

	aib_mmc1_io = ioremap(pdata->aib_mmc1_io_reg, 4);
	apbc_asfar = ioremap(pdata->apbc_asfar_reg, 4);
	apbc_assar = ioremap(pdata->apbc_assar_reg, 4);

	writel(AKEY_ASFAR, apbc_asfar);
	writel(AKEY_ASSAR, apbc_assar);
	reg = readl(aib_mmc1_io);

	switch (vol) {
	case MMC_SIGNAL_VOLTAGE_180:
		reg |= MMC1_IO_V18EN;
		break;
	default:
		reg &= ~MMC1_IO_V18EN;
		break;
	}
	writel(AKEY_ASFAR, apbc_asfar);
	writel(AKEY_ASSAR, apbc_assar);
	writel(reg, aib_mmc1_io);

	iounmap(apbc_assar);
	iounmap(apbc_asfar);
	iounmap(aib_mmc1_io);
}

static void spacemit_sdhci_voltage_switch(struct sdhci_host *host)
{
	struct mmc_host *mmc = host->mmc;
	struct mmc_ios ios = mmc->ios;

	/*
	 * v18en(MS) bit should meet TSMC's requirement when switch SOC SD
	 * IO voltage from 3.3(3.0)v to 1.8v
	 */
	if (host->quirks2 & SDHCI_QUIRK2_SET_AIB_MMC)
		spacemit_set_aib_mmc1_io(host, ios.signal_voltage);
}

static void spacemit_sw_rx_tuning_prepare(struct sdhci_host *host, u8 dline_reg)
{
	struct mmc_host *mmc = host->mmc;
	struct mmc_ios ios = mmc->ios;
	u32 reg;

	reg = sdhci_readl(host, SDHC_DLINE_CFG_REG);
	reg &= ~(RX_DLINE_REG_MASK << RX_DLINE_REG_SHIFT);
	reg |= dline_reg << RX_DLINE_REG_SHIFT;
	reg &= ~(RX_DLINE_GAIN_MASK << RX_DLINE_GAIN_SHIFT);
	if ((ios.timing == MMC_TIMING_UHS_SDR50) && (reg & 0x40))
		reg |= RX_DLINE_GAIN << RX_DLINE_GAIN_SHIFT;
	sdhci_writel(host, reg, SDHC_DLINE_CFG_REG);

	reg = sdhci_readl(host, SDHC_DLINE_CTRL_REG);
	reg |= DLINE_PU;
	sdhci_writel(host, reg, SDHC_DLINE_CTRL_REG);
	udelay(5);

	reg = sdhci_readl(host, SDHC_RX_CFG_REG);
	reg &= ~(RX_SDCLK_SEL1_MASK << RX_SDCLK_SEL1_SHIFT);
	reg |= RX_SDCLK_SEL1 << RX_SDCLK_SEL1_SHIFT;
	sdhci_writel(host, reg, SDHC_RX_CFG_REG);

	if ((mmc->ios.timing == MMC_TIMING_MMC_HS200)
		&& !(host->quirks2 & SDHCI_QUIRK2_BROKEN_PHY_MODULE)) {
		reg = sdhci_readl(host, SDHC_PHY_FUNC_REG);
		reg |= HS200_USE_RFIFO;
		sdhci_writel(host, reg, SDHC_PHY_FUNC_REG);
	}
}

static void spacemit_sw_rx_set_delaycode(struct sdhci_host *host, u32 delay)
{
	u32 reg;

	reg = sdhci_readl(host, SDHC_DLINE_CTRL_REG);
	reg &= ~(RX_DLINE_CODE_MASK << RX_DLINE_CODE_SHIFT);
	reg |= (delay & RX_DLINE_CODE_MASK) << RX_DLINE_CODE_SHIFT;
	sdhci_writel(host, reg, SDHC_DLINE_CTRL_REG);
}

static void spacemit_sw_tx_tuning_prepare(struct sdhci_host *host)
{
	u32 reg;

	/* set TX_MUX_SEL */
	reg = sdhci_readl(host, SDHC_TX_CFG_REG);
	reg |= TX_MUX_SEL;
	sdhci_writel(host, reg, SDHC_TX_CFG_REG);

	reg = sdhci_readl(host, SDHC_DLINE_CTRL_REG);
	reg |= DLINE_PU;
	sdhci_writel(host, reg, SDHC_DLINE_CTRL_REG);
	udelay(5);
}

static void spacemit_sw_tx_set_dlinereg(struct sdhci_host *host, u8 dline_reg)
{
	u32 reg;

	reg = sdhci_readl(host, SDHC_DLINE_CFG_REG);
	reg &= ~(TX_DLINE_REG_MASK << TX_DLINE_REG_SHIFT);
	reg |= dline_reg << TX_DLINE_REG_SHIFT;
	sdhci_writel(host, reg, SDHC_DLINE_CFG_REG);
}

static void spacemit_sw_tx_set_delaycode(struct sdhci_host *host, u32 delay)
{
	u32 reg;

	reg = sdhci_readl(host, SDHC_DLINE_CTRL_REG);
	reg &= ~(TX_DLINE_CODE_MASK << TX_DLINE_CODE_SHIFT);
	reg |= (delay & TX_DLINE_CODE_MASK) << TX_DLINE_CODE_SHIFT;
	sdhci_writel(host, reg, SDHC_DLINE_CTRL_REG);
}

static void spacemit_sdhci_clear_set_irqs(struct sdhci_host *host, u32 clr, u32 set)
{
	u32 ier;

	ier = sdhci_readl(host, SDHCI_INT_ENABLE);
	ier &= ~clr;
	ier |= set;
	sdhci_writel(host, ier, SDHCI_INT_ENABLE);
	sdhci_writel(host, ier, SDHCI_SIGNAL_ENABLE);
}

static int spacemit_tuning_patten_check(struct sdhci_host *host, int point)
{
	u32 read_patten;
	unsigned int i;
	u32 *tuning_patten;
	int patten_len;
	int err = 0;

	if (host->mmc->ios.bus_width == MMC_BUS_WIDTH_8) {
		tuning_patten = (u32 *)tuning_patten8;
		patten_len = ARRAY_SIZE(tuning_patten8);
	} else {
		tuning_patten = (u32 *)tuning_patten4;
		patten_len = ARRAY_SIZE(tuning_patten4);
	}

	for (i = 0; i < patten_len; i++) {
		read_patten = sdhci_readl(host, SDHCI_BUFFER);
		if (read_patten != tuning_patten[i])
			err++;
	}

	return err;
}

static int spacemit_send_tuning_cmd(struct sdhci_host *host, u32 opcode,
					int point, unsigned long flags)
{
	int err = 0;

	spin_unlock_irqrestore(&host->lock, flags);

	sdhci_send_tuning(host, opcode);

	spin_lock_irqsave(&host->lock, flags);
	if (!host->tuning_done) {
		pr_err("%s: Timeout waiting for Buffer Read Ready interrupt "
			"during tuning procedure, resetting CMD and DATA\n",
			mmc_hostname(host->mmc));
		sdhci_reset(host, SDHCI_RESET_CMD|SDHCI_RESET_DATA);
		/* err = -EIO; */
	} else
		err = spacemit_tuning_patten_check(host, point);

	host->tuning_done = 0;
	return err;
}

static int spacemit_sw_rx_select_window(struct sdhci_host *host, u32 opcode)
{
	int min;
	int max;
	u16 ctrl;
	u32 ier;
	unsigned long flags = 0;
	int err = 0;
	int i, j, len;
	struct tuning_window tmp;
	struct mmc_host *mmc = host->mmc;
	struct k1x_sdhci_platdata *pdata = mmc->parent->platform_data;
	struct rx_tuning *rxtuning = &pdata->rxtuning;

	/* change to pio mode during the tuning stage */
	spin_lock_irqsave(&host->lock, flags);
	ier = sdhci_readl(host, SDHCI_INT_ENABLE);
	spacemit_sdhci_clear_set_irqs(host, ier, SDHCI_INT_DATA_AVAIL);

	min = SDHC_RX_TUNE_DELAY_MIN;
	do {
		/* find the mininum delay first which can pass tuning */
		while (min < SDHC_RX_TUNE_DELAY_MAX) {
			spacemit_sw_rx_set_delaycode(host, min);
			if (!mmc->ops->get_cd(mmc)) {
				spin_unlock_irqrestore(&host->lock, flags);
				return -ENODEV;
			}
			err = spacemit_send_tuning_cmd(host, opcode, min, flags);
			if (err == -EIO) {
				spin_unlock_irqrestore(&host->lock, flags);
				return -EIO;
			}
			if (!err)
				break;
			ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);
			ctrl &= ~(SDHCI_CTRL_TUNED_CLK | SDHCI_CTRL_EXEC_TUNING);
			sdhci_writew(host, ctrl, SDHCI_HOST_CONTROL2);
			min += SDHC_RX_TUNE_DELAY_STEP;
		}

		/* find the maxinum delay which can not pass tuning */
		max = min + SDHC_RX_TUNE_DELAY_STEP;
		while (max < SDHC_RX_TUNE_DELAY_MAX) {
			spacemit_sw_rx_set_delaycode(host, max);
			if (!mmc->ops->get_cd(mmc)) {
				spin_unlock_irqrestore(&host->lock, flags);
				return -ENODEV;
			}
			err = spacemit_send_tuning_cmd(host, opcode, max, flags);
			if (err) {
				ctrl = sdhci_readw(host, SDHCI_HOST_CONTROL2);
				ctrl &= ~(SDHCI_CTRL_TUNED_CLK | SDHCI_CTRL_EXEC_TUNING);
				sdhci_writew(host, ctrl, SDHCI_HOST_CONTROL2);
				if (err == -EIO) {
					spin_unlock_irqrestore(&host->lock, flags);
					return -EIO;
				}
				break;
			}
			max += SDHC_RX_TUNE_DELAY_STEP;
		}

		pr_notice("%s: pass window [%d %d) \n", mmc_hostname(host->mmc), min, max);
		/* store the top 3 window */
		if ((max - min) >= rxtuning->window_limit) {
			tmp.max_delay = max;
			tmp.min_delay = min;
			tmp.type = pdata->rxtuning.window_type;
			for (i = 0; i < CANDIDATE_WIN_NUM; i++) {
				len = rxtuning->windows[i].max_delay - rxtuning->windows[i].min_delay;
				if ((tmp.max_delay - tmp.min_delay) > len) {
					for (j = CANDIDATE_WIN_NUM - 1; j > i; j--) {
						rxtuning->windows[j] = rxtuning->windows[j-1];
					}
					rxtuning->windows[i] = tmp;
					break;
				}
			}
		}
		min = max + SDHC_RX_TUNE_DELAY_STEP;
	} while (min < SDHC_RX_TUNE_DELAY_MAX);

	spacemit_sdhci_clear_set_irqs(host, SDHCI_INT_DATA_AVAIL, ier);
	spin_unlock_irqrestore(&host->lock, flags);
	return 0;
}

static int spacemit_sw_rx_select_delay(struct sdhci_host *host)
{
	int i;
	int win_len, min, max, mid;
	struct tuning_window *window;

	struct mmc_host *mmc = host->mmc;
	struct k1x_sdhci_platdata *pdata = mmc->parent->platform_data;
	struct rx_tuning *tuning = &pdata->rxtuning;

	for (i = 0; i < CANDIDATE_WIN_NUM; i++) {
		window = &tuning->windows[i];
		min = window->min_delay;
		max = window->max_delay;
		mid = (min + max - 1) / 2;
		win_len = max - min;
		if (win_len < tuning->window_limit)
			continue;

		if (window->type == LEFT_WINDOW) {
			tuning->select_delay[tuning->select_delay_num++] = min + win_len / 4;
			tuning->select_delay[tuning->select_delay_num++] = min + win_len / 3;
		} else if (window->type == RIGHT_WINDOW) {
			tuning->select_delay[tuning->select_delay_num++] = max - win_len / 4;
			tuning->select_delay[tuning->select_delay_num++] = max - win_len / 3;
		} else {
			tuning->select_delay[tuning->select_delay_num++] = mid;
			tuning->select_delay[tuning->select_delay_num++] = mid + win_len / 4;
			tuning->select_delay[tuning->select_delay_num++] = mid - win_len / 4;
		}
	}

	return tuning->select_delay_num;
}

static void spacemit_sw_rx_card_store(struct sdhci_host *host, struct rx_tuning *tuning)
{
	struct mmc_card *card = host->mmc->card;

	if (card)
		memcpy(tuning->card_cid, card->raw_cid, sizeof(card->raw_cid));
}

static int spacemit_sw_rx_card_pretuned(struct sdhci_host *host, struct rx_tuning *tuning)
{
	struct mmc_card *card = host->mmc->card;

	if (!card)
		return 0;

	return !memcmp(tuning->card_cid, card->raw_cid, sizeof(card->raw_cid));
}

static int spacemit_sdhci_execute_sw_tuning(struct sdhci_host *host, u32 opcode)
{
	int ret;
	int index;
	struct mmc_host *mmc = host->mmc;
	struct mmc_ios ios = mmc->ios;
	struct k1x_sdhci_platdata *pdata = mmc->parent->platform_data;
	struct rx_tuning *rxtuning = &pdata->rxtuning;
	struct cpufreq_policy *policy;
	unsigned int clk_rate;

	/*
	 * Tuning is required for SDR50/SDR104, HS200/HS400 cards and
	 * if clock frequency is greater than 100MHz in these modes.
	 */
	if (host->clock < 100 * 1000 * 1000 ||
	    !((ios.timing == MMC_TIMING_MMC_HS200) ||
	      (ios.timing == MMC_TIMING_UHS_SDR50) ||
	      (ios.timing == MMC_TIMING_UHS_SDR104)))
		return 0;

	if (!(mmc->caps2 & MMC_CAP2_NO_SD) && !mmc->ops->get_cd(mmc)) {
		return 0;
	}

	/* TX tuning config */
	if ((host->mmc->caps2 & MMC_CAP2_NO_MMC) || (host->quirks2 & SDHCI_QUIRK2_BROKEN_PHY_MODULE)) {
		spacemit_sw_tx_set_dlinereg(host, pdata->tx_dline_reg);
		spacemit_sw_tx_set_delaycode(host, pdata->tx_delaycode);
		pr_info("%s: set tx_delaycode: %d\n", mmc_hostname(mmc), pdata->tx_delaycode);
		spacemit_sw_tx_tuning_prepare(host);
	}

	/* step 1: check pretuned card */
	if (spacemit_sw_rx_card_pretuned(host, rxtuning) &&
	    rxtuning->select_delay_num) {
		index = rxtuning->current_delay_index;
		if (mmc->doing_retune)
			index++;
		if (index == rxtuning->select_delay_num) {
			pr_info("%s: all select delay failed, re-init to DDR50\n", mmc_hostname(mmc));
			rxtuning->select_delay_num = 0;
			rxtuning->current_delay_index = 0;
			memset(rxtuning->windows, 0, sizeof(rxtuning->windows));
			memset(rxtuning->select_delay, 0xFF, sizeof(rxtuning->select_delay));
			memset(rxtuning->card_cid, 0, sizeof(rxtuning->card_cid));
			rxtuning->tuning_fail = 1;
			return -EIO;
		}

		spacemit_sw_rx_tuning_prepare(host, rxtuning->rx_dline_reg);
		spacemit_sw_rx_set_delaycode(host, rxtuning->select_delay[index]);
		pr_info("%s: pretuned card, use select_delay[%d]:%d\n",
			mmc_hostname(mmc), index, rxtuning->select_delay[index]);
		rxtuning->current_delay_index = index;
		return 0;
	}

	/* specify cpu freq during tuning rx windows if current cpufreq exceed 1.6G */
	if (pdata->rx_tuning_freq) {
		clk_rate= cpufreq_generic_get(0);
		if (clk_rate && (clk_rate != pdata->rx_tuning_freq)) {
			policy = cpufreq_cpu_get(0);
			if (policy) {
				ret = cpufreq_driver_target(policy, pdata->rx_tuning_freq, 0);
				pr_info("%s: change cpu frequency from %d to %d before tuning\n", mmc_hostname(mmc),
					clk_rate, pdata->rx_tuning_freq);
				if (ret)
					pr_err("%s: failed to change cpu frequency before tuning, err: %d\n",
						mmc_hostname(mmc), ret);
			}
		}
	}

	rxtuning->select_delay_num = 0;
	rxtuning->current_delay_index = 0;
	memset(rxtuning->windows, 0, sizeof(rxtuning->windows));
	memset(rxtuning->select_delay, 0xFF, sizeof(rxtuning->select_delay));
	memset(rxtuning->card_cid, 0, sizeof(rxtuning->card_cid));

	/* step 2: get pass window and caculate the select_delay */
	spacemit_sw_rx_tuning_prepare(host, rxtuning->rx_dline_reg);
	ret = spacemit_sw_rx_select_window(host, opcode);

	if (ret) {
		pr_warn("%s: abort tuning, err:%d\n", mmc_hostname(mmc), ret);
		rxtuning->tuning_fail = 1;
		goto restore_freq;
	}

	if (!spacemit_sw_rx_select_delay(host)) {
		pr_warn("%s: fail to get delaycode\n", mmc_hostname(mmc));
		rxtuning->tuning_fail = 1;
		ret = -EIO;
		goto restore_freq;
	}

	/* step 3: set the delay code and store card cid */
	spacemit_sw_rx_set_delaycode(host, rxtuning->select_delay[0]);
	spacemit_sw_rx_card_store(host, rxtuning);
	rxtuning->tuning_fail = 0;
	pr_info("%s: tuning done, use the firstly delay_code:%d\n",
		mmc_hostname(mmc), rxtuning->select_delay[0]);

restore_freq:
	if (pdata->rx_tuning_freq) {
		if (clk_rate)
			cpufreq_driver_target(policy, clk_rate, 0);
		if (policy)
			cpufreq_cpu_put(policy);
	}
	return ret;
}

static unsigned int spacemit_sdhci_clk_get_max_clock(struct sdhci_host *host)
{
	unsigned long rate;
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);

	rate = clk_get_rate(pltfm_host->clk);
	return rate;
}

static unsigned int spacemit_get_max_timeout_count(struct sdhci_host *host)
{
	/*
	 * the default sdhci code use the 1 << 27 as the max timeout counter
	 * to calculate the max_busy_timeout.
	 * aquilac sdhci support 1 << 29 as the timeout counter.
	 */
	return 1 << 29;
}

static int spacemit_sdhci_pre_select_hs400(struct mmc_host *mmc)
{
	u32 reg;
	struct sdhci_host *host = mmc_priv(mmc);

	reg = sdhci_readl(host, SDHC_MMC_CTRL_REG);
	reg |= MMC_HS400;
	sdhci_writel(host, reg, SDHC_MMC_CTRL_REG);
	host->mmc->caps |= MMC_CAP_WAIT_WHILE_BUSY;

	return 0;
}

static void spacemit_sdhci_post_select_hs400(struct mmc_host *mmc)
{
	struct sdhci_host *host = mmc_priv(mmc);

	spacemit_sdhci_phy_dll_init(host);
	host->mmc->caps &= ~MMC_CAP_WAIT_WHILE_BUSY;
}

static void spacemit_sdhci_pre_hs400_to_hs200(struct mmc_host *mmc)
{
	u32 reg;
	struct sdhci_host *host = mmc_priv(mmc);

	reg = sdhci_readl(host, SDHC_PHY_CTRL_REG);
	reg &= ~(PHY_FUNC_EN | PHY_PLL_LOCK);
	sdhci_writel(host, reg, SDHC_PHY_CTRL_REG);

	reg = sdhci_readl(host, SDHC_MMC_CTRL_REG);
	reg &= ~(MMC_HS400 | MMC_HS200 | ENHANCE_STROBE_EN);
	sdhci_writel(host, reg, SDHC_MMC_CTRL_REG);

	reg = sdhci_readl(host, SDHC_PHY_FUNC_REG);
	reg &= ~HS200_USE_RFIFO;
	sdhci_writel(host, reg, SDHC_PHY_FUNC_REG);

	udelay(5);

	reg = sdhci_readl(host, SDHC_PHY_CTRL_REG);
	reg |= (PHY_FUNC_EN | PHY_PLL_LOCK);
	sdhci_writel(host, reg, SDHC_PHY_CTRL_REG);
}

static void __maybe_unused spacemit_sdhci_reset_dllcfg1(struct sdhci_host *host)
{
	struct mmc_host *mmc = host->mmc;
	struct k1x_sdhci_platdata *pdata = mmc->parent->platform_data;

	if (sdhci_readl(host, SDHC_PHY_DLLCFG1) == pdata->new_dllcfg1)
		return;
	if (!pdata->need_reset_dllcfg1)
		return;

	spacemit_reset_dllcfg1_reg(host, pdata->new_dllcfg1);
}

static void spacemit_sdhci_set_encrypt(struct sdhci_host *host, unsigned int enc_flag)
{
	u32 reg;

	if ((host->quirks2 & SDHCI_QUIRK2_SUPPORT_ENCRYPT)) {
		reg = sdhci_readl(host, SDHC_DLINE_CFG_REG);
		if (enc_flag)
			reg &= ~(0x1 << 8);
		else
			reg |= (0x1 << 8);
		sdhci_writel(host, reg, SDHC_DLINE_CFG_REG);
	}
}

static void spacemit_sdhci_dump_vendor_regs(struct sdhci_host *host)
{
#ifdef CONFIG_K1X_MMC_DEBUG
	dump_sdh_regs(host, &cur_com_reg[0], &cur_pri_reg[0]);
	printk_ratelimited(KERN_INFO "%s", cur_com_reg);
	printk_ratelimited(KERN_INFO "%s", cur_pri_reg);
#endif
}

static const struct sdhci_ops spacemit_sdhci_ops = {
	.set_clock = spacemit_sdhci_set_clock,
	.platform_send_init_74_clocks = spacemit_sdhci_gen_init_74_clocks,
	.get_max_clock = spacemit_sdhci_clk_get_max_clock,
	.get_max_timeout_count = spacemit_get_max_timeout_count,
	.set_bus_width = sdhci_set_bus_width,
	.reset = spacemit_sdhci_reset,
	.set_uhs_signaling = spacemit_sdhci_set_uhs_signaling,
	.voltage_switch = spacemit_sdhci_voltage_switch,
	.platform_execute_tuning = spacemit_sdhci_execute_sw_tuning,
	.irq = spacemit_handle_interrupt,
	.set_power = sdhci_set_power_and_bus_voltage,
	.dump_vendor_regs = spacemit_sdhci_dump_vendor_regs,
#ifdef CONFIG_SOC_SPACEMIT_K1X
	.set_encrypt_feature = spacemit_sdhci_set_encrypt,
#endif
};

static struct sdhci_pltfm_data sdhci_k1x_pdata = {
	.ops = &spacemit_sdhci_ops,
	.quirks = SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK
		| SDHCI_QUIRK_NO_ENDATTR_IN_NOPDESC
		| SDHCI_QUIRK_32BIT_ADMA_SIZE
		| SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_BROKEN_64_BIT_DMA,
};

static const struct of_device_id sdhci_spacemit_of_match[] = {
	{
		.compatible = "spacemit,k1-x-sdhci",
	},
	{},
};
MODULE_DEVICE_TABLE(of, sdhci_spacemit_of_match);

static struct k1x_sdhci_platdata *spacemit_get_mmc_pdata(struct device *dev)
{
	struct k1x_sdhci_platdata *pdata;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return NULL;
	return pdata;
}

static void spacemit_get_of_property(struct sdhci_host *host,
		struct device *dev, struct k1x_sdhci_platdata *pdata)
{
	struct device_node *np = dev->of_node;
	u32 property;

	/* sdh io clk */
	if (!of_property_read_u32(np, "spacemit,sdh-freq", &property))
		pdata->host_freq = property;

	if (!of_property_read_u32(np, "spacemit,sdh-flags", &property))
		pdata->flags |= property;

	if (!of_property_read_u32(np, "spacemit,sdh-host-caps", &property))
		pdata->host_caps |= property;
	if (!of_property_read_u32(np, "spacemit,sdh-host-caps2", &property))
		pdata->host_caps2 |= property;

	if (!of_property_read_u32(np, "spacemit,sdh-host-caps-disable", &property))
		pdata->host_caps_disable |= property;
	if (!of_property_read_u32(np, "spacemit,sdh-host-caps2-disable", &property))
		pdata->host_caps2_disable |= property;

	if (!of_property_read_u32(np, "spacemit,sdh-quirks", &property))
		pdata->quirks |= property;
	if (!of_property_read_u32(np, "spacemit,sdh-quirks2", &property))
		pdata->quirks2 |= property;

	pdata->aib_mmc1_io_reg = 0x0;
	pdata->apbc_asfar_reg = 0x0;
	pdata->apbc_assar_reg = 0x0;
	if (!of_property_read_u32(np, "spacemit,aib_mmc1_io_reg", &property))
		pdata->aib_mmc1_io_reg = property;
	if (!of_property_read_u32(np, "spacemit,apbc_asfar_reg", &property))
		pdata->apbc_asfar_reg = property;
	if (!of_property_read_u32(np, "spacemit,apbc_assar_reg", &property))
		pdata->apbc_assar_reg = property;

	/* read rx tuning dline_reg */
	if (!of_property_read_u32(np, "spacemit,rx_dline_reg", &property))
		pdata->rxtuning.rx_dline_reg = (u8)property;
	else
		pdata->rxtuning.rx_dline_reg = RX_TUNING_DLINE_REG;

	/* read rx tuning window limit */
	if (!of_property_read_u32(np, "spacemit,rx_tuning_limit", &property))
		pdata->rxtuning.window_limit = (u8)property;
	else
		pdata->rxtuning.window_limit = RX_TUNING_WINDOW_THRESHOLD;

	/* read rx tuning window type */
	if (!of_property_read_u32(np, "spacemit,rx_tuning_type", &property))
		pdata->rxtuning.window_type = (u8)property;
	else
		pdata->rxtuning.window_type = MIDDLE_WINDOW;

	/* tx tuning dline_reg */
	if (!of_property_read_u32(np, "spacemit,tx_dline_reg", &property))
		pdata->tx_dline_reg = (u8)property;
	else
		pdata->tx_dline_reg = TX_TUNING_DLINE_REG;
	if (!of_property_read_u32(np, "spacemit,tx_delaycode", &property))
		pdata->tx_delaycode = (u8)property;
	else
		pdata->tx_delaycode = TX_TUNING_DELAYCODE;

	/* phy driver select */
	if (!of_property_read_u32(np, "spacemit,phy_driver_sel", &property))
		pdata->phy_driver_sel = (u8)property;
	else
		pdata->phy_driver_sel = PHY_DRIVE_SEL_DEFAULT;

	/* read rx tuning cpufreq, unit 1000Hz */
	if (!of_property_read_u32(np, "spacemit,rx_tuning_freq", &property))
		pdata->rx_tuning_freq = property;

	return;
}

#ifdef CONFIG_SPACEMIT_SW_JTAG
extern void switch_jtag_tapctl(unsigned int tap_ctl);
#endif
#define SD_PMUX_SYSFS "/sys/devices/platform/soc/d4200000.axi/d4280000.sdh/sd_card_pmux"
ssize_t sdhci_sysfs_pmux_set(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_spacemit *spacemit = sdhci_pltfm_priv(pltfm_host);
	char pinname[16];

	if (spacemit == NULL || spacemit->pinctrl == NULL) {
		pr_err("could not set sd pinctrl.\n");
		return count;
	}

	if (!strncmp(buf, "jtag", strlen("jtag"))) {
		strncpy(pinname, "jtag", strlen("jtag") + 1);
		spacemit_set_aib_mmc1_io(host, MMC_SIGNAL_VOLTAGE_180);
#ifdef CONFIG_SPACEMIT_SW_JTAG
		/* switch tap_ctl as seconod jtag by sw_jtag */
		switch_jtag_tapctl(0x42);
#endif
	} else if (!strncmp(buf, "default", strlen("default"))) {
		strncpy(pinname, "default", strlen("default") + 1);
		spacemit_set_aib_mmc1_io(host, MMC_SIGNAL_VOLTAGE_330);
#ifdef CONFIG_SPACEMIT_SW_JTAG
		/* switch tap_ctl as primary jtag by sw_jtag */
		switch_jtag_tapctl(0x0a);
#endif
	} else {
		pr_info("usage: echo [jtag | default] > %s\n", SD_PMUX_SYSFS);
		return count;
	}

	spacemit->pin = pinctrl_lookup_state(spacemit->pinctrl, pinname);
	if (IS_ERR(spacemit->pin)) {
		pr_err("could not get sdhci pinctrl state.\n");
		return count;
	}
	pinctrl_select_state(spacemit->pinctrl, spacemit->pin);
	msleep(1);

	return count;
}

ssize_t sdhci_tx_delaycode_show(struct device *dev, struct device_attribute *attr,
					char *buf)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct mmc_host *mmc = host->mmc;
	struct k1x_sdhci_platdata *pdata = mmc->parent->platform_data;

	return sprintf(buf, "0x%02x\n", pdata->tx_delaycode);
}

ssize_t sdhci_tx_delaycode_set(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct mmc_host *mmc = host->mmc;
	struct k1x_sdhci_platdata *pdata = mmc->parent->platform_data;
	u8 delaycode;

	if (kstrtou8(buf, 0, &delaycode))
		return -EINVAL;

	pdata->tx_delaycode = delaycode;
	return count;
}

static struct device_attribute sdhci_sysfs_files[] = {
	__ATTR(sd_card_pmux, S_IWUSR, NULL, sdhci_sysfs_pmux_set),
	__ATTR(tx_delaycode, S_IRUGO|S_IWUSR, sdhci_tx_delaycode_show, sdhci_tx_delaycode_set),
};

static int spacemit_sdhci_probe(struct platform_device *pdev)
{
	struct sdhci_pltfm_host *pltfm_host;
	struct device *dev = &pdev->dev;
	struct sdhci_host *host;
	const struct of_device_id *match;
	int i;
	struct sdhci_spacemit *spacemit;
	struct k1x_sdhci_platdata *pdata;
	int ret;

	host = sdhci_pltfm_init(pdev, &sdhci_k1x_pdata, sizeof(*spacemit));
	if (IS_ERR(host))
		return PTR_ERR(host);

	pltfm_host = sdhci_priv(host);

	spacemit = sdhci_pltfm_priv(pltfm_host);

	spacemit->clk_io = devm_clk_get(dev, "sdh-io");
	if (IS_ERR(spacemit->clk_io))
		spacemit->clk_io = devm_clk_get(dev, NULL);
	if (IS_ERR(spacemit->clk_io)) {
		dev_err(dev, "failed to get io clock\n");
		ret = PTR_ERR(spacemit->clk_io);
		goto err_clk_get;
	}
	pltfm_host->clk = spacemit->clk_io;
	clk_prepare_enable(spacemit->clk_io);

	spacemit->clk_core = devm_clk_get(dev, "sdh-core");
	if (!IS_ERR(spacemit->clk_core))
		clk_prepare_enable(spacemit->clk_core);

	spacemit->clk_aib = devm_clk_get(dev, "aib-clk");
	if (!IS_ERR(spacemit->clk_aib))
		clk_prepare_enable(spacemit->clk_aib);

	spacemit->reset = devm_reset_control_array_get_optional_shared(dev);
	if (IS_ERR(spacemit->reset)) {
		dev_err(dev, "failed to get reset control\n");
		ret = PTR_ERR(spacemit->reset);
		goto err_rst_get;
	}

	ret = reset_control_deassert(spacemit->reset);
	if (ret)
		goto err_rst_get;

	match = of_match_device(of_match_ptr(sdhci_spacemit_of_match), &pdev->dev);
	if (match) {
		ret = mmc_of_parse(host->mmc);
		if (ret)
			goto err_of_parse;
		sdhci_get_of_property(pdev);
	}

	pdata = pdev->dev.platform_data ? pdev->dev.platform_data : spacemit_get_mmc_pdata(dev);
	if (IS_ERR_OR_NULL(pdata))
		goto err_of_parse;

	spacemit_get_of_property(host, dev, pdata);
	if (pdata->quirks)
		host->quirks |= pdata->quirks;
	if (pdata->quirks2)
		host->quirks2 |= pdata->quirks2;
	if (pdata->host_caps)
		host->mmc->caps |= pdata->host_caps;
	if (pdata->host_caps2)
		host->mmc->caps2 |= pdata->host_caps2;
	if (pdata->pm_caps)
		host->mmc->pm_caps |= pdata->pm_caps;
	pdev->dev.platform_data = pdata;

	if (host->mmc->pm_caps)
		host->mmc->pm_flags |= host->mmc->pm_caps;

	if (!(host->mmc->caps2 & MMC_CAP2_NO_MMC)) {
		host->mmc_host_ops.hs400_prepare_ddr = spacemit_sdhci_pre_select_hs400;
		host->mmc_host_ops.hs400_complete = spacemit_sdhci_post_select_hs400;
		host->mmc_host_ops.hs400_downgrade = spacemit_sdhci_pre_hs400_to_hs200;
		if (host->mmc->caps2 & MMC_CAP2_HS400_ES)
			host->mmc_host_ops.hs400_enhanced_strobe = spacemit_sdhci_hs400_enhanced_strobe;
	}

	host->mmc_host_ops.start_signal_voltage_switch = spacemit_sdhci_start_signal_voltage_switch;
	host->mmc_host_ops.card_busy = spacemit_sdhci_card_busy;
	host->mmc_host_ops.init_card = spacemit_init_card_quriks;
	host->mmc_host_ops.enable_sdio_irq = spacemit_enable_sdio_irq;

	if (!(host->mmc->caps2 & MMC_CAP2_NO_SDIO)) {
		/* skip auto rescan */
		host->mmc->rescan_entered = 1;
	}
#if BOOTPART_NOACC_DEFAULT
	if (!(host->mmc->caps2 & MMC_CAP2_NO_MMC) && !is_recovery_boot)
		host->mmc->caps2 |= MMC_CAP2_BOOTPART_NOACC;
#endif
	host->mmc->caps |= MMC_CAP_NEED_RSP_BUSY;

	pm_runtime_get_noresume(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_set_autosuspend_delay(&pdev->dev, RPM_DELAY);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_suspend_ignore_children(&pdev->dev, 1);
	pm_runtime_get_sync(&pdev->dev);

	/* set io clock rate */
	if (pdata->host_freq) {
		ret = clk_set_rate(spacemit->clk_io, pdata->host_freq);
		if (ret) {
			dev_err(dev, "failed to set io clock freq\n");
			goto err_add_host;
		}
	} else {
		dev_err(dev, "failed to get io clock freq\n");
		goto err_add_host;
	}

	ret = sdhci_add_host(host);
	if (ret) {
		dev_err(&pdev->dev, "failed to add spacemit sdhc.\n");
		goto err_add_host;
	} else {
		if (!(host->mmc->caps2 & MMC_CAP2_NO_SDIO)) {
			pr_notice("sdio: save sdio_host <- %p\n", host);
			sdio_host = host;
		}
	}

	spacemit_sdhci_caps_disable(host);

	if ((host->mmc->caps2 & MMC_CAP2_NO_MMC) || (host->quirks2 & SDHCI_QUIRK2_BROKEN_PHY_MODULE)) {
		pr_debug("%s: get card pinctrl\n", mmc_hostname(host->mmc));
		spacemit->pinctrl = devm_pinctrl_get(&pdev->dev);
	}
	if (host->mmc->caps2 & MMC_CAP2_NO_MMC) {
#ifdef CONFIG_SYSFS
		for (i = 0; i < ARRAY_SIZE(sdhci_sysfs_files); i++) {
			device_create_file(dev, &sdhci_sysfs_files[i]);
		}
#endif
	}

	if (host->mmc->pm_caps & MMC_PM_WAKE_SDIO_IRQ)
		device_init_wakeup(&pdev->dev, 1);
	pm_runtime_put_autosuspend(&pdev->dev);
	return 0;

err_add_host:
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
err_of_parse:
	reset_control_assert(spacemit->reset);
err_rst_get:
	if (!IS_ERR(spacemit->clk_aib))
		clk_disable_unprepare(spacemit->clk_aib);
	clk_disable_unprepare(spacemit->clk_io);
	clk_disable_unprepare(spacemit->clk_core);
err_clk_get:
	sdhci_pltfm_free(pdev);
	return ret;
}

static int spacemit_sdhci_remove(struct platform_device *pdev)
{
	struct sdhci_host *host = platform_get_drvdata(pdev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_spacemit *spacemit = sdhci_pltfm_priv(pltfm_host);
	int i;

	pm_runtime_get_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
	sdhci_remove_host(host, 1);

	reset_control_assert(spacemit->reset);
	if (!IS_ERR(spacemit->clk_aib))
		clk_disable_unprepare(spacemit->clk_aib);
	clk_disable_unprepare(spacemit->clk_io);
	clk_disable_unprepare(spacemit->clk_core);

	if (!(host->mmc->caps2 & MMC_CAP2_NO_SD)) {
#ifdef CONFIG_SYSFS
		for (i = 0; i < ARRAY_SIZE(sdhci_sysfs_files); i++) {
			device_remove_file(&pdev->dev, &sdhci_sysfs_files[i]);
		}
#endif
	}

	sdhci_pltfm_free(pdev);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int spacemit_sdhci_suspend(struct device *dev)
{
	int ret;
	struct sdhci_host *host = dev_get_drvdata(dev);

	pm_runtime_get_sync(dev);
	ret = sdhci_suspend_host(host);
	if (ret)
		return ret;
	ret = pm_runtime_force_suspend(dev);
	return ret;
}

static int spacemit_sdhci_resume(struct device *dev)
{
	int ret;
	struct sdhci_host *host = dev_get_drvdata(dev);

	ret = pm_runtime_force_resume(dev);
	if (ret) {
		dev_err(dev, "failed to resume pm_runtime (%d)\n", ret);
		return ret;
	}
	ret = sdhci_resume_host(host);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	if (ret) {
		dev_err(dev, "failed to resume (%d)\n", ret);
		return ret;
	}
	return 0;
}
#endif

#ifdef CONFIG_PM
static int spacemit_sdhci_runtime_suspend(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_spacemit *spacemit = sdhci_pltfm_priv(pltfm_host);
	unsigned long flags;
	u32 reg;

	spin_lock_irqsave(&host->lock, flags);
	host->runtime_suspended = true;
	if (!(host->mmc->caps2 & MMC_CAP2_NO_MMC)
		&& !(host->quirks2 & SDHCI_QUIRK2_BROKEN_PHY_MODULE)
		&& !(host->quirks2 & SDHCI_QUIRK2_SUPPORT_PHY_BYPASS)) {
		reg = sdhci_readl(host, SDHC_PHY_CTRL_REG);
		reg &= ~PHY_FUNC_EN;
		sdhci_writel(host, reg, SDHC_PHY_CTRL_REG);
	}
	spin_unlock_irqrestore(&host->lock, flags);

	clk_disable_unprepare(spacemit->clk_io);
	if (!IS_ERR(spacemit->clk_aib))
		clk_disable_unprepare(spacemit->clk_aib);
	if (!IS_ERR(spacemit->clk_core))
		clk_disable_unprepare(spacemit->clk_core);

	return 0;
}

static int spacemit_sdhci_runtime_resume(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_spacemit *spacemit = sdhci_pltfm_priv(pltfm_host);
	unsigned long flags;
	u32 reg;

	clk_prepare_enable(spacemit->clk_io);
	if (!IS_ERR(spacemit->clk_aib))
		clk_prepare_enable(spacemit->clk_aib);
	if (!IS_ERR(spacemit->clk_core))
		clk_prepare_enable(spacemit->clk_core);

	spin_lock_irqsave(&host->lock, flags);
	if (!(host->mmc->caps2 & MMC_CAP2_NO_MMC)
		&& !(host->quirks2 & SDHCI_QUIRK2_BROKEN_PHY_MODULE)
		&& !(host->quirks2 & SDHCI_QUIRK2_SUPPORT_PHY_BYPASS)) {
		reg = sdhci_readl(host, SDHC_PHY_CTRL_REG);
		reg |= PHY_FUNC_EN;
		sdhci_writel(host, reg, SDHC_PHY_CTRL_REG);
	}
	host->runtime_suspended = false;
	spin_unlock_irqrestore(&host->lock, flags);

	return 0;
}

static const struct dev_pm_ops sdhci_spacemit_pmops = {
	SET_SYSTEM_SLEEP_PM_OPS(spacemit_sdhci_suspend, spacemit_sdhci_resume)
	SET_RUNTIME_PM_OPS(spacemit_sdhci_runtime_suspend,
		spacemit_sdhci_runtime_resume, NULL)
};

#define SDHCI_SPACEMIT_PMOPS (&sdhci_spacemit_pmops)

#else
#define SDHCI_SPACEMIT_PMOPS NULL
#endif

static struct platform_driver spacemit_sdhci_driver = {
	.driver		= {
		.name	= "sdhci-spacemit",
		.of_match_table = of_match_ptr(sdhci_spacemit_of_match),
		.pm	= SDHCI_SPACEMIT_PMOPS,
	},
	.probe		= spacemit_sdhci_probe,
	.remove		= spacemit_sdhci_remove,
};

module_platform_driver(spacemit_sdhci_driver);

MODULE_DESCRIPTION("SDHCI platform driver for Spacemit");
MODULE_LICENSE("GPL v2");
