/*
 * Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Copyright (c) 2010-2011 NVIDIA Corporation
 *  NVIDIA Corporation <www.nvidia.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <fdtdec.h>
#include <i2c.h>
#include <asm/io.h>
#include <asm/arch/clock.h>
#include <asm/arch/funcmux.h>
#include <asm/arch/gpio.h>
#include <asm/arch/pinmux.h>
#include <asm/arch-tegra/clk_rst.h>
#include <asm/arch-tegra/tegra_i2c.h>

DECLARE_GLOBAL_DATA_PTR;

/* Information about i2c controller */
struct i2c_bus {
	int			id;
	enum periph_id		periph_id;
	int			speed;
	int			pinmux_config;
	struct i2c_control	*control;
	struct i2c_ctlr		*regs;
	int			is_dvc;	/* DVC type, rather than I2C */
	int			is_scs;	/* single clock source (T114+) */
	int			inited;	/* bus is inited */
};

static struct i2c_bus i2c_controllers[TEGRA_I2C_NUM_CONTROLLERS];

static void set_packet_mode(struct i2c_bus *i2c_bus)
{
	u32 config;

	config = I2C_CNFG_NEW_MASTER_FSM_MASK | I2C_CNFG_PACKET_MODE_MASK;

	if (i2c_bus->is_dvc) {
		struct dvc_ctlr *dvc = (struct dvc_ctlr *)i2c_bus->regs;

		writel(config, &dvc->cnfg);
	} else {
		writel(config, &i2c_bus->regs->cnfg);
		/*
		 * program I2C_SL_CNFG.NEWSL to ENABLE. This fixes probe
		 * issues, i.e., some slaves may be wrongly detected.
		 */
		setbits_le32(&i2c_bus->regs->sl_cnfg, I2C_SL_CNFG_NEWSL_MASK);
	}
}

static void i2c_reset_controller(struct i2c_bus *i2c_bus)
{
	/* Reset I2C controller. */
	reset_periph(i2c_bus->periph_id, 1);

	/* re-program config register to packet mode */
	set_packet_mode(i2c_bus);
}

static void i2c_init_controller(struct i2c_bus *i2c_bus)
{
	/*
	 * Use PLLP - DP-04508-001_v06 datasheet indicates a divisor of 8
	 * here, in section 23.3.1, but in fact we seem to need a factor of
	 * 16 to get the right frequency.
	 */
	clock_start_periph_pll(i2c_bus->periph_id, CLOCK_ID_PERIPH,
		i2c_bus->speed * 2 * 8);

	if (i2c_bus->is_scs) {
		/*
		 * T114 I2C went to a single clock source for standard/fast and
		 * HS clock speeds. The new clock rate setting calculation is:
		 *  SCL = CLK_SOURCE.I2C /
		 *   (CLK_MULT_STD_FAST_MODE * (I2C_CLK_DIV_STD_FAST_MODE+1) *
		 *   I2C FREQUENCY DIVISOR) as per the T114 TRM (sec 30.3.1).
		 *
		 * NOTE: We do this here, after the initial clock/pll start,
		 * because if we read the clk_div reg before the controller
		 * is running, we hang, and we need it for the new calc.
		 */
		int clk_div_stdfst_mode = readl(&i2c_bus->regs->clk_div) >> 16;
		debug("%s: CLK_DIV_STD_FAST_MODE setting = %d\n", __func__,
			clk_div_stdfst_mode);

		clock_start_periph_pll(i2c_bus->periph_id, CLOCK_ID_PERIPH,
			CLK_MULT_STD_FAST_MODE * (clk_div_stdfst_mode + 1) *
			i2c_bus->speed * 2);
	}

	/* Reset I2C controller. */
	i2c_reset_controller(i2c_bus);

	/* Configure I2C controller. */
	if (i2c_bus->is_dvc) {	/* only for DVC I2C */
		struct dvc_ctlr *dvc = (struct dvc_ctlr *)i2c_bus->regs;

		setbits_le32(&dvc->ctrl3, DVC_CTRL_REG3_I2C_HW_SW_PROG_MASK);
	}

	funcmux_select(i2c_bus->periph_id, i2c_bus->pinmux_config);
}

