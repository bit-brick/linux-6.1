// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Spacemit
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/scatterlist.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/reboot.h>
#include <linux/of_device.h>
#include <linux/rpmsg.h>

#include "i2c-k1x.h"

#ifdef CONFIG_SOC_SPACEMIT_K1X

#define STARTUP_MSG		"startup"
#define IRQUP_MSG		"irqon"

struct instance_data {
	struct rpmsg_device *rpdev;
	struct spacemit_i2c_dev *spacemit_i2c;
};

static unsigned long long private_data[2];
static const struct of_device_id r_spacemit_i2c_dt_match[];
#endif

static inline u32 spacemit_i2c_read_reg(struct spacemit_i2c_dev *spacemit_i2c, int reg)
{
	return readl(spacemit_i2c->mapbase + reg);
}

static inline void
spacemit_i2c_write_reg(struct spacemit_i2c_dev *spacemit_i2c, int reg, u32 val)
{
	writel(val, spacemit_i2c->mapbase + reg);
}

static void spacemit_i2c_enable(struct spacemit_i2c_dev *spacemit_i2c)
{
	spacemit_i2c_write_reg(spacemit_i2c, REG_CR,
	spacemit_i2c_read_reg(spacemit_i2c, REG_CR) | CR_IUE);
}

static void spacemit_i2c_disable(struct spacemit_i2c_dev *spacemit_i2c)
{
	spacemit_i2c->i2c_ctrl_reg_value = spacemit_i2c_read_reg(spacemit_i2c, REG_CR) & ~CR_IUE;
	spacemit_i2c_write_reg(spacemit_i2c, REG_CR, spacemit_i2c->i2c_ctrl_reg_value);
}

static void spacemit_i2c_flush_fifo_buffer(struct spacemit_i2c_dev *spacemit_i2c)
{
	/* flush REG_WFIFO_WPTR and REG_WFIFO_RPTR */
	spacemit_i2c_write_reg(spacemit_i2c, REG_WFIFO_WPTR, 0);
	spacemit_i2c_write_reg(spacemit_i2c, REG_WFIFO_RPTR, 0);

	/* flush REG_RFIFO_WPTR and REG_RFIFO_RPTR */
	spacemit_i2c_write_reg(spacemit_i2c, REG_RFIFO_WPTR, 0);
	spacemit_i2c_write_reg(spacemit_i2c, REG_RFIFO_RPTR, 0);
}

static void spacemit_i2c_controller_reset(struct spacemit_i2c_dev *spacemit_i2c)
{
	/* i2c controller reset */
	spacemit_i2c_write_reg(spacemit_i2c, REG_CR, CR_UR);
	udelay(5);
	spacemit_i2c_write_reg(spacemit_i2c, REG_CR, 0);

	/* set load counter register */
	if (spacemit_i2c->i2c_lcr)
		spacemit_i2c_write_reg(spacemit_i2c, REG_LCR, spacemit_i2c->i2c_lcr);

	/* set wait counter register */
	if (spacemit_i2c->i2c_wcr)
		spacemit_i2c_write_reg(spacemit_i2c, REG_WCR, spacemit_i2c->i2c_wcr);
}

static void spacemit_i2c_bus_reset(struct spacemit_i2c_dev *spacemit_i2c)
{
	int clk_cnt = 0;
	u32 bus_status;

	/* if bus is locked, reset unit. 0: locked */
	bus_status = spacemit_i2c_read_reg(spacemit_i2c, REG_BMR);
	if (!(bus_status & BMR_SDA) || !(bus_status & BMR_SCL)) {
		spacemit_i2c_controller_reset(spacemit_i2c);
		usleep_range(10, 20);

		/* check scl status again */
		bus_status = spacemit_i2c_read_reg(spacemit_i2c, REG_BMR);
		if (!(bus_status & BMR_SCL))
			dev_alert(spacemit_i2c->dev, "unit reset failed\n");
	}

	while (clk_cnt < 9) {
		/* check whether the SDA is still locked by slave */
		bus_status = spacemit_i2c_read_reg(spacemit_i2c, REG_BMR);
		if (bus_status & BMR_SDA)
			break;

		/* if still locked, send one clk to slave to request release */
		spacemit_i2c_write_reg(spacemit_i2c, REG_RST_CYC, 0x1);
		spacemit_i2c_write_reg(spacemit_i2c, REG_CR, CR_RSTREQ);
		usleep_range(20, 30);
		clk_cnt++;
	}

	bus_status = spacemit_i2c_read_reg(spacemit_i2c, REG_BMR);
	if (clk_cnt >= 9 && !(bus_status & BMR_SDA))
		dev_alert(spacemit_i2c->dev, "bus reset clk reaches the max 9-clocks\n");
	else
		dev_alert(spacemit_i2c->dev, "bus reset, send clk: %d\n", clk_cnt);
}

static void spacemit_i2c_reset(struct spacemit_i2c_dev *spacemit_i2c)
{
	spacemit_i2c_controller_reset(spacemit_i2c);
}

static int spacemit_i2c_recover_bus_busy(struct spacemit_i2c_dev *spacemit_i2c)
{
	int timeout;
	int cnt, ret = 0;

	if (spacemit_i2c->high_mode)
		timeout = 1000; /* 1000us */
	else
		timeout = 1500; /* 1500us  */

	cnt = SPACEMIT_I2C_BUS_RECOVER_TIMEOUT / timeout;

	if (likely(!(spacemit_i2c_read_reg(spacemit_i2c, REG_SR) & (SR_UB | SR_IBB))))
		return 0;

	/* wait unit and bus to recover idle */
	while (unlikely(spacemit_i2c_read_reg(spacemit_i2c, REG_SR) & (SR_UB | SR_IBB))) {
		if (cnt-- <= 0)
			break;

		usleep_range(timeout / 2, timeout);
	}

	if (unlikely(cnt <= 0)) {
		/* reset controller */
		spacemit_i2c_reset(spacemit_i2c);
		ret = -EAGAIN;
	}

	return ret;
}

static void spacemit_i2c_check_bus_release(struct spacemit_i2c_dev *spacemit_i2c)
{
	/* in case bus is not released after transfer completes */
	if (unlikely(spacemit_i2c_read_reg(spacemit_i2c, REG_SR) & SR_EBB)) {
		spacemit_i2c_bus_reset(spacemit_i2c);
		usleep_range(90, 150);
	}
}

static void spacemit_i2c_unit_init(struct spacemit_i2c_dev *spacemit_i2c)
{
	u32 cr_val = 0;

	/*
	 * Unmask interrupt bits for all xfer mode:
	 * bus error, arbitration loss detected.
	 * For transaction complete signal, we use master stop
	 * interrupt, so we don't need to unmask CR_TXDONEIE.
	 */
	cr_val |= CR_BEIE | CR_ALDIE;

	if (likely(spacemit_i2c->xfer_mode == SPACEMIT_I2C_MODE_INTERRUPT))
		/*
		 * Unmask interrupt bits for interrupt xfer mode:
		 * DBR rx full.
		 * For tx empty interrupt CR_DTEIE, we only
		 * need to enable when trigger byte transfer to start
		 * data sending.
		 */
		cr_val |= CR_DRFIE;
	else if (likely(spacemit_i2c->xfer_mode == SPACEMIT_I2C_MODE_FIFO))
		/* enable i2c FIFO mode*/
		cr_val |= CR_FIFOEN;
	else if (spacemit_i2c->xfer_mode == SPACEMIT_I2C_MODE_DMA)
		/* enable i2c DMA mode*/
		cr_val |= CR_DMAEN | CR_FIFOEN;

	/* set speed bits */
	if (spacemit_i2c->fast_mode)
		cr_val |= CR_MODE_FAST;
	if (spacemit_i2c->high_mode)
		cr_val |= CR_MODE_HIGH | CR_GPIOEN;

	/* disable response to general call */
	cr_val |= CR_GCD;

	/* enable SCL clock output */
	cr_val |= CR_SCLE;

	/* enable master stop detected */
	cr_val |= CR_MSDE | CR_MSDIE;

	spacemit_i2c_write_reg(spacemit_i2c, REG_CR, cr_val);
}

static void spacemit_i2c_trigger_byte_xfer(struct spacemit_i2c_dev *spacemit_i2c)
{
	u32 cr_val = spacemit_i2c_read_reg(spacemit_i2c, REG_CR);

	/* send start pulse */
	cr_val &= ~CR_STOP;
	cr_val |= CR_START | CR_TB | CR_DTEIE;
	spacemit_i2c_write_reg(spacemit_i2c, REG_CR, cr_val);
}

static inline void
spacemit_i2c_clear_int_status(struct spacemit_i2c_dev *spacemit_i2c, u32 mask)
{
	spacemit_i2c_write_reg(spacemit_i2c, REG_SR, mask & SPACEMIT_I2C_INT_STATUS_MASK);
}

static bool spacemit_i2c_is_last_byte_to_send(struct spacemit_i2c_dev *spacemit_i2c)
{
	return (spacemit_i2c->tx_cnt == spacemit_i2c->cur_msg->len &&
		spacemit_i2c->msg_idx == spacemit_i2c->num - 1) ? true : false;
}

static bool spacemit_i2c_is_last_byte_to_receive(struct spacemit_i2c_dev *spacemit_i2c)
{
	/*
	 * if the message length is received from slave device,
	 * should at least to read out the length byte from slave.
	 */
	if (unlikely((spacemit_i2c->cur_msg->flags & I2C_M_RECV_LEN) &&
		!spacemit_i2c->smbus_rcv_len)) {
		return false;
	} else {
		return (spacemit_i2c->rx_cnt == spacemit_i2c->cur_msg->len - 1 &&
			spacemit_i2c->msg_idx == spacemit_i2c->num - 1) ? true : false;
	}
}

static void spacemit_i2c_mark_rw_flag(struct spacemit_i2c_dev *spacemit_i2c)
{
	if (spacemit_i2c->cur_msg->flags & I2C_M_RD) {
		spacemit_i2c->is_rx = true;
		spacemit_i2c->slave_addr_rw =
			((spacemit_i2c->cur_msg->addr & 0x7f) << 1) | 1;
	} else {
		spacemit_i2c->is_rx = false;
		spacemit_i2c->slave_addr_rw = (spacemit_i2c->cur_msg->addr & 0x7f) << 1;
	}
}

static void spacemit_i2c_byte_xfer_send_master_code(struct spacemit_i2c_dev *spacemit_i2c)
{
	u32 cr_val = spacemit_i2c_read_reg(spacemit_i2c, REG_CR);

	spacemit_i2c->phase = SPACEMIT_I2C_XFER_MASTER_CODE;

	spacemit_i2c_write_reg(spacemit_i2c, REG_DBR, spacemit_i2c->master_code);

	cr_val &= ~(CR_STOP | CR_ALDIE);

	/* high mode: enable gpio to let I2C core generates SCL clock */
	cr_val |= CR_GPIOEN | CR_START | CR_TB | CR_DTEIE;
	spacemit_i2c_write_reg(spacemit_i2c, REG_CR, cr_val);
}

