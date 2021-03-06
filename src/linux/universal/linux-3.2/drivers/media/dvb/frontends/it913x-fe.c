/*
 *  Driver for it913x-fe Frontend
 *
 *  with support for on chip it9137 integral tuner
 *
 *  Copyright (C) 2011 Malcolm Priestley (tvboxspy@gmail.com)
 *  IT9137 Copyright (C) ITE Tech Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.=
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "dvb_frontend.h"
#include "it913x-fe.h"
#include "it913x-fe-priv.h"

static int it913x_debug;

module_param_named(debug, it913x_debug, int, 0644);
MODULE_PARM_DESC(debug, "set debugging level (1=info (or-able)).");

#define dprintk(level, args...) do { \
	if (level & it913x_debug) \
		printk(KERN_DEBUG "it913x-fe: " args); \
} while (0)

#define deb_info(args...)  dprintk(0x01, args)
#define debug_data_snipet(level, name, p) \
	  dprintk(level, name" (%02x%02x%02x%02x%02x%02x%02x%02x)", \
		*p, *(p+1), *(p+2), *(p+3), *(p+4), \
			*(p+5), *(p+6), *(p+7));

struct it913x_fe_state {
	struct dvb_frontend frontend;
	struct i2c_adapter *i2c_adap;
	u8 i2c_addr;
	u32 frequency;
	u8 adf;
	u32 crystalFrequency;
	u32 adcFrequency;
	u8 tuner_type;
	struct adctable *table;
	fe_status_t it913x_status;
	u16 tun_xtal;
	u8 tun_fdiv;
	u8 tun_clk_mode;
	u32 tun_fn_min;
};

static int it913x_read_reg(struct it913x_fe_state *state,
		u32 reg, u8 *data, u8 count)
{
	int ret;
	u8 pro = PRO_DMOD; /* All reads from demodulator */
	u8 b[4];
	struct i2c_msg msg[2] = {
		{ .addr = state->i2c_addr + (pro << 1), .flags = 0,
			.buf = b, .len = sizeof(b) },
		{ .addr = state->i2c_addr + (pro << 1), .flags = I2C_M_RD,
			.buf = data, .len = count }
	};
	b[0] = (u8) reg >> 24;
	b[1] = (u8)(reg >> 16) & 0xff;
	b[2] = (u8)(reg >> 8) & 0xff;
	b[3] = (u8) reg & 0xff;

	ret = i2c_transfer(state->i2c_adap, msg, 2);

	return ret;
}

static int it913x_read_reg_u8(struct it913x_fe_state *state, u32 reg)
{
	int ret;
	u8 b[1];
	ret = it913x_read_reg(state, reg, &b[0], sizeof(b));
	return (ret < 0) ? -ENODEV : b[0];
}

static int it913x_write(struct it913x_fe_state *state,
		u8 pro, u32 reg, u8 buf[], u8 count)
{
	u8 b[256];
	struct i2c_msg msg[1] = {
		{ .addr = state->i2c_addr + (pro << 1), .flags = 0,
		  .buf = b, .len = count + 4 }
	};
	int ret;

	b[0] = (u8) reg >> 24;
	b[1] = (u8)(reg >> 16) & 0xff;
	b[2] = (u8)(reg >> 8) & 0xff;
	b[3] = (u8) reg & 0xff;
	memcpy(&b[4], buf, count);

	ret = i2c_transfer(state->i2c_adap, msg, 1);

	if (ret < 0)
		return -EIO;

	return 0;
}

static int it913x_write_reg(struct it913x_fe_state *state,
		u8 pro, u32 reg, u32 data)
{
	int ret;
	u8 b[4];
	u8 s;

	b[0] = data >> 24;
	b[1] = (data >> 16) & 0xff;
	b[2] = (data >> 8) & 0xff;
	b[3] = data & 0xff;
	/* expand write as needed */
	if (data < 0x100)
		s = 3;
	else if (data < 0x1000)
		s = 2;
	else if (data < 0x100000)
		s = 1;
	else
		s = 0;

	ret = it913x_write(state, pro, reg, &b[s], sizeof(b) - s);

	return ret;
}