static void send_packet_headers(
	struct i2c_bus *i2c_bus,
	struct i2c_trans_info *trans,
	u32 packet_id)
{
	u32 data;

	/* prepare header1: Header size = 0 Protocol = I2C, pktType = 0 */
	data = PROTOCOL_TYPE_I2C << PKT_HDR1_PROTOCOL_SHIFT;
	data |= packet_id << PKT_HDR1_PKT_ID_SHIFT;
	data |= i2c_bus->id << PKT_HDR1_CTLR_ID_SHIFT;
	writel(data, &i2c_bus->control->tx_fifo);
	debug("pkt header 1 sent (0x%x)\n", data);

	/* prepare header2 */
	data = (trans->num_bytes - 1) << PKT_HDR2_PAYLOAD_SIZE_SHIFT;
	writel(data, &i2c_bus->control->tx_fifo);
	debug("pkt header 2 sent (0x%x)\n", data);

	/* prepare IO specific header: configure the slave address */
	data = trans->address << PKT_HDR3_SLAVE_ADDR_SHIFT;

	/* Enable Read if it is not a write transaction */
	if (!(trans->flags & I2C_IS_WRITE))
		data |= PKT_HDR3_READ_MODE_MASK;

	/* Write I2C specific header */
	writel(data, &i2c_bus->control->tx_fifo);
	debug("pkt header 3 sent (0x%x)\n", data);
}

static int wait_for_tx_fifo_empty(struct i2c_control *control)
{
	u32 count;
	int timeout_us = I2C_TIMEOUT_USEC;

	while (timeout_us >= 0) {
		count = (readl(&control->fifo_status) & TX_FIFO_EMPTY_CNT_MASK)
				>> TX_FIFO_EMPTY_CNT_SHIFT;
		if (count == I2C_FIFO_DEPTH)
			return 1;
		udelay(10);
		timeout_us -= 10;
	}

	return 0;
}

static int wait_for_rx_fifo_notempty(struct i2c_control *control)
{
	u32 count;
	int timeout_us = I2C_TIMEOUT_USEC;

	while (timeout_us >= 0) {
		count = (readl(&control->fifo_status) & TX_FIFO_FULL_CNT_MASK)
				>> TX_FIFO_FULL_CNT_SHIFT;
		if (count)
			return 1;
		udelay(10);
		timeout_us -= 10;
	}

	return 0;
}

static int wait_for_transfer_complete(struct i2c_control *control)
{
	int int_status;
	int timeout_us = I2C_TIMEOUT_USEC;

	while (timeout_us >= 0) {
		int_status = readl(&control->int_status);
		if (int_status & I2C_INT_NO_ACK_MASK)
			return -int_status;
		if (int_status & I2C_INT_ARBITRATION_LOST_MASK)
			return -int_status;
		if (int_status & I2C_INT_XFER_COMPLETE_MASK)
			return 0;

		udelay(10);
		timeout_us -= 10;
	}

	return -1;
}

static int send_recv_packets(struct i2c_bus *i2c_bus,
			     struct i2c_trans_info *trans)
{
	struct i2c_control *control = i2c_bus->control;
	u32 int_status;
	u32 words;
	u8 *dptr;
	u32 local;
	uchar last_bytes;
	int error = 0;
	int is_write = trans->flags & I2C_IS_WRITE;

	/* clear status from previous transaction, XFER_COMPLETE, NOACK, etc. */
	int_status = readl(&control->int_status);
	writel(int_status, &control->int_status);

	send_packet_headers(i2c_bus, trans, 1);

	words = DIV_ROUND_UP(trans->num_bytes, 4);
	last_bytes = trans->num_bytes & 3;
	dptr = trans->buf;

	while (words) {
		u32 *wptr = (u32 *)dptr;

		if (is_write) {
			/* deal with word alignment */
			if ((unsigned)dptr & 3) {
				memcpy(&local, dptr, sizeof(u32));
				writel(local, &control->tx_fifo);
				debug("pkt data sent (0x%x)\n", local);
			} else {
				writel(*wptr, &control->tx_fifo);
				debug("pkt data sent (0x%x)\n", *wptr);
			}
			if (!wait_for_tx_fifo_empty(control)) {
				error = -1;
				goto exit;
			}
		} else {
			if (!wait_for_rx_fifo_notempty(control)) {
				error = -1;
				goto exit;
			}
			/*
			 * for the last word, we read into our local buffer,
			 * in case that caller did not provide enough buffer.
			 */
			local = readl(&control->rx_fifo);
			if ((words == 1) && last_bytes)
				memcpy(dptr, (char *)&local, last_bytes);
			else if ((unsigned)dptr & 3)
				memcpy(dptr, &local, sizeof(u32));
			else
				*wptr = local;
			debug("pkt data received (0x%x)\n", local);
		}
		words--;
		dptr += sizeof(u32);
	}