static void spacemit_i2c_byte_xfer_send_slave_addr(struct spacemit_i2c_dev *spacemit_i2c)
{
	spacemit_i2c->phase = SPACEMIT_I2C_XFER_SLAVE_ADDR;

	/* write slave address to DBR for interrupt mode */
	spacemit_i2c_write_reg(spacemit_i2c, REG_DBR, spacemit_i2c->slave_addr_rw);

	spacemit_i2c_trigger_byte_xfer(spacemit_i2c);
}

static int spacemit_i2c_byte_xfer(struct spacemit_i2c_dev *spacemit_i2c);
static int spacemit_i2c_byte_xfer_next_msg(struct spacemit_i2c_dev *spacemit_i2c);

static int spacemit_i2c_byte_xfer_body(struct spacemit_i2c_dev *spacemit_i2c)
{
	int ret = 0;
	u8  msglen = 0;
	u32 cr_val = spacemit_i2c_read_reg(spacemit_i2c, REG_CR);

	cr_val &= ~(CR_TB | CR_ACKNAK | CR_STOP | CR_START);
	spacemit_i2c->phase = SPACEMIT_I2C_XFER_BODY;

	if (spacemit_i2c->i2c_status & SR_IRF) { /* i2c receive full */
		/* if current is transmit mode, ignore this signal */
		if (!spacemit_i2c->is_rx)
			return 0;

		/*
		 * if the message length is received from slave device,
		 * according to i2c spec, we should restrict the length size.
		 */
		if (unlikely((spacemit_i2c->cur_msg->flags & I2C_M_RECV_LEN) &&
				!spacemit_i2c->smbus_rcv_len)) {
			spacemit_i2c->smbus_rcv_len = true;
			msglen = (u8)spacemit_i2c_read_reg(spacemit_i2c, REG_DBR);
			if ((msglen == 0) ||
				(msglen > I2C_SMBUS_BLOCK_MAX)) {
				dev_err(spacemit_i2c->dev,
						"SMbus len out of range\n");
				*spacemit_i2c->msg_buf++ = 0;
				spacemit_i2c->rx_cnt = spacemit_i2c->cur_msg->len;
				cr_val |= CR_STOP | CR_ACKNAK;
				cr_val |= CR_ALDIE | CR_TB;
				spacemit_i2c_write_reg(spacemit_i2c, REG_CR, cr_val);

				return 0;
			} else {
				*spacemit_i2c->msg_buf++ = msglen;
				spacemit_i2c->cur_msg->len = msglen + 1;
				spacemit_i2c->rx_cnt++;
			}
		} else {
			if (spacemit_i2c->rx_cnt < spacemit_i2c->cur_msg->len) {
				*spacemit_i2c->msg_buf++ =
					spacemit_i2c_read_reg(spacemit_i2c, REG_DBR);
				spacemit_i2c->rx_cnt++;
			}
		}
		/* if transfer completes, ISR will handle it */
		if (spacemit_i2c->i2c_status & (SR_MSD | SR_ACKNAK))
			return 0;

		/* trigger next byte receive */
		if (spacemit_i2c->rx_cnt < spacemit_i2c->cur_msg->len) {
			/* send stop pulse for last byte of last msg */
			if (spacemit_i2c_is_last_byte_to_receive(spacemit_i2c))
				cr_val |= CR_STOP | CR_ACKNAK;

			cr_val |= CR_ALDIE | CR_TB;
			spacemit_i2c_write_reg(spacemit_i2c, REG_CR, cr_val);
		} else if (spacemit_i2c->msg_idx < spacemit_i2c->num - 1) {
			ret = spacemit_i2c_byte_xfer_next_msg(spacemit_i2c);
		} else {
			/*
			 * For this branch, we do nothing, here the receive
			 * transfer is already done, the master stop interrupt
			 * should be generated to complete this transaction.
			*/
		}
	} else if (spacemit_i2c->i2c_status & SR_ITE) { /* i2c transmit empty */
		/* MSD comes with ITE */
		if (spacemit_i2c->i2c_status & SR_MSD)
			return ret;

		if (spacemit_i2c->i2c_status & SR_RWM) { /* receive mode */
			/* if current is transmit mode, ignore this signal */
			if (!spacemit_i2c->is_rx)
				return 0;

			if (spacemit_i2c_is_last_byte_to_receive(spacemit_i2c))
				cr_val |= CR_STOP | CR_ACKNAK;

			/* trigger next byte receive */
			cr_val |= CR_ALDIE | CR_TB;

			/*
			 * Mask transmit empty interrupt to avoid useless tx
			 * interrupt signal after switch to receive mode, the
			 * next expected is receive full interrupt signal.
			 */
			cr_val &= ~CR_DTEIE;
			spacemit_i2c_write_reg(spacemit_i2c, REG_CR, cr_val);
		} else { /* transmit mode */
			/* if current is receive mode, ignore this signal */
			if (spacemit_i2c->is_rx)
				return 0;

			if (spacemit_i2c->tx_cnt < spacemit_i2c->cur_msg->len) {
				spacemit_i2c_write_reg(spacemit_i2c, REG_DBR,
						*spacemit_i2c->msg_buf++);
				spacemit_i2c->tx_cnt++;

				/* send stop pulse for last byte of last msg */
				if (spacemit_i2c_is_last_byte_to_send(spacemit_i2c))
					cr_val |= CR_STOP;

				cr_val |= CR_ALDIE | CR_TB;
				spacemit_i2c_write_reg(spacemit_i2c, REG_CR, cr_val);
			} else if (spacemit_i2c->msg_idx < spacemit_i2c->num - 1) {
				ret = spacemit_i2c_byte_xfer_next_msg(spacemit_i2c);
			} else {
				/*
				 * For this branch, we do nothing, here the
				 * sending transfer is already done, the master
				 * stop interrupt should be generated to
				 * complete this transaction.
				*/
			}
		}
	}

	return ret;
}

static int spacemit_i2c_byte_xfer_next_msg(struct spacemit_i2c_dev *spacemit_i2c)
{
	if (spacemit_i2c->msg_idx == spacemit_i2c->num - 1)
		return 0;

	spacemit_i2c->msg_idx++;
	spacemit_i2c->cur_msg = spacemit_i2c->msgs + spacemit_i2c->msg_idx;
	spacemit_i2c->msg_buf = spacemit_i2c->cur_msg->buf;
	spacemit_i2c->rx_cnt = 0;
	spacemit_i2c->tx_cnt = 0;
	spacemit_i2c->i2c_err = 0;
	spacemit_i2c->i2c_status = 0;
	spacemit_i2c->smbus_rcv_len = false;
	spacemit_i2c->phase = SPACEMIT_I2C_XFER_IDLE;

	spacemit_i2c_mark_rw_flag(spacemit_i2c);

	return spacemit_i2c_byte_xfer(spacemit_i2c);
}

static void spacemit_i2c_fifo_xfer_fill_buffer(struct spacemit_i2c_dev *spacemit_i2c)
{
	int finish, count = 0, fill = 0;
	u32 data = 0;
	u32 data_buf[SPACEMIT_I2C_TX_FIFO_DEPTH * 2];
	int data_cnt = 0, i;
	unsigned long flags;

	while (spacemit_i2c->msg_idx < spacemit_i2c->num) {
		spacemit_i2c_mark_rw_flag(spacemit_i2c);

		if (spacemit_i2c->is_rx)
			finish = spacemit_i2c->rx_cnt;
		else
			finish = spacemit_i2c->tx_cnt;

		/* write master code to fifo buffer */
		if (spacemit_i2c->high_mode && spacemit_i2c->is_xfer_start) {
			data = spacemit_i2c->master_code;
			data |= WFIFO_CTRL_TB | WFIFO_CTRL_START;
			data_buf[data_cnt++] = data;

			fill += 2;
			count = min_t(size_t, spacemit_i2c->cur_msg->len - finish,
					SPACEMIT_I2C_TX_FIFO_DEPTH - fill);
		} else {
			fill += 1;
			count = min_t(size_t, spacemit_i2c->cur_msg->len - finish,
					SPACEMIT_I2C_TX_FIFO_DEPTH - fill);
		}

		spacemit_i2c->is_xfer_start = false;
		fill += count;
		data = spacemit_i2c->slave_addr_rw;
		data |= WFIFO_CTRL_TB | WFIFO_CTRL_START;

		/* write slave address to fifo buffer */
		data_buf[data_cnt++] = data;

		if (spacemit_i2c->is_rx) {
			spacemit_i2c->rx_cnt += count;

			if (spacemit_i2c->rx_cnt == spacemit_i2c->cur_msg->len &&
				spacemit_i2c->msg_idx == spacemit_i2c->num - 1)
				count -= 1;

			while (count > 0) {
				data = *spacemit_i2c->msg_buf | WFIFO_CTRL_TB;
				data_buf[data_cnt++] = data;
				spacemit_i2c->msg_buf++;
				count--;
			}

			if (spacemit_i2c->rx_cnt == spacemit_i2c->cur_msg->len &&
				spacemit_i2c->msg_idx == spacemit_i2c->num - 1) {
				data = *spacemit_i2c->msg_buf++;
				data = spacemit_i2c->slave_addr_rw | WFIFO_CTRL_TB |
					WFIFO_CTRL_STOP | WFIFO_CTRL_ACKNAK;
				data_buf[data_cnt++] = data;
			}
		} else {
			spacemit_i2c->tx_cnt += count;
			if (spacemit_i2c_is_last_byte_to_send(spacemit_i2c))
				count -= 1;

			while (count > 0) {
				data = *spacemit_i2c->msg_buf | WFIFO_CTRL_TB;
				data_buf[data_cnt++] = data;
				spacemit_i2c->msg_buf++;
				count--;
			}
			if (spacemit_i2c_is_last_byte_to_send(spacemit_i2c)) {
				data = *spacemit_i2c->msg_buf | WFIFO_CTRL_TB |
						WFIFO_CTRL_STOP;
				data_buf[data_cnt++] = data;
			}
		}

		if (spacemit_i2c->tx_cnt == spacemit_i2c->cur_msg->len ||
			spacemit_i2c->rx_cnt == spacemit_i2c->cur_msg->len) {
			spacemit_i2c->msg_idx++;
			if (spacemit_i2c->msg_idx == spacemit_i2c->num)
				break;

			spacemit_i2c->cur_msg = spacemit_i2c->msgs + spacemit_i2c->msg_idx;
			spacemit_i2c->msg_buf = spacemit_i2c->cur_msg->buf;
			spacemit_i2c->rx_cnt = 0;
			spacemit_i2c->tx_cnt = 0;
		}

		if (fill == SPACEMIT_I2C_TX_FIFO_DEPTH)
			break;
	}

	spin_lock_irqsave(&spacemit_i2c->fifo_lock, flags);
	for (i = 0; i < data_cnt; i++)
		spacemit_i2c_write_reg(spacemit_i2c, REG_WFIFO, data_buf[i]);
	spin_unlock_irqrestore(&spacemit_i2c->fifo_lock, flags);
}