static int it913x_fe_script_loader(struct it913x_fe_state *state,
		struct it913xset *loadscript)
{
	int ret, i;
	if (loadscript == NULL)
		return -EINVAL;

	for (i = 0; i < 1000; ++i) {
		if (loadscript[i].pro == 0xff)
			break;
		ret = it913x_write(state, loadscript[i].pro,
			loadscript[i].address,
			loadscript[i].reg, loadscript[i].count);
		if (ret < 0)
			return -ENODEV;
	}
	return 0;
}

static int it913x_init_tuner(struct it913x_fe_state *state)
{
	int ret, i, reg;
	u8 val, nv_val;
	u8 nv[] = {48, 32, 24, 16, 12, 8, 6, 4, 2};
	u8 b[2];

	reg = it913x_read_reg_u8(state, 0xec86);
	switch (reg) {
	case 0:
		state->tun_clk_mode = reg;
		state->tun_xtal = 2000;
		state->tun_fdiv = 3;
		val = 16;
		break;
	case -ENODEV:
		return -ENODEV;
	case 1:
	default:
		state->tun_clk_mode = reg;
		state->tun_xtal = 640;
		state->tun_fdiv = 1;
		val = 6;
		break;
	}

	reg = it913x_read_reg_u8(state, 0xed03);

	if (reg < 0)
		return -ENODEV;
	else if (reg < sizeof(nv))
		nv_val = nv[reg];
	else
		nv_val = 2;

	for (i = 0; i < 50; i++) {
		ret = it913x_read_reg(state, 0xed23, &b[0], sizeof(b));
		reg = (b[1] << 8) + b[0];
		if (reg > 0)
			break;
		if (ret < 0)
			return -ENODEV;
		udelay(2000);
	}
	state->tun_fn_min = state->tun_xtal * reg;
	state->tun_fn_min /= (state->tun_fdiv * nv_val);
	deb_info("Tuner fn_min %d", state->tun_fn_min);

	for (i = 0; i < 50; i++) {
		reg = it913x_read_reg_u8(state, 0xec82);
		if (reg > 0)
			break;
		if (reg < 0)
			return -ENODEV;
		udelay(2000);
	}

	return it913x_write_reg(state, PRO_DMOD, 0xed81, val);
}

static int it9137_set_tuner(struct it913x_fe_state *state,
		enum fe_bandwidth bandwidth, u32 frequency_m)
{
	struct it913xset *set_tuner = set_it9137_template;
	int ret, reg;
	u32 frequency = frequency_m / 1000;
	u32 freq, temp_f, tmp;
	u16 iqik_m_cal;
	u16 n_div;
	u8 n;
	u8 l_band;
	u8 lna_band;
	u8 bw;

	deb_info("Tuner Frequency %d Bandwidth %d", frequency, bandwidth);

	if (frequency >= 51000 && frequency <= 440000) {
		l_band = 0;
		lna_band = 0;
	} else if (frequency > 440000 && frequency <= 484000) {
		l_band = 1;
		lna_band = 1;
	} else if (frequency > 484000 && frequency <= 533000) {
		l_band = 1;
		lna_band = 2;
	} else if (frequency > 533000 && frequency <= 587000) {
		l_band = 1;
		lna_band = 3;
	} else if (frequency > 587000 && frequency <= 645000) {
		l_band = 1;
		lna_band = 4;
	} else if (frequency > 645000 && frequency <= 710000) {
		l_band = 1;
		lna_band = 5;
	} else if (frequency > 710000 && frequency <= 782000) {
		l_band = 1;
		lna_band = 6;
	} else if (frequency > 782000 && frequency <= 860000) {
		l_band = 1;
		lna_band = 7;
	} else if (frequency > 1450000 && frequency <= 1492000) {
		l_band = 1;
		lna_band = 0;
	} else if (frequency > 1660000 && frequency <= 1685000) {
		l_band = 1;
		lna_band = 1;
	} else
		return -EINVAL;
	set_tuner[0].reg[0] = lna_band;