	if (wait_for_transfer_complete(control)) {
		error = -1;
		goto exit;
	}
	return 0;
exit:
	/* error, reset the controller. */
	i2c_reset_controller(i2c_bus);

	return error;
}

static int tegra_i2c_write_data(struct i2c_bus *bus, u32 addr, u8 *data,
				u32 len)
{
	int error;
	struct i2c_trans_info trans_info;

	trans_info.address = addr;
	trans_info.buf = data;
	trans_info.flags = I2C_IS_WRITE;
	trans_info.num_bytes = len;
	trans_info.is_10bit_address = 0;

	error = send_recv_packets(bus, &trans_info);
	if (error)
		debug("tegra_i2c_write_data: Error (%d) !!!\n", error);

	return error;
}

static int tegra_i2c_read_data(struct i2c_bus *bus, u32 addr, u8 *data,
			       u32 len)
{
	int error;
	struct i2c_trans_info trans_info;

	trans_info.address = addr | 1;
	trans_info.buf = data;
	trans_info.flags = 0;
	trans_info.num_bytes = len;
	trans_info.is_10bit_address = 0;

	error = send_recv_packets(bus, &trans_info);
	if (error)
		debug("tegra_i2c_read_data: Error (%d) !!!\n", error);

	return error;
}

#ifndef CONFIG_OF_CONTROL
#error "Please enable device tree support to use this driver"
#endif

/**
 * Check that a bus number is valid and return a pointer to it
 *
 * @param bus_num	Bus number to check / return
 * @return pointer to bus, if valid, else NULL
 */
static struct i2c_bus *tegra_i2c_get_bus(struct i2c_adapter *adap)
{
	struct i2c_bus *bus;

	bus = &i2c_controllers[adap->hwadapnr];
	if (!bus->inited) {
		debug("%s: Bus %u not available\n", __func__, adap->hwadapnr);
		return NULL;
	}

	return bus;
}

static unsigned int tegra_i2c_set_bus_speed(struct i2c_adapter *adap,
			unsigned int speed)
{
	struct i2c_bus *bus;

	bus = tegra_i2c_get_bus(adap);
	if (!bus)
		return 0;
	bus->speed = speed;
	i2c_init_controller(bus);

	return 0;
}

static int i2c_get_config(const void *blob, int node, struct i2c_bus *i2c_bus)
{
	i2c_bus->regs = (struct i2c_ctlr *)fdtdec_get_addr(blob, node, "reg");

	/*
	 * We don't have a binding for pinmux yet. Leave it out for now. So
	 * far no one needs anything other than the default.
	 */
	i2c_bus->pinmux_config = FUNCMUX_DEFAULT;
	i2c_bus->speed = fdtdec_get_int(blob, node, "clock-frequency", 0);
	i2c_bus->periph_id = clock_decode_periph_id(blob, node);

	/*
	 * We can't specify the pinmux config in the fdt, so I2C2 will not
	 * work on Seaboard. It normally has no devices on it anyway.
	 * You could add in this little hack if you need to use it.
	 * The correct solution is a pinmux binding in the fdt.
	 *
	 *	if (i2c_bus->periph_id == PERIPH_ID_I2C2)
	 *		i2c_bus->pinmux_config = FUNCMUX_I2C2_PTA;
	 */
	if (i2c_bus->periph_id == -1)
		return -FDT_ERR_NOTFOUND;

	return 0;
}

/*
 * Process a list of nodes, adding them to our list of I2C ports.
 *
 * @param blob		fdt blob
 * @param node_list	list of nodes to process (any <=0 are ignored)
 * @param count		number of nodes to process
 * @param is_dvc	1 if these are DVC ports, 0 if standard I2C
 * @param is_scs	1 if this HW uses a single clock source (T114+)
 * @return 0 if ok, -1 on error
 */