static void spacemit_i2c_fifo_xfer_copy_buffer(struct spacemit_i2c_dev *spacemit_i2c)
{
	int idx = 0, cnt = 0;
	struct i2c_msg *msg;

	/* copy the rx FIFO buffer to msg */
	while (idx < spacemit_i2c->num) {
		msg = spacemit_i2c->msgs + idx;
		if (msg->flags & I2C_M_RD) {
			cnt = msg->len;
			while (cnt > 0) {
				*(msg->buf + msg->len - cnt)
					= spacemit_i2c_read_reg(spacemit_i2c, REG_RFIFO);
				cnt--;
			}
		}
		idx++;
	}
}

static int spacemit_i2c_fifo_xfer(struct spacemit_i2c_dev *spacemit_i2c)
{
	int ret = 0;
	unsigned long time_left;

	spacemit_i2c_fifo_xfer_fill_buffer(spacemit_i2c);

	time_left = wait_for_completion_timeout(&spacemit_i2c->complete,
					spacemit_i2c->timeout);
	if (unlikely(time_left == 0)) {
		dev_alert(spacemit_i2c->dev, "fifo transfer timeout\n");
		spacemit_i2c_bus_reset(spacemit_i2c);
		ret = -ETIMEDOUT;
		goto err_out;
	}

	if (unlikely(spacemit_i2c->i2c_err)) {
		ret = -1;
		spacemit_i2c_flush_fifo_buffer(spacemit_i2c);
		goto err_out;
	}

	spacemit_i2c_fifo_xfer_copy_buffer(spacemit_i2c);

err_out:
	return ret;
}

static void spacemit_i2c_dma_copy_buffer(struct spacemit_i2c_dev *spacemit_i2c)
{
	int idx = 0, total = 0, i, cnt = 0;
	struct i2c_msg *cur_msg;

	/* calculate total rx bytes */
	while (idx < spacemit_i2c->num) {
		if ((spacemit_i2c->msgs + idx)->flags & I2C_M_RD)
			total += (spacemit_i2c->msgs + idx)->len;
		idx++;
	}

	idx = 0;
	total -= total % SPACEMIT_I2C_RX_FIFO_DEPTH;
	while (idx < spacemit_i2c->num) {
		cur_msg = spacemit_i2c->msgs + idx;
		if (cur_msg->flags & I2C_M_RD) {
			for (i = 0; i < cur_msg->len; i++) {
				if (cnt < total) {
					*(cur_msg->buf + i) = spacemit_i2c->rx_dma_buf[cnt];
				} else {
					/* copy the rest bytes from FIFO  */
					*(cur_msg->buf + i) =
					spacemit_i2c_read_reg(spacemit_i2c, REG_RFIFO) &
					0xff;
				}
				cnt++;
			}
		}
		idx++;
	}
}

static void spacemit_i2c_dma_callback(void *data)
{
	return;
}

static int
spacemit_i2c_map_rx_sg(struct spacemit_i2c_dev *spacemit_i2c, int rx_nents, int *rx_total)
{
	int len;
	int rx_buf_start = *rx_total;

	*rx_total += spacemit_i2c->cur_msg->len;
	if (*rx_total < spacemit_i2c->rx_total) {
		len = spacemit_i2c->cur_msg->len;
	} else {
		len = spacemit_i2c->cur_msg->len - *rx_total + spacemit_i2c->rx_total;
		spacemit_i2c->rx_total = 0;
	}
	sg_set_buf(spacemit_i2c->rx_sg + rx_nents, &(spacemit_i2c->rx_dma_buf[rx_buf_start]), len);

	return dma_map_sg(spacemit_i2c->dev, spacemit_i2c->rx_sg + rx_nents,
			1, DMA_FROM_DEVICE);
}

static int spacemit_i2c_dma_xfer(struct spacemit_i2c_dev *spacemit_i2c)
{
	struct dma_async_tx_descriptor *tx_des = NULL, *rx_des = NULL;
	dma_cookie_t rx_ck = 0, tx_ck;
	u32 rx_nents = 0, tx_nents = 0, data;
	int ret = 0, idx = 0, count = 0, start = 0, i;
	unsigned long time_left;
	int rx_total = 0;
	int comp_timeout = 1000000; /* (us) */

	spacemit_i2c->rx_total -= spacemit_i2c->rx_total % SPACEMIT_I2C_RX_FIFO_DEPTH;
	while (idx < spacemit_i2c->num) {
		spacemit_i2c->msg_idx = idx;
		spacemit_i2c->cur_msg = spacemit_i2c->msgs + idx;
		spacemit_i2c_mark_rw_flag(spacemit_i2c);

		if (idx == 0 && spacemit_i2c->high_mode) {
			/* fill master code */
			data = (spacemit_i2c->master_code & 0xff) |
				WFIFO_CTRL_TB | WFIFO_CTRL_START;
			*(spacemit_i2c->tx_dma_buf + count) = data;
			count++;
		}
		/* fill slave address */
		data = spacemit_i2c->slave_addr_rw |
				WFIFO_CTRL_TB | WFIFO_CTRL_START;
		*(spacemit_i2c->tx_dma_buf + count) = data;
		count++;

		if (spacemit_i2c->is_rx) {
			if (spacemit_i2c->rx_total) {
				ret = spacemit_i2c_map_rx_sg(spacemit_i2c,
						rx_nents, &rx_total);
				if (!ret) {
					dev_err(spacemit_i2c->dev,
						"failed to map scatterlist\n");
					ret = -EINVAL;
					goto err_map;
				}

				rx_nents++;
			}

			for (i = 0; i < spacemit_i2c->cur_msg->len - 1; i++) {
				data = spacemit_i2c->slave_addr_rw | WFIFO_CTRL_TB;
				*(spacemit_i2c->tx_dma_buf + count) = data;
				count++;
			}
			data = spacemit_i2c->slave_addr_rw | WFIFO_CTRL_TB;

			/* send nak and stop pulse for last msg */
			if (idx == spacemit_i2c->num - 1)
				data |= WFIFO_CTRL_ACKNAK | WFIFO_CTRL_STOP;
			*(spacemit_i2c->tx_dma_buf + count++) = data;
			start += spacemit_i2c->cur_msg->len;
		} else {
			for (i = 0; i < spacemit_i2c->cur_msg->len - 1; i++) {
				data = *(spacemit_i2c->cur_msg->buf + i) |
					WFIFO_CTRL_TB;
				*(spacemit_i2c->tx_dma_buf + count) = data;
				count++;
			}
			data = *(spacemit_i2c->cur_msg->buf + i) | WFIFO_CTRL_TB;

			/* send stop pulse for last msg */
			if (idx == spacemit_i2c->num - 1)
				data |= WFIFO_CTRL_STOP;
			*(spacemit_i2c->tx_dma_buf + count++) = data;
		}
		idx++;
	}

	sg_set_buf(spacemit_i2c->tx_sg, spacemit_i2c->tx_dma_buf,
			count * sizeof(spacemit_i2c->tx_dma_buf[0]));
	ret = dma_map_sg(spacemit_i2c->dev, spacemit_i2c->tx_sg, 1, DMA_TO_DEVICE);
	if (unlikely(!ret)) {
		dev_err(spacemit_i2c->dev, "failed to map scatterlist\n");
		ret = -EINVAL;
		goto err_map;
	}

	tx_nents++;
	tx_des = dmaengine_prep_slave_sg(spacemit_i2c->tx_dma, spacemit_i2c->tx_sg, 1,
				      DMA_MEM_TO_DEV,
				      DMA_PREP_INTERRUPT | DMA_PREP_FENCE);
	if (unlikely(!tx_des)) {
		dev_err(spacemit_i2c->dev, "failed to get dma tx descriptor\n");
		ret = -EINVAL;
		goto err_desc;
	}

	tx_des->callback = spacemit_i2c_dma_callback;
	tx_des->callback_param = spacemit_i2c;

	tx_ck = dmaengine_submit(tx_des);
	if (unlikely(dma_submit_error(tx_ck))) {
		ret = -EINVAL;
		goto err_desc;
	}

	if (likely(rx_nents)) {
		rx_des = dmaengine_prep_slave_sg(spacemit_i2c->rx_dma,
						spacemit_i2c->rx_sg,
						rx_nents, DMA_DEV_TO_MEM,
						DMA_PREP_INTERRUPT);
		if (unlikely(!rx_des)) {
			dev_err(spacemit_i2c->dev,
				"failed to get dma rx descriptor\n");
			ret = -EINVAL;
			goto err_desc;
		}

		rx_des->callback = spacemit_i2c_dma_callback;
		rx_des->callback_param = spacemit_i2c;
		rx_ck = dmaengine_submit(rx_des);
		if (unlikely(dma_submit_error(rx_ck))) {
			dev_err(spacemit_i2c->dev,
				"failed to submit rx channel\n");
			ret = -EINVAL;
			goto err_desc;
		}

		dma_async_issue_pending(spacemit_i2c->rx_dma);
	}

	dma_async_issue_pending(spacemit_i2c->tx_dma);

	time_left = wait_for_completion_timeout(&spacemit_i2c->complete,
						spacemit_i2c->timeout);
	if (unlikely(time_left == 0)) {
		dev_alert(spacemit_i2c->dev, "dma transfer timeout\n");
		spacemit_i2c_bus_reset(spacemit_i2c);
		spacemit_i2c_reset(spacemit_i2c);
		ret = -ETIMEDOUT;
		comp_timeout = 0;
		goto finish;
	}

	if (unlikely(spacemit_i2c->i2c_err)) {
		ret = -1;
		spacemit_i2c_flush_fifo_buffer(spacemit_i2c);
		comp_timeout = 0;
	}

finish:
	/*
	 * wait for the rx DMA to complete, for tx, we use the i2c
	 * TXDONE/STOP interrupt, here we already receive the
	 * TXDONE/STOP signal.
	 */
	if (unlikely(rx_nents && dma_async_is_tx_complete(spacemit_i2c->rx_dma,
				rx_ck, NULL, NULL) != DMA_COMPLETE)) {
		int timeout = comp_timeout;

		while (timeout > 0) {
			if (dma_async_is_tx_complete(spacemit_i2c->rx_dma,
				rx_ck, NULL, NULL) != DMA_COMPLETE) {
				usleep_range(2, 4);
				timeout -= 4;
			} else
				break;
		}
		if (timeout <= 0) {
			dmaengine_pause(spacemit_i2c->rx_dma);
			if (ret >= 0) {
				ret = -1;
				dev_err(spacemit_i2c->dev,
					"dma rx channel timeout\n");
			}
		}
	}

	if (likely(ret >= 0))
		spacemit_i2c_dma_copy_buffer(spacemit_i2c);

err_desc:
	dma_unmap_sg(spacemit_i2c->dev, spacemit_i2c->tx_sg, tx_nents, DMA_TO_DEVICE);
err_map:
	if (likely(rx_nents))
		dma_unmap_sg(spacemit_i2c->dev, spacemit_i2c->rx_sg,
				rx_nents, DMA_FROM_DEVICE);

	/* make sure terminate transfers and free descriptors */
	if (tx_des)
		dmaengine_terminate_all(spacemit_i2c->tx_dma);

	if (rx_des)
		dmaengine_terminate_all(spacemit_i2c->rx_dma);

	return ret < 0 ? ret : 0;
}