	if (bandwidth == BANDWIDTH_5_MHZ)
		bw = 0;
	else if (bandwidth == BANDWIDTH_6_MHZ)
		bw = 2;
	else if (bandwidth == BANDWIDTH_7_MHZ)
		bw = 4;
	else if (bandwidth == BANDWIDTH_8_MHZ)
		bw = 6;
	else
		bw = 6;

	set_tuner[1].reg[0] = bw;
	set_tuner[2].reg[0] = 0xa0 | (l_band << 3);

	if (frequency > 53000 && frequency <= 74000) {
		n_div = 48;
		n = 0;
	} else if (frequency > 74000 && frequency <= 111000) {
		n_div = 32;
		n = 1;
	} else if (frequency > 111000 && frequency <= 148000) {
		n_div = 24;
		n = 2;
	} else if (frequency > 148000 && frequency <= 222000) {
		n_div = 16;
		n = 3;
	} else if (frequency > 222000 && frequency <= 296000) {
		n_div = 12;
		n = 4;
	} else if (frequency > 296000 && frequency <= 445000) {
		n_div = 8;
		n = 5;
	} else if (frequency > 445000 && frequency <= state->tun_fn_min) {
		n_div = 6;
		n = 6;
	} else if (frequency > state->tun_fn_min && frequency <= 950000) {
		n_div = 4;
		n = 7;
	} else if (frequency > 1450000 && frequency <= 1680000) {
		n_div = 2;
		n = 0;
	} else
		return -EINVAL;

	reg = it913x_read_reg_u8(state, 0xed81);
	iqik_m_cal = (u16)reg * n_div;

	if (reg < 0x20) {
		if (state->tun_clk_mode == 0)
			iqik_m_cal = (iqik_m_cal * 9) >> 5;
		else
			iqik_m_cal >>= 1;
	} else {
		iqik_m_cal = 0x40 - iqik_m_cal;
		if (state->tun_clk_mode == 0)
			iqik_m_cal = ~((iqik_m_cal * 9) >> 5);
		else
			iqik_m_cal = ~(iqik_m_cal >> 1);
	}

	temp_f = frequency * (u32)n_div * (u32)state->tun_fdiv;
	freq = temp_f / state->tun_xtal;
	tmp = freq * state->tun_xtal;

	if ((temp_f - tmp) >= (state->tun_xtal >> 1))
		freq++;

	freq += (u32) n << 13;
	/* Frequency OMEGA_IQIK_M_CAL_MID*/
	temp_f = freq + (u32)iqik_m_cal;

	set_tuner[3].reg[0] =  temp_f & 0xff;
	set_tuner[4].reg[0] =  (temp_f >> 8) & 0xff;

	deb_info("High Frequency = %04x", temp_f);

	/* Lower frequency */
	set_tuner[5].reg[0] =  freq & 0xff;
	set_tuner[6].reg[0] =  (freq >> 8) & 0xff;

	deb_info("low Frequency = %04x", freq);

	ret = it913x_fe_script_loader(state, set_tuner);

	return (ret < 0) ? -ENODEV : 0;
}

static int it913x_fe_select_bw(struct it913x_fe_state *state,
			enum fe_bandwidth bandwidth, u32 adcFrequency)
{
	int ret, i;
	u8 buffer[256];
	u32 coeff[8];
	u16 bfsfcw_fftinx_ratio;
	u16 fftinx_bfsfcw_ratio;
	u8 count;
	u8 bw;
	u8 adcmultiplier;

	deb_info("Bandwidth %d Adc %d", bandwidth, adcFrequency);

	if (bandwidth == BANDWIDTH_5_MHZ)
		bw = 3;
	else if (bandwidth == BANDWIDTH_6_MHZ)
		bw = 0;
	else if (bandwidth == BANDWIDTH_7_MHZ)
		bw = 1;
	else if (bandwidth == BANDWIDTH_8_MHZ)
		bw = 2;
	else
		bw = 2;

	ret = it913x_write_reg(state, PRO_DMOD, REG_BW, bw);

	if (state->table == NULL)
		return -EINVAL;