static int process_nodes(const void *blob, int node_list[], int count,
			 int is_dvc, int is_scs)
{
	struct i2c_bus *i2c_bus;
	int i;

	/* build the i2c_controllers[] for each controller */
	for (i = 0; i < count; i++) {
		int node = node_list[i];

		if (node <= 0)
			continue;

		i2c_bus = &i2c_controllers[i];
		i2c_bus->id = i;

		if (i2c_get_config(blob, node, i2c_bus)) {
			printf("i2c_init_board: failed to decode bus %d\n", i);
			return -1;
		}

		i2c_bus->is_scs = is_scs;

		i2c_bus->is_dvc = is_dvc;
		if (is_dvc) {
			i2c_bus->control =
				&((struct dvc_ctlr *)i2c_bus->regs)->control;
		} else {
			i2c_bus->control = &i2c_bus->regs->control;
		}
		debug("%s: controller bus %d at %p, periph_id %d, speed %d: ",
		      is_dvc ? "dvc" : "i2c", i, i2c_bus->regs,
		      i2c_bus->periph_id, i2c_bus->speed);
		i2c_init_controller(i2c_bus);
		debug("ok\n");
		i2c_bus->inited = 1;

		/* Mark position as used */
		node_list[i] = -1;
	}

	return 0;
}

/* Sadly there is no error return from this function */
void i2c_init_board(void)
{
	int node_list[TEGRA_I2C_NUM_CONTROLLERS];
	const void *blob = gd->fdt_blob;
	int count;

	/* First check for newer (T114+) I2C ports */
	count = fdtdec_find_aliases_for_id(blob, "i2c",
			COMPAT_NVIDIA_TEGRA114_I2C, node_list,
			TEGRA_I2C_NUM_CONTROLLERS);
	if (process_nodes(blob, node_list, count, 0, 1))
		return;

	/* Now get the older (T20/T30) normal I2C ports */
	count = fdtdec_find_aliases_for_id(blob, "i2c",
			COMPAT_NVIDIA_TEGRA20_I2C, node_list,
			TEGRA_I2C_NUM_CONTROLLERS);
	if (process_nodes(blob, node_list, count, 0, 0))
		return;

	/* Now look for dvc ports */
	count = fdtdec_add_aliases_for_id(blob, "i2c",
			COMPAT_NVIDIA_TEGRA20_DVC, node_list,
			TEGRA_I2C_NUM_CONTROLLERS);
	if (process_nodes(blob, node_list, count, 1, 0))
		return;
}

static void tegra_i2c_init(struct i2c_adapter *adap, int speed, int slaveaddr)
{
	/* This will override the speed selected in the fdt for that port */
	debug("i2c_init(speed=%u, slaveaddr=0x%x)\n", speed, slaveaddr);
	i2c_set_bus_speed(speed);
}

/* i2c write version without the register address */
int i2c_write_data(struct i2c_bus *bus, uchar chip, uchar *buffer, int len)
{
	int rc;

	debug("i2c_write_data: chip=0x%x, len=0x%x\n", chip, len);
	debug("write_data: ");
	/* use rc for counter */
	for (rc = 0; rc < len; ++rc)
		debug(" 0x%02x", buffer[rc]);
	debug("\n");

	/* Shift 7-bit address over for lower-level i2c functions */
	rc = tegra_i2c_write_data(bus, chip << 1, buffer, len);
	if (rc)
		debug("i2c_write_data(): rc=%d\n", rc);

	return rc;
}

/* i2c read version without the register address */
int i2c_read_data(struct i2c_bus *bus, uchar chip, uchar *buffer, int len)
{
	int rc;

	debug("inside i2c_read_data():\n");
	/* Shift 7-bit address over for lower-level i2c functions */
	rc = tegra_i2c_read_data(bus, chip << 1, buffer, len);
	if (rc) {
		debug("i2c_read_data(): rc=%d\n", rc);
		return rc;
	}

	debug("i2c_read_data: ");
	/* reuse rc for counter*/
	for (rc = 0; rc < len; ++rc)
		debug(" 0x%02x", buffer[rc]);
	debug("\n");

	return 0;
}