static int spacemit_i2c_byte_xfer(struct spacemit_i2c_dev *spacemit_i2c)
{
	int ret = 0;

	/* i2c error occurs */
	if (unlikely(spacemit_i2c->i2c_err))
		return -1;

	if (spacemit_i2c->phase == SPACEMIT_I2C_XFER_IDLE) {
		if (spacemit_i2c->high_mode && spacemit_i2c->is_xfer_start)
			spacemit_i2c_byte_xfer_send_master_code(spacemit_i2c);
		else
			spacemit_i2c_byte_xfer_send_slave_addr(spacemit_i2c);

		spacemit_i2c->is_xfer_start = false;
	} else if (spacemit_i2c->phase == SPACEMIT_I2C_XFER_MASTER_CODE) {
		spacemit_i2c_byte_xfer_send_slave_addr(spacemit_i2c);
	} else {
		ret = spacemit_i2c_byte_xfer_body(spacemit_i2c);
	}

	return ret;
}

static void spacemit_i2c_print_msg_info(struct spacemit_i2c_dev *spacemit_i2c)
{
	int i, j, idx;
	char printbuf[512];

	idx = sprintf(printbuf, "msgs: %d, mode: %d", spacemit_i2c->num,
					spacemit_i2c->xfer_mode);
	for (i = 0; i < spacemit_i2c->num && i < sizeof(printbuf) / 128; i++) {
		u16 len = spacemit_i2c->msgs[i].len & 0xffff;

		idx += sprintf(printbuf + idx, ", addr: %02x",
			spacemit_i2c->msgs[i].addr);
		idx += sprintf(printbuf + idx, ", flag: %c, len: %d",
			spacemit_i2c->msgs[i].flags & I2C_M_RD ? 'R' : 'W', len);
		if (!(spacemit_i2c->msgs[i].flags & I2C_M_RD)) {
			idx += sprintf(printbuf + idx, ", data:");
			/* print at most ten bytes of data */
			for (j = 0; j < len && j < 10; j++)
				idx += sprintf(printbuf + idx, " %02x",
					spacemit_i2c->msgs[i].buf[j]);
		}
	}

}

static int spacemit_i2c_handle_err(struct spacemit_i2c_dev *spacemit_i2c)
{
	if (unlikely(spacemit_i2c->i2c_err)) {
		dev_dbg(spacemit_i2c->dev, "i2c error status: 0x%08x\n",
				spacemit_i2c->i2c_status);
		if (spacemit_i2c->i2c_err & (SR_BED  | SR_ALD))
			spacemit_i2c_reset(spacemit_i2c);

		/* try transfer again */
		if (spacemit_i2c->i2c_err & (SR_RXOV | SR_ALD)) {
			spacemit_i2c_flush_fifo_buffer(spacemit_i2c);
			return -EAGAIN;
		}
		return (spacemit_i2c->i2c_status & SR_ACKNAK) ? -ENXIO : -EIO;
	}

	return 0;
}

#ifdef CONFIG_I2C_SLAVE
static void spacemit_i2c_slave_handler(struct spacemit_i2c_dev *spacemit_i2c)
{
	u32 status = spacemit_i2c->i2c_status;
	u8 value;

	/* clear interrupt status bits[31:18]*/
	spacemit_i2c_clear_int_status(spacemit_i2c, status);

	if (unlikely(status & (SR_EBB | SR_BED))) {
		dev_err(spacemit_i2c->dev,"i2c slave bus error status = 0x%x, reset controller\n", status);
		/* controller reset */
		spacemit_i2c_controller_reset(spacemit_i2c);

		/* reinit spacemit i2c slave */
		spacemit_i2c_write_reg(spacemit_i2c, REG_CR, SPACEMIT_I2C_SLAVE_CRINIT);
		return;
	}

	/* slave address detected */
	if (status & SR_SAD) {
		/* read or write request */
		if (status & SR_RWM) {
			i2c_slave_event(spacemit_i2c->slave, I2C_SLAVE_READ_REQUESTED, &value);
			spacemit_i2c_write_reg(spacemit_i2c, REG_DBR, value & 0xff);
		} else {
			i2c_slave_event(spacemit_i2c->slave, I2C_SLAVE_WRITE_REQUESTED, &value);
		}
		spacemit_i2c_write_reg(spacemit_i2c, REG_CR, CR_TB | spacemit_i2c_read_reg(spacemit_i2c, REG_CR));
	} else if (status & SR_SSD) { /* stop detect */
		i2c_slave_event(spacemit_i2c->slave, I2C_SLAVE_STOP, &value);
		spacemit_i2c_write_reg(spacemit_i2c, REG_SR, SR_SSD);
	} else if (status & SR_IRF) { /* master write to us */
		spacemit_i2c_write_reg(spacemit_i2c, REG_SR, SR_IRF);

		value = spacemit_i2c_read_reg(spacemit_i2c, REG_DBR);
		spacemit_i2c_write_reg(spacemit_i2c, REG_CR, CR_TB | spacemit_i2c_read_reg(spacemit_i2c, REG_CR));

		i2c_slave_event(spacemit_i2c->slave, I2C_SLAVE_WRITE_RECEIVED, &value);
	} else if (status & SR_ITE) { /* ITE tx empty */
		spacemit_i2c_write_reg(spacemit_i2c, REG_SR, SR_ITE);

		i2c_slave_event(spacemit_i2c->slave, I2C_SLAVE_READ_PROCESSED, &value);
		spacemit_i2c_write_reg(spacemit_i2c, REG_DBR, value & 0xff);

		spacemit_i2c_write_reg(spacemit_i2c, REG_CR, CR_TB | spacemit_i2c_read_reg(spacemit_i2c, REG_CR));
	} else
		dev_err(spacemit_i2c->dev,"unknown slave status 0x%x\n", status);

	return;
}
#endif

static irqreturn_t spacemit_i2c_int_handler(int irq, void *devid)
{
	struct spacemit_i2c_dev *spacemit_i2c = devid;
	u32 status, ctrl;
	int ret = 0;

	/* record i2c status */
	status = spacemit_i2c_read_reg(spacemit_i2c, REG_SR);
	spacemit_i2c->i2c_status = status;

	/* check if a valid interrupt status */
	if(!status) {
		/* nothing need be done */
		return IRQ_HANDLED;
	}

#ifdef CONFIG_I2C_SLAVE
	if (spacemit_i2c->slave) {
		spacemit_i2c_slave_handler(spacemit_i2c);
		return IRQ_HANDLED;
	}
#endif

	/* bus error, rx overrun, arbitration lost */
	spacemit_i2c->i2c_err = status & (SR_BED | SR_RXOV | SR_ALD);

	/* clear interrupt status bits[31:18]*/
	spacemit_i2c_clear_int_status(spacemit_i2c, status);

	/* i2c error happens */
	if (unlikely(spacemit_i2c->i2c_err))
		goto err_out;

	/* process interrupt mode */
	if (likely(spacemit_i2c->xfer_mode == SPACEMIT_I2C_MODE_INTERRUPT))
		ret = spacemit_i2c_byte_xfer(spacemit_i2c);

err_out:
	/*
	 * send transaction complete signal:
	 * error happens, detect master stop
	 */
	if (likely(spacemit_i2c->i2c_err || (ret < 0) || (status & SR_MSD))) {
		/*
		 * Here the transaction is already done, we don't need any
		 * other interrupt signals from now, in case any interrupt
		 * happens before spacemit_i2c_xfer to disable irq and i2c unit,
		 * we mask all the interrupt signals and clear the interrupt
		 * status.
		*/
		ctrl = spacemit_i2c_read_reg(spacemit_i2c, REG_CR);
		ctrl &= ~SPACEMIT_I2C_INT_CTRL_MASK;
		spacemit_i2c_write_reg(spacemit_i2c, REG_CR, ctrl);

		spacemit_i2c_clear_int_status(spacemit_i2c, SPACEMIT_I2C_INT_STATUS_MASK);

		complete(&spacemit_i2c->complete);
	}

	return IRQ_HANDLED;
}