	/* In write order */
	coeff[0] = state->table[bw].coeff_1_2048;
	coeff[1] = state->table[bw].coeff_2_2k;
	coeff[2] = state->table[bw].coeff_1_8191;
	coeff[3] = state->table[bw].coeff_1_8192;
	coeff[4] = state->table[bw].coeff_1_8193;
	coeff[5] = state->table[bw].coeff_2_8k;
	coeff[6] = state->table[bw].coeff_1_4096;
	coeff[7] = state->table[bw].coeff_2_4k;
	bfsfcw_fftinx_ratio = state->table[bw].bfsfcw_fftinx_ratio;
	fftinx_bfsfcw_ratio = state->table[bw].fftinx_bfsfcw_ratio;

	/* ADC multiplier */
	ret = it913x_read_reg_u8(state, ADC_X_2);
	if (ret < 0)
		return -EINVAL;

	adcmultiplier = ret;

	count = 0;

	/*  Build Buffer for COEFF Registers */
	for (i = 0; i < 8; i++) {
		if (adcmultiplier == 1)
			coeff[i] /= 2;
		buffer[count++] = (coeff[i] >> 24) & 0x3;
		buffer[count++] = (coeff[i] >> 16) & 0xff;
		buffer[count++] = (coeff[i] >> 8) & 0xff;
		buffer[count++] = coeff[i] & 0xff;
	}

	/* bfsfcw_fftinx_ratio register 0x21-0x22 */
	buffer[count++] = bfsfcw_fftinx_ratio & 0xff;
	buffer[count++] = (bfsfcw_fftinx_ratio >> 8) & 0xff;
	/* fftinx_bfsfcw_ratio register 0x23-0x24 */
	buffer[count++] = fftinx_bfsfcw_ratio & 0xff;
	buffer[count++] = (fftinx_bfsfcw_ratio >> 8) & 0xff;
	/* start at COEFF_1_2048 and write through to fftinx_bfsfcw_ratio*/
	ret = it913x_write(state, PRO_DMOD, COEFF_1_2048, buffer, count);

	for (i = 0; i < 42; i += 8)
		debug_data_snipet(0x1, "Buffer", &buffer[i]);

	return ret;
}



static int it913x_fe_read_status(struct dvb_frontend *fe, fe_status_t *status)
{
	struct it913x_fe_state *state = fe->demodulator_priv;
	int ret, i;
	fe_status_t old_status = state->it913x_status;
	*status = 0;

	if (state->it913x_status == 0) {
		ret = it913x_read_reg_u8(state, EMPTY_CHANNEL_STATUS);
		if (ret == 0x1) {
			*status |= FE_HAS_SIGNAL;
			for (i = 0; i < 40; i++) {
				ret = it913x_read_reg_u8(state, MP2IF_SYNC_LK);
				if (ret == 0x1)
					break;
				msleep(25);
			}
			if (ret == 0x1)
				*status |= FE_HAS_CARRIER
					| FE_HAS_VITERBI
					| FE_HAS_SYNC;
			state->it913x_status = *status;
		}
	}

	if (state->it913x_status & FE_HAS_SYNC) {
		ret = it913x_read_reg_u8(state, TPSD_LOCK);
		if (ret == 0x1)
			*status |= FE_HAS_LOCK
				| state->it913x_status;
		else
			state->it913x_status = 0;
		if (old_status != state->it913x_status)
			ret = it913x_write_reg(state, PRO_LINK, GPIOH3_O, ret);
	}

	return 0;
}

static int it913x_fe_read_signal_strength(struct dvb_frontend *fe,
		u16 *strength)
{
	struct it913x_fe_state *state = fe->demodulator_priv;
	int ret = it913x_read_reg_u8(state, SIGNAL_LEVEL);
	/*SIGNAL_LEVEL always returns 100%! so using FE_HAS_SIGNAL as switch*/
	if (state->it913x_status & FE_HAS_SIGNAL)
		ret = (ret * 0xff) / 0x64;
	else
		ret = 0x0;
	ret |= ret << 0x8;
	*strength = ret;
	return 0;
}