/* Probe to see if a chip is present. */
static int tegra_i2c_probe(struct i2c_adapter *adap, uchar chip)
{
	struct i2c_bus *bus;
	int rc;
	uchar reg;

	debug("i2c_probe: addr=0x%x\n", chip);
	bus = tegra_i2c_get_bus(adap);
	if (!bus)
		return 1;
	reg = 0;
	rc = i2c_write_data(bus, chip, &reg, 1);
	if (rc) {
		debug("Error probing 0x%x.\n", chip);
		return 1;
	}
	return 0;
}

static int i2c_addr_ok(const uint addr, const int alen)
{
	/* We support 7 or 10 bit addresses, so one or two bytes each */
	return alen == 1 || alen == 2;
}

/* Read bytes */
static int tegra_i2c_read(struct i2c_adapter *adap, uchar chip, uint addr,
			int alen, uchar *buffer, int len)
{
	struct i2c_bus *bus;
	uint offset;
	int i;

	debug("i2c_read: chip=0x%x, addr=0x%x, len=0x%x\n",
				chip, addr, len);
	bus = tegra_i2c_get_bus(adap);
	if (!bus)
		return 1;
	if (!i2c_addr_ok(addr, alen)) {
		debug("i2c_read: Bad address %x.%d.\n", addr, alen);
		return 1;
	}
	for (offset = 0; offset < len; offset++) {
		if (alen) {
			uchar data[alen];
			for (i = 0; i < alen; i++) {
				data[alen - i - 1] =
					(addr + offset) >> (8 * i);
			}
			if (i2c_write_data(bus, chip, data, alen)) {
				debug("i2c_read: error sending (0x%x)\n",
					addr);
				return 1;
			}
		}
		if (i2c_read_data(bus, chip, buffer + offset, 1)) {
			debug("i2c_read: error reading (0x%x)\n", addr);
			return 1;
		}
	}

	return 0;
}

/* Write bytes */
static int tegra_i2c_write(struct i2c_adapter *adap, uchar chip, uint addr,
			int alen, uchar *buffer, int len)
{
	struct i2c_bus *bus;
	uint offset;
	int i;

	debug("i2c_write: chip=0x%x, addr=0x%x, len=0x%x\n",
				chip, addr, len);
	bus = tegra_i2c_get_bus(adap);
	if (!bus)
		return 1;
	if (!i2c_addr_ok(addr, alen)) {
		debug("i2c_write: Bad address %x.%d.\n", addr, alen);
		return 1;
	}
	for (offset = 0; offset < len; offset++) {
		uchar data[alen + 1];
		for (i = 0; i < alen; i++)
			data[alen - i - 1] = (addr + offset) >> (8 * i);
		data[alen] = buffer[offset];
		if (i2c_write_data(bus, chip, data, alen + 1)) {
			debug("i2c_write: error sending (0x%x)\n", addr);
			return 1;
		}
	}

	return 0;
}

int tegra_i2c_get_dvc_bus_num(void)
{
	int i;

	for (i = 0; i < TEGRA_I2C_NUM_CONTROLLERS; i++) {
		struct i2c_bus *bus = &i2c_controllers[i];

		if (bus->inited && bus->is_dvc)
			return i;
	}

	return -1;
}

/*
 * Register soft i2c adapters
 */
U_BOOT_I2C_ADAP_COMPLETE(tegra0, tegra_i2c_init, tegra_i2c_probe,
			 tegra_i2c_read, tegra_i2c_write,
			 tegra_i2c_set_bus_speed, 100000, 0, 0)
U_BOOT_I2C_ADAP_COMPLETE(tegra1, tegra_i2c_init, tegra_i2c_probe,
			 tegra_i2c_read, tegra_i2c_write,
			 tegra_i2c_set_bus_speed, 100000, 0, 1)
U_BOOT_I2C_ADAP_COMPLETE(tegra2, tegra_i2c_init, tegra_i2c_probe,
			 tegra_i2c_read, tegra_i2c_write,
			 tegra_i2c_set_bus_speed, 100000, 0, 2)
U_BOOT_I2C_ADAP_COMPLETE(tegra3, tegra_i2c_init, tegra_i2c_probe,
			 tegra_i2c_read, tegra_i2c_write,
			 tegra_i2c_set_bus_speed, 100000, 0, 3)