static void spacemit_i2c_choose_xfer_mode(struct spacemit_i2c_dev *spacemit_i2c)
{
	unsigned long timeout;
	int idx = 0, cnt = 0, freq;
	bool block = false;

	/* scan msgs */
	if (spacemit_i2c->high_mode)
		cnt++;
	spacemit_i2c->rx_total = 0;
	while (idx < spacemit_i2c->num) {
		cnt += (spacemit_i2c->msgs + idx)->len + 1;
		if ((spacemit_i2c->msgs + idx)->flags & I2C_M_RD)
			spacemit_i2c->rx_total += (spacemit_i2c->msgs + idx)->len;

		/*
		 * Some SMBus transactions require that
		 * we receive the transacttion length as the first read byte.
		 * force to use I2C_MODE_INTERRUPT
		 */
		if ((spacemit_i2c->msgs + idx)->flags & I2C_M_RECV_LEN) {
			block = true;
			cnt += I2C_SMBUS_BLOCK_MAX + 2;
		}
		idx++;
	}

	if (likely(spacemit_i2c->dma_disable) || block) {
		spacemit_i2c->xfer_mode = SPACEMIT_I2C_MODE_INTERRUPT;

#ifdef CONFIG_DEBUG_FS
	} else if (unlikely(spacemit_i2c->dbgfs_mode != SPACEMIT_I2C_MODE_INVALID)) {
		spacemit_i2c->xfer_mode = spacemit_i2c->dbgfs_mode;
		if (cnt > SPACEMIT_I2C_TX_FIFO_DEPTH &&
				spacemit_i2c->xfer_mode == SPACEMIT_I2C_MODE_FIFO)
			spacemit_i2c->xfer_mode = SPACEMIT_I2C_MODE_DMA;

		/* flush fifo buffer */
		spacemit_i2c_flush_fifo_buffer(spacemit_i2c);
#endif
	} else {
		if (likely(cnt <= SPACEMIT_I2C_TX_FIFO_DEPTH))
			spacemit_i2c->xfer_mode = SPACEMIT_I2C_MODE_FIFO;
		else
			spacemit_i2c->xfer_mode = SPACEMIT_I2C_MODE_DMA;

		/* flush fifo buffer */
		spacemit_i2c_flush_fifo_buffer(spacemit_i2c);
	}

	/*
	 * if total message length is too large to over the allocated MDA
	 * total buf length, use interrupt mode. This may happens in the
	 * syzkaller test.
	 */
	if (unlikely(cnt > (SPACEMIT_I2C_MAX_MSG_LEN * SPACEMIT_I2C_SCATTERLIST_SIZE) ||
		spacemit_i2c->rx_total > SPACEMIT_I2C_DMA_RX_BUF_LEN))
		spacemit_i2c->xfer_mode = SPACEMIT_I2C_MODE_INTERRUPT;

	/* calculate timeout */
	if (likely(spacemit_i2c->high_mode))
		freq = 1500000;
	else if (likely(spacemit_i2c->fast_mode))
		freq = 400000;
	else
		freq = 100000;

	timeout = cnt * 9 * USEC_PER_SEC / freq;

	if (likely(spacemit_i2c->xfer_mode == SPACEMIT_I2C_MODE_INTERRUPT ||
		spacemit_i2c->xfer_mode == SPACEMIT_I2C_MODE_PIO))
		timeout += (cnt - 1) * 220;

	if (spacemit_i2c->xfer_mode == SPACEMIT_I2C_MODE_INTERRUPT)
		spacemit_i2c->timeout = usecs_to_jiffies(timeout + 500000);
	else
		spacemit_i2c->timeout = usecs_to_jiffies(timeout + 100000);
}

static void spacemit_i2c_init_xfer_params(struct spacemit_i2c_dev *spacemit_i2c)
{
	/* initialize transfer parameters */
	spacemit_i2c->msg_idx = 0;
	spacemit_i2c->cur_msg = spacemit_i2c->msgs;
	spacemit_i2c->msg_buf = spacemit_i2c->cur_msg->buf;
	spacemit_i2c->rx_cnt = 0;
	spacemit_i2c->tx_cnt = 0;
	spacemit_i2c->i2c_err = 0;
	spacemit_i2c->i2c_status = 0;
	spacemit_i2c->phase = SPACEMIT_I2C_XFER_IDLE;

	/* only send master code once for high speed mode */
	spacemit_i2c->is_xfer_start = true;
}

static int spacemit_i2c_pio_xfer(struct spacemit_i2c_dev *spacemit_i2c)
{
	int ret = 0, xfer_try = 0;
	u32 status;
	signed long timeout;

xfer_retry:
	/* calculate timeout */
	spacemit_i2c_choose_xfer_mode(spacemit_i2c);
	spacemit_i2c->xfer_mode = SPACEMIT_I2C_MODE_PIO;
	timeout = jiffies_to_usecs(spacemit_i2c->timeout);

	if (!spacemit_i2c->clk_always_on)
		clk_enable(spacemit_i2c->clk);

	spacemit_i2c_controller_reset(spacemit_i2c);
	udelay(2);

	spacemit_i2c_unit_init(spacemit_i2c);

	spacemit_i2c_clear_int_status(spacemit_i2c, SPACEMIT_I2C_INT_STATUS_MASK);

	spacemit_i2c_init_xfer_params(spacemit_i2c);

	spacemit_i2c_mark_rw_flag(spacemit_i2c);

	spacemit_i2c_enable(spacemit_i2c);

	ret = spacemit_i2c_byte_xfer(spacemit_i2c);
	if (unlikely(ret < 0)) {
		ret = -EINVAL;
		goto out;
	}

	while (spacemit_i2c->num > 0 && timeout > 0) {
		status = spacemit_i2c_read_reg(spacemit_i2c, REG_SR);
		spacemit_i2c_clear_int_status(spacemit_i2c, status);
		spacemit_i2c->i2c_status = status;

		/* bus error, arbitration lost */
		spacemit_i2c->i2c_err = status & (SR_BED | SR_ALD);
		if (unlikely(spacemit_i2c->i2c_err)) {
			ret = -1;
			break;
		}

		/* receive full */
		if (likely(status & SR_IRF)) {
			ret = spacemit_i2c_byte_xfer(spacemit_i2c);
			if (unlikely(ret < 0))
				break;
		}

		/* transmit empty */
		if (likely(status & SR_ITE)) {
			ret = spacemit_i2c_byte_xfer(spacemit_i2c);
			if (unlikely(ret < 0))
				break;
		}

		/* transaction done */
		if (likely(status & SR_MSD))
			break;

		udelay(10);
		timeout -= 10;
	}

	spacemit_i2c_disable(spacemit_i2c);

	if (!spacemit_i2c->clk_always_on)
		clk_disable(spacemit_i2c->clk);

	if (unlikely(timeout <= 0)) {
		dev_alert(spacemit_i2c->dev, "i2c pio transfer timeout\n");
		spacemit_i2c_print_msg_info(spacemit_i2c);
		spacemit_i2c_bus_reset(spacemit_i2c);
		udelay(100);
		ret = -ETIMEDOUT;
		goto out;
	}

	/* process i2c error */
	if (unlikely(spacemit_i2c->i2c_err)) {
		dev_dbg(spacemit_i2c->dev, "i2c pio error status: 0x%08x\n",
			spacemit_i2c->i2c_status);
		spacemit_i2c_print_msg_info(spacemit_i2c);

		/* try transfer again */
		if (spacemit_i2c->i2c_err & SR_ALD)
			ret = -EAGAIN;
		else
			ret = (spacemit_i2c->i2c_status & SR_ACKNAK) ? -ENXIO : -EIO;
	}

out:
	xfer_try++;
	/* retry i2c transfer 3 times for timeout and bus busy */
	if (unlikely((ret == -ETIMEDOUT || ret == -EAGAIN) &&
		xfer_try <= spacemit_i2c->drv_retries)) {
		dev_alert(spacemit_i2c->dev, "i2c pio retry %d, ret %d err 0x%x\n",
				xfer_try, ret, spacemit_i2c->i2c_err);
		udelay(150);
		ret = 0;
		goto xfer_retry;
	}

	return ret < 0 ? ret : spacemit_i2c->num;
}


static bool spacemit_i2c_restart_notify = false;
static bool spacemit_i2c_poweroff_notify = false;
struct sys_off_handler *i2c_poweroff_handler;

static int
spacemit_i2c_notifier_reboot_call(struct notifier_block *nb, unsigned long action, void *data)
{
	spacemit_i2c_restart_notify = true;
	return 0;
}

static int spacemit_i2c_notifier_poweroff_call(struct sys_off_data *data)
{
	spacemit_i2c_poweroff_notify = true;

	return NOTIFY_DONE;
}

static struct notifier_block spacemit_i2c_sys_nb = {
	.notifier_call  = spacemit_i2c_notifier_reboot_call,
	.priority   = 0,
};

static int
spacemit_i2c_xfer(struct i2c_adapter *adapt, struct i2c_msg msgs[], int num)
{
	struct spacemit_i2c_dev *spacemit_i2c = i2c_get_adapdata(adapt);
	int ret = 0, xfer_try = 0;
	unsigned long time_left;
	bool clk_directly = false;

#ifdef CONFIG_I2C_SLAVE
	if (spacemit_i2c->slave) {
		dev_err(spacemit_i2c->dev, "working as slave mode here\n");
		return -EBUSY;
	}
#endif

	/*
	 * at the end of system power off sequence, system will send
	 * software power down command to pmic via i2c interface
	 * with local irq disabled, so just enter PIO mode at once
	*/
	if (unlikely(spacemit_i2c_restart_notify == true ||
		spacemit_i2c_poweroff_notify == true
#ifdef CONFIG_DEBUG_FS
		|| spacemit_i2c->dbgfs_mode == SPACEMIT_I2C_MODE_PIO
#endif
		)) {

		spacemit_i2c->msgs = msgs;
		spacemit_i2c->num = num;

		return spacemit_i2c_pio_xfer(spacemit_i2c);
	}

	mutex_lock(&spacemit_i2c->mtx);
	spacemit_i2c->msgs = msgs;
	spacemit_i2c->num = num;

	if (spacemit_i2c->shutdown) {
		mutex_unlock(&spacemit_i2c->mtx);
		return -ENXIO;
	}

	if (!spacemit_i2c->clk_always_on) {
		ret = pm_runtime_get_sync(spacemit_i2c->dev);
		if (unlikely(ret < 0)) {
			/*
			 * during system suspend_late to system resume_early stage,
			 * if PM runtime is suspended, we will get -EACCES return
			 * value, so we need to enable clock directly, and disable after
			 * i2c transfer is finished, if PM runtime is active, it will
			 * work normally. During this stage, pmic onkey ISR that
			 * invoked in an irq thread may use i2c interface if we have
			 * onkey press action
			 */
			if (likely(ret == -EACCES)) {
				clk_directly = true;
				clk_enable(spacemit_i2c->clk);
			} else {
				dev_err(spacemit_i2c->dev, "pm runtime sync error: %d\n",
					ret);
				goto err_runtime;
			}
		}
	}

xfer_retry:
	/* if unit keeps the last control status, don't need to do reset */
	if (unlikely(spacemit_i2c_read_reg(spacemit_i2c, REG_CR) != spacemit_i2c->i2c_ctrl_reg_value))
		/* i2c controller & bus reset */
		spacemit_i2c_reset(spacemit_i2c);

	/* choose transfer mode */
	spacemit_i2c_choose_xfer_mode(spacemit_i2c);

	/* i2c unit init */
	spacemit_i2c_unit_init(spacemit_i2c);

	/* clear all interrupt status */
	spacemit_i2c_clear_int_status(spacemit_i2c, SPACEMIT_I2C_INT_STATUS_MASK);

	spacemit_i2c_init_xfer_params(spacemit_i2c);

	spacemit_i2c_mark_rw_flag(spacemit_i2c);

	reinit_completion(&spacemit_i2c->complete);

	spacemit_i2c_enable(spacemit_i2c);
	enable_irq(spacemit_i2c->irq);

	/* i2c wait for bus busy */
	ret = spacemit_i2c_recover_bus_busy(spacemit_i2c);
	if (unlikely(ret))
		goto err_recover;

	/* i2c msg transmit */
	if (likely(spacemit_i2c->xfer_mode == SPACEMIT_I2C_MODE_INTERRUPT))
		ret = spacemit_i2c_byte_xfer(spacemit_i2c);
	else if (likely(spacemit_i2c->xfer_mode == SPACEMIT_I2C_MODE_FIFO))
		ret = spacemit_i2c_fifo_xfer(spacemit_i2c);
	else
		ret = spacemit_i2c_dma_xfer(spacemit_i2c);

	if (unlikely(ret < 0)) {
		dev_dbg(spacemit_i2c->dev, "i2c transfer error\n");
		/* timeout error should not be overrided, and the transfer
		 * error will be confirmed by err handle function latter,
		 * the reset should be invalid argument error. */
		if (ret != -ETIMEDOUT)
			ret = -EINVAL;
		goto err_xfer;
	}

	if (likely(spacemit_i2c->xfer_mode == SPACEMIT_I2C_MODE_INTERRUPT)) {
		time_left = wait_for_completion_timeout(&spacemit_i2c->complete,
							spacemit_i2c->timeout);
		if (unlikely(time_left == 0)) {
			dev_alert(spacemit_i2c->dev, "msg completion timeout\n");
			synchronize_irq(spacemit_i2c->irq);
			disable_irq(spacemit_i2c->irq);
			spacemit_i2c_bus_reset(spacemit_i2c);
			spacemit_i2c_reset(spacemit_i2c);
			ret = -ETIMEDOUT;
			goto timeout_xfex;
		}
	}

err_xfer:
	if (likely(!ret))
		spacemit_i2c_check_bus_release(spacemit_i2c);

err_recover:
	disable_irq(spacemit_i2c->irq);

timeout_xfex:
	/* disable spacemit i2c */
	spacemit_i2c_disable(spacemit_i2c);

	/* print more message info when error or timeout happens */
	if (unlikely(ret < 0 || spacemit_i2c->i2c_err))
		spacemit_i2c_print_msg_info(spacemit_i2c);

	/* process i2c error */
	if (unlikely(spacemit_i2c->i2c_err))
		ret = spacemit_i2c_handle_err(spacemit_i2c);

	xfer_try++;
	/* retry i2c transfer 3 times for timeout and bus busy */
	if (unlikely((ret == -ETIMEDOUT || ret == -EAGAIN) &&
		xfer_try <= spacemit_i2c->drv_retries)) {
		dev_alert(spacemit_i2c->dev, "i2c transfer retry %d, ret %d mode %d err 0x%x\n",
				xfer_try, ret, spacemit_i2c->xfer_mode, spacemit_i2c->i2c_err);
		usleep_range(150, 200);
		ret = 0;
		goto xfer_retry;
	}

err_runtime:
	if (unlikely(clk_directly)) {
		/* if clock is enabled directly, here disable it */
		clk_disable(spacemit_i2c->clk);
	}

	if (!spacemit_i2c->clk_always_on) {
		pm_runtime_mark_last_busy(spacemit_i2c->dev);
		pm_runtime_put_autosuspend(spacemit_i2c->dev);
	}

	mutex_unlock(&spacemit_i2c->mtx);

	return ret < 0 ? ret : num;
}