static int it913x_fe_read_snr(struct dvb_frontend *fe, u16* snr)
{
	struct it913x_fe_state *state = fe->demodulator_priv;
	int ret = it913x_read_reg_u8(state, SIGNAL_QUALITY);
	ret = (ret * 0xff) / 0x64;
	ret |= (ret << 0x8);
	*snr = ~ret;
	return 0;
}

static int it913x_fe_read_ber(struct dvb_frontend *fe, u32 *ber)
{
	*ber = 0;
	return 0;
}

static int it913x_fe_read_ucblocks(struct dvb_frontend *fe, u32 *ucblocks)
{
	*ucblocks = 0;
	return 0;
}

static int it913x_fe_get_frontend(struct dvb_frontend *fe,
			struct dvb_frontend_parameters *p)
{
	struct it913x_fe_state *state = fe->demodulator_priv;
	int ret;
	u8 reg[8];

	ret = it913x_read_reg(state, REG_TPSD_TX_MODE, reg, sizeof(reg));

	if (reg[3] < 3)
		p->u.ofdm.constellation = fe_con[reg[3]];

	if (reg[0] < 3)
		p->u.ofdm.transmission_mode = fe_mode[reg[0]];

	if (reg[1] < 4)
		p->u.ofdm.guard_interval = fe_gi[reg[1]];

	if (reg[2] < 4)
		p->u.ofdm.hierarchy_information = fe_hi[reg[2]];

	p->u.ofdm.code_rate_HP = (reg[6] < 6) ? fe_code[reg[6]] : FEC_NONE;
	p->u.ofdm.code_rate_LP = (reg[7] < 6) ? fe_code[reg[7]] : FEC_NONE;

	return 0;
}

static int it913x_fe_set_frontend(struct dvb_frontend *fe,
			struct dvb_frontend_parameters *p)
{
	struct it913x_fe_state *state = fe->demodulator_priv;
	int ret, i;
	u8 empty_ch, last_ch;

	state->it913x_status = 0;

	/* Set bw*/
	ret = it913x_fe_select_bw(state, p->u.ofdm.bandwidth,
		state->adcFrequency);

	/* Training Mode Off */
	ret = it913x_write_reg(state, PRO_LINK, TRAINING_MODE, 0x0);

	/* Clear Empty Channel */
	ret = it913x_write_reg(state, PRO_DMOD, EMPTY_CHANNEL_STATUS, 0x0);

	/* Clear bits */
	ret = it913x_write_reg(state, PRO_DMOD, MP2IF_SYNC_LK, 0x0);
	/* LED on */
	ret = it913x_write_reg(state, PRO_LINK, GPIOH3_O, 0x1);
	/* Select Band*/
	if ((p->frequency >= 51000000) && (p->frequency <= 230000000))
		i = 0;
	else if ((p->frequency >= 350000000) && (p->frequency <= 900000000))
			i = 1;
	else if ((p->frequency >= 1450000000) && (p->frequency <= 1680000000))
			i = 2;
		else
			return -EOPNOTSUPP;

	ret = it913x_write_reg(state, PRO_DMOD, FREE_BAND, i);

	deb_info("Frontend Set Tuner Type %02x", state->tuner_type);
	switch (state->tuner_type) {
	case IT9137: /* Tuner type 0x38 */
		ret = it9137_set_tuner(state,
			p->u.ofdm.bandwidth, p->frequency);
		break;
	default:
		if (fe->ops.tuner_ops.set_params) {
			fe->ops.tuner_ops.set_params(fe, p);
			if (fe->ops.i2c_gate_ctrl)
				fe->ops.i2c_gate_ctrl(fe, 0);
		}
		break;
	}
	/* LED off */
	ret = it913x_write_reg(state, PRO_LINK, GPIOH3_O, 0x0);
	/* Trigger ofsm */
	ret = it913x_write_reg(state, PRO_DMOD, TRIGGER_OFSM, 0x0);
	last_ch = 2;
	for (i = 0; i < 40; ++i) {
		empty_ch = it913x_read_reg_u8(state, EMPTY_CHANNEL_STATUS);
		if (last_ch == 1 && empty_ch == 1)
			break;
		if (last_ch == 2 && empty_ch == 2)
			return 0;
		last_ch = empty_ch;
		msleep(25);
	}
	for (i = 0; i < 40; ++i) {
		if (it913x_read_reg_u8(state, D_TPSD_LOCK) == 1)
			break;
		msleep(25);
	}