static int spacemit_i2c_prepare_dma(struct spacemit_i2c_dev *spacemit_i2c)
{
	int ret = 0;
	struct dma_slave_config *rx_cfg = &spacemit_i2c->rx_dma_cfg;
	struct dma_slave_config *tx_cfg = &spacemit_i2c->tx_dma_cfg;

	if (spacemit_i2c->dma_disable)
		return 0;

	/* request dma channels */
	spacemit_i2c->rx_dma = dma_request_slave_channel(spacemit_i2c->dev, "rx");
	if (IS_ERR_OR_NULL(spacemit_i2c->rx_dma)) {
		ret = -1;
		dev_err(spacemit_i2c->dev, "failed to request rx dma channel\n");
		return ret;
	}

	spacemit_i2c->tx_dma = dma_request_slave_channel(spacemit_i2c->dev, "tx");
	if (IS_ERR_OR_NULL(spacemit_i2c->tx_dma)) {
		ret = -1;
		dev_err(spacemit_i2c->dev, "failed to request tx dma channel\n");
		goto err_rxch;
	}

	rx_cfg->direction = DMA_DEV_TO_MEM;
	rx_cfg->src_addr = spacemit_i2c->resrc.start + REG_RFIFO;
	rx_cfg->device_fc = true;
	rx_cfg->src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	rx_cfg->src_maxburst = SPACEMIT_I2C_RX_FIFO_DEPTH * 1;

	ret = dmaengine_slave_config(spacemit_i2c->rx_dma, rx_cfg);
	if (ret) {
		dev_err(spacemit_i2c->dev, "failed to config rx channel\n");
		goto err_txch;
	}

	tx_cfg->direction = DMA_MEM_TO_DEV;
	tx_cfg->dst_addr = spacemit_i2c->resrc.start + REG_WFIFO;
	tx_cfg->device_fc = true;
	tx_cfg->dst_addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
	tx_cfg->dst_maxburst = SPACEMIT_I2C_TX_FIFO_DEPTH * 1;

	ret = dmaengine_slave_config(spacemit_i2c->tx_dma, tx_cfg);
	if (ret) {
		dev_err(spacemit_i2c->dev, "failed to config tx channel\n");
		goto err_txch;
	}

	/* allocate scatter lists */
	spacemit_i2c->rx_sg = devm_kmalloc(spacemit_i2c->dev,
			sizeof(*spacemit_i2c->rx_sg) * SPACEMIT_I2C_SCATTERLIST_SIZE,
			GFP_KERNEL);
	if (!spacemit_i2c->rx_sg) {
		ret = -ENOMEM;
		dev_err(spacemit_i2c->dev,
			"failed to allocate memory for rx scatterlist\n");
		goto err_txch;
	}
	sg_init_table(spacemit_i2c->rx_sg, SPACEMIT_I2C_SCATTERLIST_SIZE);

	spacemit_i2c->tx_sg = devm_kmalloc(spacemit_i2c->dev,
				sizeof(*spacemit_i2c->tx_sg),
				GFP_KERNEL);
	if (!spacemit_i2c->tx_sg) {
		ret = -ENOMEM;
		dev_err(spacemit_i2c->dev,
			"failed to allocate memory for tx scatterlist\n");
		goto err_txch;
	}
	sg_init_table(spacemit_i2c->tx_sg, 1);

	/* allocate memory for tx */
	spacemit_i2c->tx_dma_buf = devm_kzalloc(spacemit_i2c->dev,
			sizeof(spacemit_i2c->tx_dma_buf[0]) * SPACEMIT_I2C_DMA_TX_BUF_LEN,
			GFP_KERNEL);
	if (!spacemit_i2c->tx_dma_buf) {
		ret = -ENOMEM;
		dev_err(spacemit_i2c->dev,
			"failed to allocate memory for tx dma buffer\n");
		goto err_txch;
	}

	/* allocate memory for rx */
	spacemit_i2c->rx_dma_buf = devm_kzalloc(spacemit_i2c->dev,
			sizeof(spacemit_i2c->rx_dma_buf[0]) * SPACEMIT_I2C_DMA_RX_BUF_LEN,
			GFP_KERNEL);
	if (!spacemit_i2c->rx_dma_buf) {
		ret = -ENOMEM;
		dev_err(spacemit_i2c->dev,
			"failed to allocate memory for rx dma buffer\n");
		goto err_txch;
	}

	/*
	 * DMA controller can access all 4G or higher 4G address space, set
	 * dma mask will avoid to use swiotlb, that will improve performance
	 * and also avoid panic if swiotlb is not initialized.
	 * Besides, device's coherent_dma_mask is set as DMA_BIT_MASK(32)
	 * in initialization, see of_dma_configure().
	 */
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	dma_set_mask(spacemit_i2c->dev, DMA_BIT_MASK(64));
#else
	dma_set_mask(spacemit_i2c->dev, spacemit_i2c->dev->coherent_dma_mask);
#endif

	return 0;

err_txch:
	dma_release_channel(spacemit_i2c->tx_dma);
err_rxch:
	dma_release_channel(spacemit_i2c->rx_dma);
	return ret;
}

static int spacemit_i2c_release_dma(struct spacemit_i2c_dev *spacemit_i2c)
{
	if (spacemit_i2c->dma_disable)
		return 0;

	if (!IS_ERR_OR_NULL(spacemit_i2c->rx_dma))
		dma_release_channel(spacemit_i2c->rx_dma);

	if (!IS_ERR_OR_NULL(spacemit_i2c->tx_dma))
		dma_release_channel(spacemit_i2c->tx_dma);

	return 0;
}

#ifdef CONFIG_DEBUG_FS

static ssize_t
spacemit_i2c_dbgfs_read(struct file *filp, char __user *user_buf,
	size_t size, loff_t *ppos)
{
	struct spacemit_i2c_dev *spacemit_i2c = filp->private_data;
	char buf[64];
	int ret, n, copy;

	n = min(sizeof(buf) - 1, size);
	switch (spacemit_i2c->xfer_mode) {
	case SPACEMIT_I2C_MODE_INTERRUPT:
		copy = sprintf(buf, "%s: interrupt mode\n",
				spacemit_i2c->dbgfs_name);
		break;
	case SPACEMIT_I2C_MODE_FIFO:
		copy = sprintf(buf, "%s: fifo mode\n", spacemit_i2c->dbgfs_name);
		break;
	case SPACEMIT_I2C_MODE_DMA:
		copy = sprintf(buf, "%s: dma mode\n", spacemit_i2c->dbgfs_name);
		break;
	case SPACEMIT_I2C_MODE_PIO:
		copy = sprintf(buf, "%s: pio mode\n", spacemit_i2c->dbgfs_name);
		break;
	default:
		copy = sprintf(buf, "%s: mode is invalid\n",
				spacemit_i2c->dbgfs_name);
		break;
	}

	copy  = min(n, copy);
	ret = simple_read_from_buffer(user_buf, size, ppos, buf, copy);

	return ret;
}

static ssize_t
spacemit_i2c_dbgfs_write(struct file *filp, const char __user *user_buf,
	size_t size, loff_t *ppos)
{
	struct spacemit_i2c_dev *spacemit_i2c = filp->private_data;
	char buf[32];
	int buf_size, i = 0;

	buf_size = min(size, sizeof(buf) - 1);
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;

	*(buf + buf_size) = '\0';
	while (*(buf + i) != '\n' && *(buf + i) != '\0')
		i++;
	*(buf + i) = '\0';

	i = 0;
	while (*(buf + i) == ' ')
		i++;

	if (!strncmp(buf + i, "pio", 3)) {
		spacemit_i2c->dbgfs_mode = SPACEMIT_I2C_MODE_PIO;
	} else if (!strncmp(buf + i, "interrupt", 9)) {
		spacemit_i2c->dbgfs_mode = SPACEMIT_I2C_MODE_INTERRUPT;
	} else if (!strncmp(buf + i, "fifo", 4)) {
		if (!spacemit_i2c->dma_disable)
			spacemit_i2c->dbgfs_mode = SPACEMIT_I2C_MODE_FIFO;
		else
			goto err_out;
	} else if (!strncmp(buf + i, "dma", 3)) {
		if (!spacemit_i2c->dma_disable)
			spacemit_i2c->dbgfs_mode = SPACEMIT_I2C_MODE_DMA;
		else
			goto err_out;
	} else {
		if (!spacemit_i2c->dma_disable)
			dev_err(spacemit_i2c->dev,
				"only accept: interrupt, fifo, dma, pio\n");
		else
			goto err_out;
	}

	return size;

err_out:
	spacemit_i2c->dbgfs_mode = SPACEMIT_I2C_MODE_INTERRUPT;
	dev_err(spacemit_i2c->dev,
		"dma is disabled, only accept: interrupt, pio\n");
	return size;
}