	state->frequency = p->frequency;
	return 0;
}

static int it913x_fe_suspend(struct it913x_fe_state *state)
{
	int ret, i;
	u8 b;

	ret = it913x_write_reg(state, PRO_DMOD, SUSPEND_FLAG, 0x1);

	ret |= it913x_write_reg(state, PRO_DMOD, TRIGGER_OFSM, 0x0);

	for (i = 0; i < 128; i++) {
		ret = it913x_read_reg(state, SUSPEND_FLAG, &b, 1);
		if (ret < 0)
			return -ENODEV;
		if (b == 0)
			break;

	}

	ret |= it913x_write_reg(state, PRO_DMOD, AFE_MEM0, 0x8);
	/* Turn LED off */
	ret |= it913x_write_reg(state, PRO_LINK, GPIOH3_O, 0x0);

	ret |= it913x_fe_script_loader(state, it9137_tuner_off);

	return (ret < 0) ? -ENODEV : 0;
}

/* Power sequence */
/* Power Up	Tuner on -> Frontend suspend off -> Tuner clk on */
/* Power Down	Frontend suspend on -> Tuner clk off -> Tuner off */

static int it913x_fe_sleep(struct dvb_frontend *fe)
{
	struct it913x_fe_state *state = fe->demodulator_priv;
	return it913x_fe_suspend(state);
}

static u32 compute_div(u32 a, u32 b, u32 x)
{
	u32 res = 0;
	u32 c = 0;
	u32 i = 0;

	if (a > b) {
		c = a / b;
		a = a - c * b;
	}

	for (i = 0; i < x; i++) {
		if (a >= b) {
			res += 1;
			a -= b;
		}
		a <<= 1;
		res <<= 1;
	}

	res = (c << x) + res;

	return res;
}

static int it913x_fe_start(struct it913x_fe_state *state)
{
	struct it913xset *set_fe;
	struct it913xset *set_mode;
	int ret;
	u8 adf = (state->adf & 0xf);
	u32 adc, xtal;
	u8 b[4];

	ret = it913x_init_tuner(state);

	if (adf < 12) {
		state->crystalFrequency = fe_clockTable[adf].xtal ;
		state->table = fe_clockTable[adf].table;
		state->adcFrequency = state->table->adcFrequency;

		adc = compute_div(state->adcFrequency, 1000000ul, 19ul);
		xtal = compute_div(state->crystalFrequency, 1000000ul, 19ul);

	} else
		return -EINVAL;

	deb_info("Xtal Freq :%d Adc Freq :%d Adc %08x Xtal %08x",
		state->crystalFrequency, state->adcFrequency, adc, xtal);

	/* Set LED indicator on GPIOH3 */
	ret = it913x_write_reg(state, PRO_LINK, GPIOH3_EN, 0x1);
	ret |= it913x_write_reg(state, PRO_LINK, GPIOH3_ON, 0x1);
	ret |= it913x_write_reg(state, PRO_LINK, GPIOH3_O, 0x1);

	ret |= it913x_write_reg(state, PRO_LINK, 0xf641, state->tuner_type);
	ret |= it913x_write_reg(state, PRO_DMOD, 0xf5ca, 0x01);
	ret |= it913x_write_reg(state, PRO_DMOD, 0xf715, 0x01);

	b[0] = xtal & 0xff;
	b[1] = (xtal >> 8) & 0xff;
	b[2] = (xtal >> 16) & 0xff;
	b[3] = (xtal >> 24);
	ret |= it913x_write(state, PRO_DMOD, XTAL_CLK, b , 4);

	b[0] = adc & 0xff;
	b[1] = (adc >> 8) & 0xff;
	b[2] = (adc >> 16) & 0xff;
	ret |= it913x_write(state, PRO_DMOD, ADC_FREQ, b, 3);

	switch (state->tuner_type) {
	case IT9137: /* Tuner type 0x38 */
		set_fe = it9137_set;
		break;
	default:
		return -EINVAL;
	}

	/* set the demod */
	ret = it913x_fe_script_loader(state, set_fe);
	/* Always solo frontend */
	set_mode = set_solo_fe;
	ret |= it913x_fe_script_loader(state, set_mode);

	ret |= it913x_fe_suspend(state);
	return 0;
}