static const struct file_operations spacemit_i2c_dbgfs_ops = {
	.open	= simple_open,
	.read	= spacemit_i2c_dbgfs_read,
	.write	= spacemit_i2c_dbgfs_write,
};
#endif /* CONFIG_DEBUG_FS */

#ifdef CONFIG_PM_SLEEP
/** static int spacemit_i2c_suspend(struct device *dev)
 * {
 *	struct spacemit_i2c_dev *spacemit_i2c = dev_get_drvdata(dev);
 *
 *	dev_dbg(spacemit_i2c->dev, "system suspend\n");
 *
 *	if (spacemit_i2c->clk_always_on)
 *		return 0;
 *
 *	// grab mutex to make sure the i2c transaction is over
 *	mutex_lock(&spacemit_i2c->mtx);
 *	if (!pm_runtime_status_suspended(dev)) {
 *		 // sync runtime pm and system pm states:
 *		 // prevent runtime pm suspend callback from being re-invoked
 *		pm_runtime_disable(dev);
 *		pm_runtime_set_suspended(dev);
 *		pm_runtime_enable(dev);
 *	}
 *	mutex_unlock(&spacemit_i2c->mtx);
 *
 *	return 0;
 * }
 *
 * static int spacemit_i2c_resume(struct device *dev)
 * {
 *	struct spacemit_i2c_dev *spacemit_i2c = dev_get_drvdata(dev);
 *
 *	dev_dbg(spacemit_i2c->dev, "system resume\n");
 *
 *	return 0;
 *}
 */
#endif /* CONFIG_PM_SLEEP */

/**
 * static const struct dev_pm_ops spacemit_i2c_pm_ops = {
 *	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(spacemit_i2c_suspend,
 *			spacemit_i2c_resume)
 *};
 */

static u32 spacemit_i2c_func(struct i2c_adapter *adap)
{
#ifdef CONFIG_I2C_SLAVE
	return I2C_FUNC_I2C | I2C_FUNC_SLAVE |
		(I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
#else
	return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
#endif
}

#ifdef CONFIG_I2C_SLAVE
static int spacemit_i2c_reg_slave(struct i2c_client *slave)
{
	struct spacemit_i2c_dev *spacemit_i2c = i2c_get_adapdata(slave->adapter);
	int ret = 0;

	if (spacemit_i2c->slave)
		return -EBUSY;

	if (slave->flags & I2C_CLIENT_TEN)
		return -EAFNOSUPPORT;

	if(!slave->addr) {
		dev_err(spacemit_i2c->dev, "have no slave address\n");
		return -EAFNOSUPPORT;
	}

	/* Keep device active for slave address detection logic */
	if (!spacemit_i2c->clk_always_on) {
		ret = pm_runtime_get_sync(spacemit_i2c->dev);
		if(unlikely(ret < 0)) {
			return ret;
		}
	}

	spacemit_i2c->slave = slave;

	spacemit_i2c_write_reg(spacemit_i2c, REG_SAR, slave->addr);
	spacemit_i2c_write_reg(spacemit_i2c, REG_CR, SPACEMIT_I2C_SLAVE_CRINIT);
	enable_irq(spacemit_i2c->irq);

	return 0;
}

static int spacemit_i2c_unreg_slave(struct i2c_client *slave)
{
	struct spacemit_i2c_dev *spacemit_i2c = i2c_get_adapdata(slave->adapter);

	WARN_ON(!spacemit_i2c->slave);

	disable_irq(spacemit_i2c->irq);

	spacemit_i2c_write_reg(spacemit_i2c, REG_CR, 0);
	/* clear slave address */
	spacemit_i2c_write_reg(spacemit_i2c, REG_SAR, 0);

	if (!spacemit_i2c->clk_always_on)
		pm_runtime_put(spacemit_i2c->dev);

	spacemit_i2c->slave = NULL;

	return 0;
}
#endif

static const struct i2c_algorithm spacemit_i2c_algrtm = {
	.master_xfer	= spacemit_i2c_xfer,
	.functionality	= spacemit_i2c_func,
#ifdef CONFIG_I2C_SLAVE
	.reg_slave	= spacemit_i2c_reg_slave,
	.unreg_slave	= spacemit_i2c_unreg_slave,
#endif
};

/* i2c message limitation for DMA mode */
static struct i2c_adapter_quirks spacemit_i2c_quirks = {
	.max_num_msgs	= SPACEMIT_I2C_SCATTERLIST_SIZE,
	.max_write_len	= SPACEMIT_I2C_MAX_MSG_LEN,
	.max_read_len	= SPACEMIT_I2C_MAX_MSG_LEN,
};

static int
spacemit_i2c_parse_dt(struct platform_device *pdev, struct spacemit_i2c_dev *spacemit_i2c)
{
	struct device_node *dnode = pdev->dev.of_node;
	int ret;

	/* enable fast speed mode */
	spacemit_i2c->fast_mode = of_property_read_bool(dnode, "spacemit,i2c-fast-mode");

	/* enable high speed mode */
	spacemit_i2c->high_mode = of_property_read_bool(dnode, "spacemit,i2c-high-mode");
	if (spacemit_i2c->high_mode) {
		/* get master code for high speed mode */
		ret = of_property_read_u8(dnode, "spacemit,i2c-master-code",
				&spacemit_i2c->master_code);
		if (ret) {
			spacemit_i2c->master_code = 0x0e;
			dev_warn(spacemit_i2c->dev,
			"failed to get i2c master code, use default: 0x0e\n");
		}

		ret = of_property_read_u32(dnode, "spacemit,i2c-clk-rate",
				&spacemit_i2c->clk_rate);
		if (ret) {
			dev_err(spacemit_i2c->dev,
				"failed to get i2c high mode clock rate\n");
			return ret;
		}
	}

	ret = of_property_read_u32(dnode, "spacemit,i2c-lcr", &spacemit_i2c->i2c_lcr);
	if (ret) {
		dev_err(spacemit_i2c->dev, "failed to get i2c lcr\n");
		return ret;
	}

	ret = of_property_read_u32(dnode, "spacemit,i2c-wcr", &spacemit_i2c->i2c_wcr);
	if (ret) {
		dev_err(spacemit_i2c->dev, "failed to get i2c wcr\n");
		return ret;
	}

	/*
	 * adapter device id:
	 * assigned in dt node or alias name, or automatically allocated
	 * in i2c_add_numbered_adapter()
	 */
	ret = of_property_read_u32(dnode, "spacemit,adapter-id", &pdev->id);
	if (ret)
		pdev->id = -1;

	/* disable DMA transfer mode */
	spacemit_i2c->dma_disable = of_property_read_bool(dnode, "spacemit,dma-disable");

	/* default: interrupt mode */
	if (spacemit_i2c->dma_disable)
		spacemit_i2c->xfer_mode = SPACEMIT_I2C_MODE_INTERRUPT;
	else
		spacemit_i2c->xfer_mode = SPACEMIT_I2C_MODE_DMA;

	/* true: the clock will always on and not use runtime mechanism */
	spacemit_i2c->clk_always_on = of_property_read_bool(dnode, "spacemit,clk-always-on");

	/* apb clock: 26MHz or 52MHz */
	ret = of_property_read_u32(dnode, "spacemit,apb_clock", &spacemit_i2c->apb_clock);
	if (ret) {
		dev_err(spacemit_i2c->dev, "failed to get apb clock\n");
		return ret;
	} else if ((spacemit_i2c->apb_clock != SPACEMIT_I2C_APB_CLOCK_26M) &&
			(spacemit_i2c->apb_clock != SPACEMIT_I2C_APB_CLOCK_52M)) {
		dev_err(spacemit_i2c->dev, "the apb clock should be 26M or 52M\n");
		return -EINVAL;
	}

	return 0;
}

static int spacemit_i2c_probe(struct platform_device *pdev)
{
	struct spacemit_i2c_dev *spacemit_i2c;
	struct device_node *dnode = pdev->dev.of_node;
#ifdef CONFIG_SOC_SPACEMIT_K1X
	struct rpmsg_device *rpdev;
	struct instance_data *idata;
	const struct of_device_id *of_id;
#endif
	int ret = 0;

	/* allocate memory */
	spacemit_i2c = devm_kzalloc(&pdev->dev,
				sizeof(struct spacemit_i2c_dev),
				GFP_KERNEL);
	if (!spacemit_i2c) {
		ret =  -ENOMEM;
		goto err_out;
	}

	spacemit_i2c->dev = &pdev->dev;
	platform_set_drvdata(pdev, spacemit_i2c);
	mutex_init(&spacemit_i2c->mtx);

	spacemit_i2c->resets = devm_reset_control_get_optional(&pdev->dev, NULL);
	if(IS_ERR(spacemit_i2c->resets)) {
		dev_err(&pdev->dev, "failed to get resets\n");
		goto err_out;
	}
	/* reset the i2c controller */
	reset_control_assert(spacemit_i2c->resets);
	udelay(200);
	reset_control_deassert(spacemit_i2c->resets);

	ret = spacemit_i2c_parse_dt(pdev, spacemit_i2c);
	if (ret)
		goto err_out;

	ret = of_address_to_resource(dnode, 0, &spacemit_i2c->resrc);
	if (ret) {
		dev_err(&pdev->dev, "failed to get resource\n");
		ret =  -ENODEV;
		goto err_out;
	}

	spacemit_i2c->mapbase = devm_ioremap_resource(spacemit_i2c->dev, &spacemit_i2c->resrc);
	if (IS_ERR(spacemit_i2c->mapbase)) {
		dev_err(&pdev->dev, "failed to do ioremap\n");
		ret =  PTR_ERR(spacemit_i2c->mapbase);
		goto err_out;
	}

#ifdef CONFIG_SOC_SPACEMIT_K1X
	if (of_get_property(pdev->dev.of_node, "rcpu-i2c", NULL)) {

		of_id = of_match_device(r_spacemit_i2c_dt_match, &pdev->dev);
		if (!of_id) {
			pr_err("Unable to match OF ID\n");
			return -ENODEV;
		}

		idata = (struct instance_data *)((unsigned long long *)(of_id->data))[0];
		rpdev = idata->rpdev;
		idata->spacemit_i2c = spacemit_i2c;

		ret = rpmsg_send(rpdev->ept, STARTUP_MSG, strlen(STARTUP_MSG));
		if (ret) {
			dev_err(&rpdev->dev, "rpmsg_send failed: %d\n", ret);
			return ret;
		}
	} else
#endif
	{
		spacemit_i2c->irq = platform_get_irq(pdev, 0);
		if (spacemit_i2c->irq < 0) {
			dev_err(spacemit_i2c->dev, "failed to get irq resource\n");
			ret = spacemit_i2c->irq;
			goto err_out;
		}

		ret = devm_request_irq(spacemit_i2c->dev, spacemit_i2c->irq, spacemit_i2c_int_handler,
				IRQF_NO_SUSPEND | IRQF_NO_AUTOEN,
				dev_name(spacemit_i2c->dev), spacemit_i2c);
		if (ret) {
			dev_err(spacemit_i2c->dev, "failed to request irq\n");
			goto err_out;
		}
	}

	ret = spacemit_i2c_prepare_dma(spacemit_i2c);
	if (ret) {
		dev_err(&pdev->dev, "failed to request dma channels\n");
		goto err_out;
	}

	spacemit_i2c->clk = devm_clk_get(spacemit_i2c->dev, NULL);
	if (IS_ERR(spacemit_i2c->clk)) {
		dev_err(spacemit_i2c->dev, "failed to get clock\n");
		ret = PTR_ERR(spacemit_i2c->clk);
		goto err_dma;
	}
	clk_prepare_enable(spacemit_i2c->clk);

	i2c_set_adapdata(&spacemit_i2c->adapt, spacemit_i2c);
	spacemit_i2c->adapt.owner = THIS_MODULE;
	spacemit_i2c->adapt.algo = &spacemit_i2c_algrtm;
	spacemit_i2c->adapt.dev.parent = spacemit_i2c->dev;
	spacemit_i2c->adapt.nr = pdev->id;
	/* retries used by i2c framework: 3 times */
	spacemit_i2c->adapt.retries = 3;
	/*
	 * retries used by i2c driver: 3 times
	 * this is for the very low occasionally PMIC i2c access failure.
	 */
	spacemit_i2c->drv_retries = 3;
	spacemit_i2c->adapt.dev.of_node = dnode;
	spacemit_i2c->adapt.algo_data = spacemit_i2c;
	strlcpy(spacemit_i2c->adapt.name, "spacemit-i2c-adapter",
		sizeof(spacemit_i2c->adapt.name));

	if (!spacemit_i2c->dma_disable)
		spacemit_i2c->adapt.quirks = &spacemit_i2c_quirks;

	init_completion(&spacemit_i2c->complete);
	spin_lock_init(&spacemit_i2c->fifo_lock);

	if (!spacemit_i2c->clk_always_on) {
		pm_runtime_set_autosuspend_delay(spacemit_i2c->dev, MSEC_PER_SEC);
		pm_runtime_use_autosuspend(spacemit_i2c->dev);
		pm_runtime_set_active(spacemit_i2c->dev);
		pm_suspend_ignore_children(&pdev->dev, 1);
		pm_runtime_enable(spacemit_i2c->dev);
	} else
		dev_dbg(spacemit_i2c->dev, "clock keeps always on\n");

	spacemit_i2c->dbgfs_mode = SPACEMIT_I2C_MODE_INVALID;
	spacemit_i2c->shutdown = false;
	ret = i2c_add_numbered_adapter(&spacemit_i2c->adapt);
	if (ret) {
		dev_err(spacemit_i2c->dev, "failed to add i2c adapter\n");
		goto err_clk;
	}

#ifdef CONFIG_DEBUG_FS
	snprintf(spacemit_i2c->dbgfs_name, sizeof(spacemit_i2c->dbgfs_name),
			"spacemit-i2c-%d", spacemit_i2c->adapt.nr);
	spacemit_i2c->dbgfs = debugfs_create_file(spacemit_i2c->dbgfs_name, 0644,
					NULL, spacemit_i2c, &spacemit_i2c_dbgfs_ops);
	if (!spacemit_i2c->dbgfs) {
		dev_err(spacemit_i2c->dev, "failed to create debugfs\n");
		ret = -ENOMEM;
		goto err_adapt;
	}
#endif

	dev_dbg(spacemit_i2c->dev, "driver probe success with dma %s\n",
		spacemit_i2c->dma_disable ? "disabled" : "enabled");
	return 0;

#ifdef CONFIG_DEBUG_FS
err_adapt:
	i2c_del_adapter(&spacemit_i2c->adapt);
#endif
err_clk:
	if (!spacemit_i2c->clk_always_on) {
		pm_runtime_disable(spacemit_i2c->dev);
		pm_runtime_set_suspended(spacemit_i2c->dev);
	}
	clk_disable_unprepare(spacemit_i2c->clk);
err_dma:
	spacemit_i2c_release_dma(spacemit_i2c);
err_out:
	return ret;
}

static int spacemit_i2c_remove(struct platform_device *pdev)
{
	struct spacemit_i2c_dev *spacemit_i2c = platform_get_drvdata(pdev);

	if (!spacemit_i2c->clk_always_on) {
		pm_runtime_disable(spacemit_i2c->dev);
		pm_runtime_set_suspended(spacemit_i2c->dev);
	}

	debugfs_remove_recursive(spacemit_i2c->dbgfs);
	i2c_del_adapter(&spacemit_i2c->adapt);

	mutex_destroy(&spacemit_i2c->mtx);

	reset_control_assert(spacemit_i2c->resets);

	spacemit_i2c_release_dma(spacemit_i2c);

	clk_disable_unprepare(spacemit_i2c->clk);

	dev_dbg(spacemit_i2c->dev, "driver removed\n");
	return 0;
}

static void spacemit_i2c_shutdown(struct platform_device *pdev)
{
	/**
	 * we should using i2c to communicate with pmic to shutdown the system
	 * so we should not shutdown i2c
	 */
/**
 *	struct spacemit_i2c_dev *spacemit_i2c = platform_get_drvdata(pdev);
 *
 *	mutex_lock(&spacemit_i2c->mtx);
 *	spacemit_i2c->shutdown = true;
 *	mutex_unlock(&spacemit_i2c->mtx);
 */
}

static const struct of_device_id spacemit_i2c_dt_match[] = {
	{
		.compatible = "spacemit,k1x-i2c",
	},
	{}
};

MODULE_DEVICE_TABLE(of, spacemit_i2c_dt_match);

static struct platform_driver spacemit_i2c_driver = {
	.probe  = spacemit_i2c_probe,
	.remove = spacemit_i2c_remove,
	.shutdown = spacemit_i2c_shutdown,
	.driver = {
		.name		= "i2c-spacemit-k1x",
		/* .pm             = &spacemit_i2c_pm_ops, */
		.of_match_table	= spacemit_i2c_dt_match,
	},
};

static int __init spacemit_i2c_init(void)
{
	register_restart_handler(&spacemit_i2c_sys_nb);
	i2c_poweroff_handler = register_sys_off_handler(SYS_OFF_MODE_POWER_OFF,
			SYS_OFF_PRIO_HIGH,
			spacemit_i2c_notifier_poweroff_call,
			NULL);

	return platform_driver_register(&spacemit_i2c_driver);
}

static void __exit spacemit_i2c_exit(void)
{
	platform_driver_unregister(&spacemit_i2c_driver);
	unregister_restart_handler(&spacemit_i2c_sys_nb);
	unregister_sys_off_handler(i2c_poweroff_handler);
}

subsys_initcall(spacemit_i2c_init);
module_exit(spacemit_i2c_exit);

#ifdef CONFIG_SOC_SPACEMIT_K1X
static const struct of_device_id r_spacemit_i2c_dt_match[] = {
	{ .compatible = "spacemit,k1x-i2c-rcpu", .data =(void *)&private_data[0] },
	{}
};

MODULE_DEVICE_TABLE(of, r_spacemit_i2c_dt_match);

static struct platform_driver r_spacemit_i2c_driver = {
	.probe  = spacemit_i2c_probe,
	.remove = spacemit_i2c_remove,
	.shutdown = spacemit_i2c_shutdown,
	.driver = {
		.name		= "ri2c-spacemit-k1x",
		/* .pm             = &spacemit_i2c_pm_ops, */
		.of_match_table	= r_spacemit_i2c_dt_match,
	},
};

static struct rpmsg_device_id rpmsg_driver_i2c_id_table[] = {
	{ .name	= "i2c-service", .driver_data = 0 },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_driver_i2c_id_table);

static int rpmsg_i2c_client_probe(struct rpmsg_device *rpdev)
{
	struct instance_data *idata;

	dev_info(&rpdev->dev, "new channel: 0x%x -> 0x%x!\n",
					rpdev->src, rpdev->dst);

	idata = devm_kzalloc(&rpdev->dev, sizeof(*idata), GFP_KERNEL);
	if (!idata)
		return -ENOMEM;

	dev_set_drvdata(&rpdev->dev, idata);
	idata->rpdev = rpdev;

	((unsigned long long *)(r_spacemit_i2c_dt_match[0].data))[0] = (unsigned long long)idata;

	return platform_driver_register(&r_spacemit_i2c_driver);
}

static int rpmsg_i2c_client_cb(struct rpmsg_device *rpdev, void *data,
		int len, void *priv, u32 src)
{
	int ret;
	struct instance_data *idata = dev_get_drvdata(&rpdev->dev);
	struct spacemit_i2c_dev *spacemit_i2c = idata->spacemit_i2c;

	spacemit_i2c_int_handler(0, (void *)spacemit_i2c);

        ret = rpmsg_send(rpdev->ept, IRQUP_MSG, strlen(IRQUP_MSG));
        if (ret) {
                dev_err(&rpdev->dev, "rpmsg_send failed: %d\n", ret);
                return ret;
        }

	return 0;
}

static void rpmsg_i2c_client_remove(struct rpmsg_device *rpdev)
{
	dev_info(&rpdev->dev, "rpmsg i2c client driver is removed\n");

	platform_driver_unregister(&r_spacemit_i2c_driver);
}

static struct rpmsg_driver rpmsg_i2c_client = {
	.drv.name	= KBUILD_MODNAME,
	.id_table	= rpmsg_driver_i2c_id_table,
	.probe		= rpmsg_i2c_client_probe,
	.callback	= rpmsg_i2c_client_cb,
	.remove		= rpmsg_i2c_client_remove,
};
module_rpmsg_driver(rpmsg_i2c_client);
#endif

MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:i2c-spacemit-k1x");