static int it913x_fe_init(struct dvb_frontend *fe)
{
	struct it913x_fe_state *state = fe->demodulator_priv;
	int ret = 0;
	/* Power Up Tuner - common all versions */
	ret = it913x_write_reg(state, PRO_DMOD, 0xec40, 0x1);

	ret |= it913x_write_reg(state, PRO_DMOD, AFE_MEM0, 0x0);

	ret |= it913x_fe_script_loader(state, init_1);

	switch (state->tuner_type) {
	case IT9137:
		ret |= it913x_write_reg(state, PRO_DMOD, 0xfba8, 0x0);
		break;
	default:
		return -EINVAL;
	}

	return (ret < 0) ? -ENODEV : 0;
}

static void it913x_fe_release(struct dvb_frontend *fe)
{
	struct it913x_fe_state *state = fe->demodulator_priv;
	kfree(state);
}

static struct dvb_frontend_ops it913x_fe_ofdm_ops;

struct dvb_frontend *it913x_fe_attach(struct i2c_adapter *i2c_adap,
		u8 i2c_addr, u8 adf, u8 type)
{
	struct it913x_fe_state *state = NULL;
	int ret;
	/* allocate memory for the internal state */
	state = kzalloc(sizeof(struct it913x_fe_state), GFP_KERNEL);
	if (state == NULL)
		goto error;

	state->i2c_adap = i2c_adap;
	state->i2c_addr = i2c_addr;
	state->adf = adf;
	state->tuner_type = type;

	ret = it913x_fe_start(state);
	if (ret < 0)
		goto error;


	/* create dvb_frontend */
	memcpy(&state->frontend.ops, &it913x_fe_ofdm_ops,
			sizeof(struct dvb_frontend_ops));
	state->frontend.demodulator_priv = state;

	return &state->frontend;
error:
	kfree(state);
	return NULL;
}
EXPORT_SYMBOL(it913x_fe_attach);

static struct dvb_frontend_ops it913x_fe_ofdm_ops = {

	.info = {
		.name			= "it913x-fe DVB-T",
		.type			= FE_OFDM,
		.frequency_min		= 51000000,
		.frequency_max		= 1680000000,
		.frequency_stepsize	= 62500,
		.caps = FE_CAN_FEC_1_2 | FE_CAN_FEC_2_3 | FE_CAN_FEC_3_4 |
			FE_CAN_FEC_4_5 | FE_CAN_FEC_5_6 | FE_CAN_FEC_6_7 |
			FE_CAN_FEC_7_8 | FE_CAN_FEC_8_9 | FE_CAN_FEC_AUTO |
			FE_CAN_QAM_16 | FE_CAN_QAM_64 | FE_CAN_QAM_AUTO |
			FE_CAN_TRANSMISSION_MODE_AUTO |
			FE_CAN_GUARD_INTERVAL_AUTO |
			FE_CAN_HIERARCHY_AUTO,
	},

	.release = it913x_fe_release,

	.init = it913x_fe_init,
	.sleep = it913x_fe_sleep,

	.set_frontend = it913x_fe_set_frontend,
	.get_frontend = it913x_fe_get_frontend,

	.read_status = it913x_fe_read_status,
	.read_signal_strength = it913x_fe_read_signal_strength,
	.read_snr = it913x_fe_read_snr,
	.read_ber = it913x_fe_read_ber,
	.read_ucblocks = it913x_fe_read_ucblocks,
};

MODULE_DESCRIPTION("it913x Frontend and it9137 tuner");
MODULE_AUTHOR("Malcolm Priestley tvboxspy@gmail.com");
MODULE_VERSION("1.07");
MODULE_LICENSE("GPL");
