/* Copyright (c) 2011-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "%s:%d " fmt, __func__, __LINE__

#include <linux/module.h>
#include "msm_sd.h"
#include "msm_actuator.h"
#include "msm_cci.h"

DEFINE_MSM_MUTEX(msm_actuator_mutex);

#undef CDBG
#ifdef MSM_ACUTUATOR_DEBUG
#define CDBG(fmt, args...) pr_err(fmt, ##args)
#else
#define CDBG(fmt, args...) pr_debug(fmt, ##args)
#endif

#ifdef CONFIG_SEKONIX_LENS_ACT
#define CHECK_ACT_WRITE_COUNT
#define ACT_STOP_POS            10
#define ACT_MIN_MOVE_RANGE      200
#define ACT_POSTURE_MARGIN      100
//extern uint8_t imx111_afcalib_data[4];
#else
/* modification qct's af calibration routines */
#define ACTUATOR_EEPROM_SADDR                (0x50 >> 1)
#define ACTUATOR_START_ADDR                  0x06
#define ACTUATOR_MACRO_ADDR                  0x08
#define ACTUATOR_MARGIN                      30
#define ACTUATOR_MIN_MOVE_RANGE              200 // TBD
#endif

#define ACTUATOR_EEPROM_SLAVEADDR                (0x20)

static struct msm_actuator_ctrl_t msm_actuator_t;
static struct msm_actuator msm_vcm_actuator_table;
static struct msm_actuator msm_piezo_actuator_table;

static struct msm_actuator *actuators[] = {
	&msm_vcm_actuator_table,
	&msm_piezo_actuator_table,
};

static int32_t msm_actuator_piezo_set_default_focus(
	struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_move_params_t *move_params)
{
	int32_t rc = 0;
	CDBG("Enter\n");

	if (a_ctrl->curr_step_pos != 0) {
		a_ctrl->i2c_tbl_index = 0;
		a_ctrl->func_tbl->actuator_parse_i2c_params(a_ctrl,
			a_ctrl->initial_code, 0, 0);
		a_ctrl->func_tbl->actuator_parse_i2c_params(a_ctrl,
			a_ctrl->initial_code, 0, 0);
		rc = a_ctrl->i2c_client.i2c_func_tbl->
			i2c_write_table_w_microdelay(
			&a_ctrl->i2c_client, a_ctrl->i2c_reg_tbl,
			a_ctrl->i2c_tbl_index, a_ctrl->i2c_data_type);
		if (rc < 0) {
			pr_err("%s: i2c write error:%d\n",
				__func__, rc);
			return rc;
		}
		a_ctrl->i2c_tbl_index = 0;
		a_ctrl->curr_step_pos = 0;
	}
	CDBG("Exit\n");
	return rc;
}

static void msm_actuator_parse_i2c_params(struct msm_actuator_ctrl_t *a_ctrl,
	int16_t next_lens_position, uint32_t hw_params, uint16_t delay)
{
	struct msm_actuator_reg_params_t *write_arr = a_ctrl->reg_tbl;
	uint32_t hw_dword = hw_params;
	uint16_t i2c_byte1 = 0, i2c_byte2 = 0;
	uint16_t value = 0;
	uint32_t size = a_ctrl->reg_tbl_size, i = 0;
	struct msm_camera_i2c_reg_tbl *i2c_tbl = a_ctrl->i2c_reg_tbl;
	CDBG("Enter\n");
	for (i = 0; i < size; i++) {
		if (write_arr[i].reg_write_type == MSM_ACTUATOR_WRITE_DAC) {
			value = (next_lens_position <<
				write_arr[i].data_shift) |
				((hw_dword & write_arr[i].hw_mask) >>
				write_arr[i].hw_shift);

			if (write_arr[i].reg_addr != 0xFFFF) {
				i2c_byte1 = write_arr[i].reg_addr;
				i2c_byte2 = value;
				i++;
			} else {
				i2c_byte1 = (value & 0xFF00) >> 8;
				i2c_byte2 = value & 0xFF;
			}
		} else {
			i2c_byte1 = write_arr[i].reg_addr;
			i2c_byte2 = (hw_dword & write_arr[i].hw_mask) >>
				write_arr[i].hw_shift;
		}
		if (a_ctrl->i2c_tbl_index > a_ctrl->total_steps) {
			pr_err("failed: i2c table index out of bound\n");
			break;
		}
		CDBG("i2c_byte1:0x%x, i2c_byte2:0x%x\n", i2c_byte1, i2c_byte2);
		if (a_ctrl->i2c_tbl_index >
			a_ctrl->total_steps) {
			pr_err("failed:i2c table index out of bound\n");
			break;
		}
		i2c_tbl[a_ctrl->i2c_tbl_index].reg_addr = i2c_byte1;
		i2c_tbl[a_ctrl->i2c_tbl_index].reg_data = i2c_byte2;
		i2c_tbl[a_ctrl->i2c_tbl_index].delay = delay;
		a_ctrl->i2c_tbl_index++;
	}
	CDBG("Exit\n");
}

#define OTP_LOAD_DUMP   0x3D81
#define OTP_BANK_SELECT 0x3D84
#define OTP_BANK_START  0x3D00

static int32_t calibrate_af_data(struct msm_actuator_ctrl_t *a_ctrl,
	struct reg_settings_t *settings,
	struct msm_actuator_set_info_t *set_info) {
	int rc = 0;
	uint8_t data[2];
	uint16_t address;
	uint16_t saddr;
	uint16_t inf_dac, start_dac, macro_dac;

	inf_dac = start_dac = macro_dac = 0;
	saddr = a_ctrl->i2c_client.client->addr;
	a_ctrl->i2c_client.client->addr = ACTUATOR_EEPROM_SLAVEADDR;
	a_ctrl->i2c_client.addr_type = 2;

	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
		&a_ctrl->i2c_client, 0x0100,
		0x01, MSM_CAMERA_I2C_BYTE_DATA);

	/* select otp bank 3 */
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
		&a_ctrl->i2c_client, OTP_BANK_SELECT,
		0xc3, MSM_CAMERA_I2C_BYTE_DATA);

	/* load otp */
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
		&a_ctrl->i2c_client, OTP_LOAD_DUMP,
		0x01, MSM_CAMERA_I2C_BYTE_DATA);

	/* read inf */
	address = OTP_BANK_START;
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_read_seq(
		&a_ctrl->i2c_client, address,
		data, 2);
	if (rc < 0)
		pr_err("infinity otp data read failed\n");
	inf_dac = data[0] << 8 | data[1];
	if (inf_dac == 0) {
		/* select otp bank 2 */
		rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
			&a_ctrl->i2c_client, OTP_BANK_SELECT,
			0xc2, MSM_CAMERA_I2C_BYTE_DATA);

		/* load otp */
		rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
			&a_ctrl->i2c_client, OTP_LOAD_DUMP,
			0x01, MSM_CAMERA_I2C_BYTE_DATA);

		/* read inf */
		address = OTP_BANK_START;
		rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_read_seq(
			&a_ctrl->i2c_client, address,
			data, 2);
		if (rc < 0)
			pr_err("infinity otp data read failed\n");
		inf_dac = data[0] << 8 | data[1];
		if (inf_dac == 0) {
			/* select otp bank 1 */
			rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&a_ctrl->i2c_client, OTP_BANK_SELECT,
				0xc1, MSM_CAMERA_I2C_BYTE_DATA);

			/* load otp */
			rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&a_ctrl->i2c_client, OTP_LOAD_DUMP,
				0x01, MSM_CAMERA_I2C_BYTE_DATA);

			/* read inf */
			address = OTP_BANK_START;
			rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_read_seq(
				&a_ctrl->i2c_client, address,
				data, 2);
			if (rc < 0)
				pr_err("infinity otp data read failed\n");
			inf_dac = data[0] << 8 | data[1];
		}
	}

	/* read 1m */
	address += 2;

	/* read mac */
	address += 2;
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_read_seq(
		&a_ctrl->i2c_client, address,
		data, 2);
	if (rc < 0)
		pr_err("macro otp data read failed\n");

	macro_dac = data[0] << 8 | data[1];

	/* read start position */
	address += 2;
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_read_seq(
		&a_ctrl->i2c_client, address,
		data, 2);

	if (rc < 0)
		pr_err("Starting current otp data read failed\n");

	start_dac = data[0] << 8 | data[1];

	CDBG("OTP inf = 0x%x", inf_dac);
	CDBG("OTP mac = 0x%x", macro_dac);
	CDBG("OTP start = 0x%x", start_dac);

	if ((inf_dac != 0) && (macro_dac != 0) && (macro_dac > inf_dac)) {
		if (inf_dac > 60)
			inf_dac -= 60;
		set_info->af_tuning_params.initial_code = inf_dac;
		a_ctrl->region_params[0].code_per_step =
		   (macro_dac - inf_dac) /
		   (a_ctrl->region_params[0].step_bound[0] - 5);
		settings[3].reg_data = inf_dac >> 8;
		settings[5].reg_data = settings[3].reg_data | 0x04;
		settings[4].reg_data =
		settings[6].reg_data = inf_dac & 0xFF;
	}

	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
		&a_ctrl->i2c_client, 0x0100,
		0x00, MSM_CAMERA_I2C_BYTE_DATA);
	a_ctrl->i2c_client.client->addr = saddr;
	a_ctrl->i2c_client.addr_type = 1;

	CDBG("code_per_step = %d\n", a_ctrl->region_params[0].code_per_step);
	return rc;
}

static int32_t msm_actuator_init_focus(struct msm_actuator_ctrl_t *a_ctrl,
	uint16_t size, enum msm_actuator_data_type type,
	struct reg_settings_t *settings)
{
	int32_t rc = -EFAULT;
	int32_t i = 0;
	CDBG("Enter\n");

	for (i = 0; i < size; i++) {
		switch (type) {
		case MSM_ACTUATOR_BYTE_DATA:
			rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&a_ctrl->i2c_client,
				settings[i].reg_addr,
				settings[i].reg_data, MSM_CAMERA_I2C_BYTE_DATA);
			break;
		case MSM_ACTUATOR_WORD_DATA:
			rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&a_ctrl->i2c_client,
				settings[i].reg_addr,
				settings[i].reg_data, MSM_CAMERA_I2C_WORD_DATA);
			break;
		default:
			pr_err("Unsupport data type: %d\n", type);
			break;
		}
		if (rc < 0)
			break;
	}

	a_ctrl->curr_step_pos = 0;
	CDBG("Exit\n");
	return rc;
}

static void msm_actuator_write_focus(
	struct msm_actuator_ctrl_t *a_ctrl,
	uint16_t curr_lens_pos,
	struct damping_params_t *damping_params,
	int8_t sign_direction,
	int16_t code_boundary)
{
	int16_t next_lens_pos = 0;
	uint16_t damping_code_step = 0;
	uint16_t wait_time = 0;
	CDBG("Enter\n");

	damping_code_step = damping_params->damping_step;
	wait_time = damping_params->damping_delay;

#ifdef CONFIG_SEKONIX_LENS_ACT
        CDBG("damping_code_step = %d\n",damping_code_step);
        CDBG("wait_time = %d\n",wait_time);
        CDBG("curr_lens_pos = %d\n",curr_lens_pos);
        CDBG("sign_direction = %d\n",sign_direction);
        CDBG("code_boundary = %d\n",code_boundary);
        CDBG("damping_params->hw_params = %d\n",damping_params->hw_params);
        if (damping_code_step ==0) {
            CDBG("[ERROR][%s] damping_code_step = %d ---> 255\n",
                               __func__,damping_code_step);
            damping_code_step = 255;
        }
        if (wait_time ==0) {
            CDBG("[ERROR][%s] wait_time = %d ---> 4500\n",
                               __func__,damping_code_step);
            wait_time = 4500;
        }
#endif


	/* Write code based on damping_code_step in a loop */
	for (next_lens_pos =
		curr_lens_pos + (sign_direction * damping_code_step);
		(sign_direction * next_lens_pos) <=
			(sign_direction * code_boundary);
		next_lens_pos =
			(next_lens_pos +
				(sign_direction * damping_code_step))) {
		a_ctrl->func_tbl->actuator_parse_i2c_params(a_ctrl,
			next_lens_pos, damping_params->hw_params, wait_time);
		curr_lens_pos = next_lens_pos;
	}

	if (curr_lens_pos != code_boundary) {
		a_ctrl->func_tbl->actuator_parse_i2c_params(a_ctrl,
			code_boundary, damping_params->hw_params, wait_time);
	}
	CDBG("Exit\n");
}

static int32_t msm_actuator_piezo_move_focus(
	struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_move_params_t *move_params)
{
	int32_t dest_step_position = move_params->dest_step_pos;
	struct damping_params_t ringing_params_kernel;
	int32_t rc = 0;
	int32_t num_steps = move_params->num_steps;
	CDBG("Enter\n");

	if (copy_from_user(&ringing_params_kernel,
		&(move_params->ringing_params[0]),
		sizeof(struct damping_params_t))) {
		pr_err("copy_from_user failed\n");
		return -EFAULT;
	}

	if (num_steps == 0)
		return rc;

	a_ctrl->i2c_tbl_index = 0;
	a_ctrl->func_tbl->actuator_parse_i2c_params(a_ctrl,
		(num_steps *
		a_ctrl->region_params[0].code_per_step),
		ringing_params_kernel.hw_params, 0);

	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write_table_w_microdelay(
		&a_ctrl->i2c_client,
		a_ctrl->i2c_reg_tbl, a_ctrl->i2c_tbl_index,
		a_ctrl->i2c_data_type);
	if (rc < 0) {
		pr_err("i2c write error:%d\n", rc);
		return rc;
	}
	a_ctrl->i2c_tbl_index = 0;
	a_ctrl->curr_step_pos = dest_step_position;
	CDBG("Exit\n");
	return rc;
}

static int32_t msm_actuator_move_focus(
	struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_move_params_t *move_params)
{
	int32_t rc = 0;
	struct damping_params_t ringing_params_kernel;
	int8_t sign_dir = move_params->sign_dir;
	uint16_t step_boundary = 0;
	uint16_t target_step_pos = 0;
	uint16_t target_lens_pos = 0;
	int16_t dest_step_pos = move_params->dest_step_pos;
	uint16_t curr_lens_pos = 0;
	int dir = move_params->dir;
	int32_t num_steps = move_params->num_steps;

	if (copy_from_user(&ringing_params_kernel,
		&(move_params->ringing_params[a_ctrl->curr_region_index]),
		sizeof(struct damping_params_t))) {
		pr_err("copy_from_user failed\n");
		return -EFAULT;
	}


	CDBG("called, dir %d, num_steps %d\n", dir, num_steps);

	if (dest_step_pos == a_ctrl->curr_step_pos)
		return rc;

	if ((sign_dir > MSM_ACTUATOR_MOVE_SIGNED_NEAR) ||
		(sign_dir < MSM_ACTUATOR_MOVE_SIGNED_FAR)) {
		pr_err("Invalid sign_dir = %d\n", sign_dir);
		return -EFAULT;
	}
	if ((dir > MOVE_FAR) || (dir < MOVE_NEAR)) {
		pr_err("Invalid direction = %d\n", dir);
		return -EFAULT;
	}
	if (dest_step_pos > a_ctrl->total_steps) {
		pr_err("Step pos greater than total steps = %d\n",
		dest_step_pos);
		return -EFAULT;
	}
	curr_lens_pos = a_ctrl->step_position_table[a_ctrl->curr_step_pos];
	a_ctrl->i2c_tbl_index = 0;
	CDBG("curr_step_pos =%d dest_step_pos =%d curr_lens_pos=%d\n",
		a_ctrl->curr_step_pos, dest_step_pos, curr_lens_pos);

	while (a_ctrl->curr_step_pos != dest_step_pos) {
		step_boundary =
			a_ctrl->region_params[a_ctrl->curr_region_index].
			step_bound[dir];
		if ((dest_step_pos * sign_dir) <=
			(step_boundary * sign_dir)) {

			target_step_pos = dest_step_pos;
			target_lens_pos =
				a_ctrl->step_position_table[target_step_pos];
			a_ctrl->func_tbl->actuator_write_focus(a_ctrl,
					curr_lens_pos,
					&ringing_params_kernel,
					sign_dir,
					target_lens_pos);
			curr_lens_pos = target_lens_pos;

		} else {
			target_step_pos = step_boundary;
			target_lens_pos =
				a_ctrl->step_position_table[target_step_pos];
			a_ctrl->func_tbl->actuator_write_focus(a_ctrl,
					curr_lens_pos,
					&ringing_params_kernel,
					sign_dir,
					target_lens_pos);
			curr_lens_pos = target_lens_pos;

			a_ctrl->curr_region_index += sign_dir;
		}
		a_ctrl->curr_step_pos = target_step_pos;
	}
	a_ctrl->i2c_data_type = 2;
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write_table_w_microdelay(
		&a_ctrl->i2c_client,
		a_ctrl->i2c_reg_tbl, a_ctrl->i2c_tbl_index,
		a_ctrl->i2c_data_type);
	a_ctrl->i2c_data_type = 1;
	if (rc < 0) {
		pr_err("i2c write error:%d\n", rc);
		return rc;
	}
	a_ctrl->i2c_tbl_index = 0;
	CDBG("Exit\n");

	return rc;
}

static int32_t msm_actuator_init_default_step_table(struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_set_info_t *set_info)
{
	int16_t code_per_step = 0;
	int16_t cur_code = 0;
	uint16_t step_index = 0, region_index = 0;
	uint16_t step_boundary = 0;
	uint32_t max_code_size = 1;
	uint16_t data_size = set_info->actuator_params.data_size;
	CDBG("Enter\n");

	for (; data_size > 0; data_size--)
		max_code_size *= 2;

        if (a_ctrl->step_position_table) {
        	kfree(a_ctrl->step_position_table);
        	a_ctrl->step_position_table = NULL;
        }

	if (set_info->af_tuning_params.total_steps
		>  MAX_ACTUATOR_AF_TOTAL_STEPS) {
		pr_err("Max actuator totalsteps exceeded = %d\n",
		set_info->af_tuning_params.total_steps);
		return -EFAULT;
	}
	/* Fill step position table */
	a_ctrl->step_position_table =
		kmalloc(sizeof(uint16_t) *
		(set_info->af_tuning_params.total_steps + 1), GFP_KERNEL);

	if (a_ctrl->step_position_table == NULL)
		return -ENOMEM;

	cur_code = set_info->af_tuning_params.initial_code;
	a_ctrl->step_position_table[step_index++] = cur_code;
	for (region_index = 0;
		region_index < a_ctrl->region_size;
		region_index++) {
		code_per_step =
			a_ctrl->region_params[region_index].code_per_step;
		step_boundary =
			a_ctrl->region_params[region_index].
			step_bound[MOVE_NEAR];
		if (step_boundary >
			set_info->af_tuning_params.total_steps) {
			pr_err("invalid step_boundary = %d, max_val = %d",
				step_boundary,
				set_info->af_tuning_params.total_steps);
			kfree(a_ctrl->step_position_table);
			a_ctrl->step_position_table = NULL;
			return -EINVAL;
		}
		for (; step_index <= step_boundary;
			step_index++) {
			cur_code += code_per_step;
			if (cur_code < max_code_size)
				a_ctrl->step_position_table[step_index] =
					cur_code;
			else {
				for (; step_index <
					set_info->af_tuning_params.total_steps;
					step_index++)
					a_ctrl->
						step_position_table[
						step_index] =
						max_code_size;
			}
		}
	}
	CDBG("Exit\n");
	return 0;
}

#ifdef CONFIG_SEKONIX_LENS_ACT
int32_t msm_actuator_init_step_table_use_eeprom(
		struct msm_actuator_ctrl_t *a_ctrl,
		struct msm_actuator_set_info_t *set_info)
{
	int32_t rc = 0;
	int16_t step_index = 0;
	uint32_t total_steps = set_info->af_tuning_params.total_steps;
	uint16_t act_start = 0, act_macro = 0, move_range = 0, step_diff = 0;

	if (total_steps < 1) {
		pr_err("%s: total_steps is too small (%d)\n", __func__,
				total_steps);
		return -EINVAL;
	}

	if (a_ctrl->step_position_table) {
		kfree(a_ctrl->step_position_table);
		a_ctrl->step_position_table = NULL;
	}

	a_ctrl->step_position_table = kmalloc(sizeof(uint16_t) *
			(total_steps + 1), GFP_KERNEL);

	if (a_ctrl->step_position_table == NULL) {
		rc = -ENOMEM;
		return rc;
	}

	/*act_start = (uint16_t)(imx111_afcalib_data[1] << 8) |
			imx111_afcalib_data[0];
	act_macro = ((uint16_t)(imx111_afcalib_data[3] << 8) |
			imx111_afcalib_data[2])+20;*/
	act_start = 25;
	act_macro = 200;

	a_ctrl->step_position_table[0] =
			set_info->af_tuning_params.initial_code;
	if ( act_start > ACT_POSTURE_MARGIN )
		a_ctrl->step_position_table[1] = act_start - ACT_POSTURE_MARGIN;
	else
		a_ctrl->step_position_table[1] = act_start;

	move_range = act_macro - a_ctrl->step_position_table[1];

	if (move_range < ACT_MIN_MOVE_RANGE)
		goto act_cal_fail;

	step_diff = ((step_index - 1) * move_range + ((total_steps - 1) >> 1)) /
				(total_steps - 1);

	for (step_index = 2; step_index < total_steps; step_index++)
		a_ctrl->step_position_table[step_index] = step_diff +
				a_ctrl->step_position_table[1];

	return rc;
act_cal_fail:
	pr_err("%s: calibration to default value not using eeprom data\n",
			__func__);
	rc = msm_actuator_init_default_step_table(a_ctrl, set_info);
	return rc;
}

int32_t msm_actuator_i2c_read_b_eeprom(struct msm_camera_i2c_client *dev_client,
            unsigned char saddr, unsigned char *rxdata)
{
       int32_t rc = 0;
       struct i2c_msg msgs[] = {
               {
                       .addr  = saddr << 1,
                       .flags = 0,
                       .len   = 1,
                       .buf   = rxdata,
               },
               {
                       .addr  = saddr << 1,
                       .flags = I2C_M_RD,
                       .len   = 1,
                       .buf   = rxdata,
               },
       };
       rc = i2c_transfer(dev_client->client->adapter, msgs, 2);
       if (rc < 0)
               CDBG("msm_actuator_i2c_read_b_eeprom failed 0x%x\n", saddr);
       return rc;
}

static int32_t msm_actuator_init_step_table(struct msm_actuator_ctrl_t *a_ctrl,
            struct msm_actuator_set_info_t *set_info)
{
       int32_t rc = 0;
       int16_t cur_code = 0;
       int16_t step_index = 0;
       uint32_t max_code_size = 1;
       uint16_t data_size = set_info->actuator_params.data_size;
       uint16_t act_start = 0, act_macro = 0, move_range = 0;
       unsigned char buf;
       CDBG("%s called\n", __func__);
       if (set_info->af_tuning_params.total_steps < 1) {
               pr_err("%s: total_steps is too small (%d)\n", __func__,
                               set_info->af_tuning_params.total_steps);
               return -EINVAL;
       }
       buf = ACTUATOR_START_ADDR;
       rc = msm_actuator_i2c_read_b_eeprom(&a_ctrl->i2c_client,
               ACTUATOR_EEPROM_SADDR, &buf);
       if (rc < 0)
               goto act_cal_fail;
       act_start = (buf << 8) & 0xFF00;
       buf = ACTUATOR_START_ADDR + 1;
       rc = msm_actuator_i2c_read_b_eeprom(&a_ctrl->i2c_client,
               ACTUATOR_EEPROM_SADDR, &buf);
       if (rc < 0)
               goto act_cal_fail;
       act_start |= buf & 0xFF;
       CDBG("%s: act_start = 0x%4x\n", __func__, act_start);
       buf = ACTUATOR_MACRO_ADDR;
       rc = msm_actuator_i2c_read_b_eeprom(&a_ctrl->i2c_client,
               ACTUATOR_EEPROM_SADDR, &buf);
       if (rc < 0)
               goto act_cal_fail;
       act_macro = (buf << 8) & 0xFF00;
       buf = ACTUATOR_MACRO_ADDR + 1;
       rc = msm_actuator_i2c_read_b_eeprom(&a_ctrl->i2c_client,
               ACTUATOR_EEPROM_SADDR, &buf);
       if (rc < 0)
               goto act_cal_fail;
       act_macro |= buf & 0xFF;
       CDBG("%s: act_macro = 0x%4x\n", __func__, act_macro);
       for (; data_size > 0; data_size--)
               max_code_size *= 2;
       if (a_ctrl->step_position_table) {
               kfree(a_ctrl->step_position_table);
               a_ctrl->step_position_table = NULL;
       }
       a_ctrl->step_position_table =
               kmalloc(sizeof(uint16_t) *
               (set_info->af_tuning_params.total_steps + 1), GFP_KERNEL);
       if (a_ctrl->step_position_table == NULL)
               return -EFAULT;
       cur_code = set_info->af_tuning_params.initial_code;
       a_ctrl->step_position_table[0] = a_ctrl->initial_code;
       if (act_start > ACTUATOR_MARGIN)
               a_ctrl->step_position_table[1] = act_start - ACTUATOR_MARGIN;
       else
               a_ctrl->step_position_table[1] = act_start;
       move_range = act_macro - a_ctrl->step_position_table[1];
       CDBG("%s: move_range = %d\n", __func__, move_range);
       if (move_range < ACTUATOR_MIN_MOVE_RANGE)
               goto act_cal_fail;
       for (step_index = 2;step_index < set_info->af_tuning_params.total_steps;step_index++) {
               a_ctrl->step_position_table[step_index]
                       = ((step_index - 1) * move_range + ((set_info->af_tuning_params.total_steps - 1) >> 1))
                       / (set_info->af_tuning_params.total_steps - 1) + a_ctrl->step_position_table[1];
       }
       a_ctrl->curr_step_pos = 0;
       a_ctrl->curr_region_index = 0;
       return rc;
act_cal_fail:
       pr_err("%s:  act_cal_fail, call default_step_table\n", __func__);
       rc = msm_actuator_init_default_step_table(a_ctrl, set_info);
       return rc;
}
#endif

static int32_t msm_actuator_set_default_focus(
	struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_move_params_t *move_params)
{
	int32_t rc = 0;
	CDBG("Enter\n");

	if (a_ctrl->curr_step_pos != 0)
		rc = a_ctrl->func_tbl->actuator_move_focus(a_ctrl, move_params);
	CDBG("Exit\n");
	return rc;
}

static int32_t msm_actuator_power_down(struct msm_actuator_ctrl_t *a_ctrl)
{
	int32_t rc = 0;
	int32_t code;
	uint8_t buf[2];
	int32_t i;
#ifdef CONFIG_SEKONIX_LENS_ACT
       int cur_pos = a_ctrl->curr_step_pos;
       struct msm_actuator_move_params_t move_params;
       if(cur_pos > ACT_STOP_POS) {
               move_params.sign_dir = MOVE_FAR;
               move_params.dest_step_pos = ACT_STOP_POS;
               rc = a_ctrl->func_tbl->actuator_move_focus(
                               a_ctrl, &move_params);
               msleep(300);
       }
#endif
	if (a_ctrl->vcm_enable) {
		rc = gpio_direction_output(a_ctrl->vcm_pwd, 0);
		if (!rc)
			gpio_free(a_ctrl->vcm_pwd);
	}
	code = a_ctrl->initial_code;
	for (i = 0; i < 3; i++) {
		code -= 30;
		if (code > 0) {
			buf[0] = (code | 0x400) >> 8;
			buf[1] = code & 0xFF;
			rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write_seq(
				&a_ctrl->i2c_client, 0x04, &buf[0], 2);
			usleep_range(15000, 16000);
		}
	}
	buf[0] = buf[1] =  0;
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write_seq(
		&a_ctrl->i2c_client, 0x04, &buf[0], 2);
	usleep_range(8000, 9000);
	kfree(a_ctrl->step_position_table);
	a_ctrl->step_position_table = NULL;
	kfree(a_ctrl->i2c_reg_tbl);
	a_ctrl->i2c_reg_tbl = NULL;
	a_ctrl->i2c_tbl_index = 0;
	CDBG("Exit\n");
	return rc;
}

static int32_t msm_actuator_init(struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_set_info_t *set_info) {
	struct reg_settings_t *init_settings = NULL;
	int32_t rc = -EFAULT;
	uint16_t i = 0;
	struct msm_camera_cci_client *cci_client = NULL;
	CDBG("Enter\n");

	for (i = 0; i < ARRAY_SIZE(actuators); i++) {
		if (set_info->actuator_params.act_type ==
			actuators[i]->act_type) {
			a_ctrl->func_tbl = &actuators[i]->func_tbl;
			rc = 0;
		}
	}

	if (rc < 0) {
		pr_err("Actuator function table not found\n");
		return rc;
	}
	if (set_info->af_tuning_params.total_steps
		>  MAX_ACTUATOR_AF_TOTAL_STEPS) {
		pr_err("Max actuator totalsteps exceeded = %d\n",
		set_info->af_tuning_params.total_steps);
		return -EFAULT;
	}
	if (set_info->af_tuning_params.region_size
		> MAX_ACTUATOR_REGION) {
		pr_err("MAX_ACTUATOR_REGION is exceeded.\n");
		return -EFAULT;
	}

	a_ctrl->region_size = set_info->af_tuning_params.region_size;
	a_ctrl->pwd_step = set_info->af_tuning_params.pwd_step;
	a_ctrl->total_steps = set_info->af_tuning_params.total_steps;

	if (copy_from_user(&a_ctrl->region_params,
		(void *)set_info->af_tuning_params.region_params,
		a_ctrl->region_size * sizeof(struct region_params_t)))
		return -EFAULT;

	if (a_ctrl->act_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		cci_client = a_ctrl->i2c_client.cci_client;
		cci_client->sid =
			set_info->actuator_params.i2c_addr >> 1;
		cci_client->retries = 3;
		cci_client->id_map = 0;
		cci_client->cci_i2c_master = a_ctrl->cci_master;
	} else {
		a_ctrl->i2c_client.client->addr =
			set_info->actuator_params.i2c_addr;
	}

	a_ctrl->i2c_data_type = set_info->actuator_params.i2c_data_type;
	a_ctrl->i2c_client.addr_type = set_info->actuator_params.i2c_addr_type;
	if (set_info->actuator_params.reg_tbl_size <=
		MAX_ACTUATOR_REG_TBL_SIZE) {
		a_ctrl->reg_tbl_size = set_info->actuator_params.reg_tbl_size;
	} else {
		a_ctrl->reg_tbl_size = 0;
		pr_err("MAX_ACTUATOR_REG_TBL_SIZE is exceeded.\n");
		return -EFAULT;
	}

	a_ctrl->i2c_reg_tbl =
		kmalloc(sizeof(struct msm_camera_i2c_reg_tbl) *
		(set_info->af_tuning_params.total_steps + 1), GFP_KERNEL);
	if (!a_ctrl->i2c_reg_tbl) {
		pr_err("kmalloc fail\n");
		return -ENOMEM;
	}

	if (copy_from_user(&a_ctrl->reg_tbl,
		(void *)set_info->actuator_params.reg_tbl_params,
		a_ctrl->reg_tbl_size *
		sizeof(struct msm_actuator_reg_params_t))) {
		kfree(a_ctrl->i2c_reg_tbl);
		return -EFAULT;
	}

	if (set_info->actuator_params.init_setting_size &&
		set_info->actuator_params.init_setting_size
		<= MAX_ACTUATOR_REG_TBL_SIZE) {
		if (a_ctrl->func_tbl->actuator_init_focus) {
			init_settings = kmalloc(sizeof(struct reg_settings_t) *
				(set_info->actuator_params.init_setting_size),
				GFP_KERNEL);
			if (init_settings == NULL) {
				kfree(a_ctrl->i2c_reg_tbl);
				pr_err("Error allocating memory for init_settings\n");
				return -EFAULT;
			}
			if (copy_from_user(init_settings,
				(void *)set_info->actuator_params.init_settings,
				set_info->actuator_params.init_setting_size *
				sizeof(struct reg_settings_t))) {
				kfree(init_settings);
				kfree(a_ctrl->i2c_reg_tbl);
				pr_err("Error copying init_settings\n");
				return -EFAULT;
			}
			rc = calibrate_af_data(a_ctrl, init_settings, set_info);
			rc = a_ctrl->func_tbl->actuator_init_focus(a_ctrl,
				set_info->actuator_params.init_setting_size,
				a_ctrl->i2c_data_type,
				init_settings);
			kfree(init_settings);
			if (rc < 0) {
				kfree(a_ctrl->i2c_reg_tbl);
				pr_err("Error actuator_init_focus\n");
				return -EFAULT;
			}
		}
	}

	a_ctrl->initial_code = set_info->af_tuning_params.initial_code;
	if (a_ctrl->func_tbl->actuator_init_step_table)
		rc = a_ctrl->func_tbl->
			actuator_init_step_table(a_ctrl, set_info);

	a_ctrl->curr_step_pos = 0;
	a_ctrl->curr_region_index = 0;
	CDBG("Exit\n");

	return rc;
}

static int32_t msm_actuator_config(struct msm_actuator_ctrl_t *a_ctrl,
	void __user *argp)
{
	struct msm_actuator_cfg_data *cdata =
		(struct msm_actuator_cfg_data *)argp;
	int32_t rc = 0;
	mutex_lock(a_ctrl->actuator_mutex);
	CDBG("Enter\n");
	CDBG("%s type %d\n", __func__, cdata->cfgtype);
	switch (cdata->cfgtype) {
	case CFG_GET_ACTUATOR_INFO:
		cdata->is_af_supported = 1;
		cdata->cfg.cam_name = a_ctrl->cam_name;
		break;

	case CFG_SET_ACTUATOR_INFO:
		rc = msm_actuator_init(a_ctrl, &cdata->cfg.set_info);
		if (rc < 0)
			pr_err("init table failed %d\n", rc);
		break;

	case CFG_SET_DEFAULT_FOCUS:
		rc = a_ctrl->func_tbl->actuator_set_default_focus(a_ctrl,
			&cdata->cfg.move);
		if (rc < 0)
			pr_err("move focus failed %d\n", rc);
		break;

	case CFG_MOVE_FOCUS:
		rc = a_ctrl->func_tbl->actuator_move_focus(a_ctrl,
			&cdata->cfg.move);
		if (rc < 0)
			pr_err("move focus failed %d\n", rc);
		break;

	case CFG_ACTUATOR_POWERDOWN:
		rc = msm_actuator_power_down(a_ctrl);
		if (rc < 0)
			pr_err("msm_actuator_power_down failed %d\n", rc);
		break;
	default:
		break;
	}
	mutex_unlock(a_ctrl->actuator_mutex);
	CDBG("Exit\n");
	return rc;
}

static int32_t msm_actuator_get_subdev_id(struct msm_actuator_ctrl_t *a_ctrl,
	void *arg)
{
	uint32_t *subdev_id = (uint32_t *)arg;
	CDBG("Enter\n");
	if (!subdev_id) {
		pr_err("failed\n");
		return -EINVAL;
	}
	*subdev_id = a_ctrl->cam_name;
	CDBG("subdev_id %d\n", *subdev_id);
	CDBG("Exit\n");
	return 0;
}

static struct msm_camera_i2c_fn_t msm_sensor_cci_func_tbl = {
	.i2c_read = msm_camera_cci_i2c_read,
	.i2c_read_seq = msm_camera_cci_i2c_read_seq,
	.i2c_write = msm_camera_cci_i2c_write,
	.i2c_write_table = msm_camera_cci_i2c_write_table,
	.i2c_write_seq_table = msm_camera_cci_i2c_write_seq_table,
	.i2c_write_table_w_microdelay =
		msm_camera_cci_i2c_write_table_w_microdelay,
	.i2c_util = msm_sensor_cci_i2c_util,
};

static struct msm_camera_i2c_fn_t msm_sensor_qup_func_tbl = {
	.i2c_read = msm_camera_qup_i2c_read,
	.i2c_read_seq = msm_camera_qup_i2c_read_seq,
	.i2c_write = msm_camera_qup_i2c_write,
	.i2c_write_table = msm_camera_qup_i2c_write_table,
	.i2c_write_seq = msm_camera_qup_i2c_write_seq,
	.i2c_write_seq_table = msm_camera_qup_i2c_write_seq_table,
	.i2c_write_table_w_microdelay =
		msm_camera_qup_i2c_write_table_w_microdelay,
};

static int msm_actuator_open(struct v4l2_subdev *sd,
	struct v4l2_subdev_fh *fh) {
	int rc = 0;
	struct msm_actuator_ctrl_t *a_ctrl =  v4l2_get_subdevdata(sd);
	CDBG("Enter\n");
	if (!a_ctrl) {
		pr_err("failed\n");
		return -EINVAL;
	}
	if (a_ctrl->act_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_util(
			&a_ctrl->i2c_client, MSM_CCI_INIT);
		if (rc < 0)
			pr_err("cci_init failed\n");
	}
	CDBG("Exit\n");
	return rc;
}

static int msm_actuator_close(struct v4l2_subdev *sd,
	struct v4l2_subdev_fh *fh) {
	int rc = 0;
	struct msm_actuator_ctrl_t *a_ctrl =  v4l2_get_subdevdata(sd);
	CDBG("Enter\n");
	if (!a_ctrl) {
		pr_err("failed\n");
		return -EINVAL;
	}

	if (a_ctrl->act_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_util(
			&a_ctrl->i2c_client, MSM_CCI_RELEASE);
		if (rc < 0)
			pr_err("cci_init failed\n");
	}
	CDBG("Exit\n");
	return rc;
}

static const struct v4l2_subdev_internal_ops msm_actuator_internal_ops = {
	.open = msm_actuator_open,
	.close = msm_actuator_close,
};

static int32_t msm_actuator_i2c_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int rc = 0;
	struct msm_actuator_ctrl_t *act_ctrl_t = NULL;
	struct msm_actuator_info *act_info = NULL;
	CDBG("Enter\n");

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("i2c_check_functionality failed\n");
		goto probe_failure;
	}

	act_ctrl_t = (struct msm_actuator_ctrl_t *)(id->driver_data);
	CDBG("client = %x\n", (unsigned int) client);
	act_ctrl_t->i2c_client.client = client;
	/* Set device type as I2C */
	act_ctrl_t->act_device_type = MSM_CAMERA_I2C_DEVICE;
	act_ctrl_t->i2c_client.i2c_func_tbl = &msm_sensor_qup_func_tbl;
	act_info = (struct msm_actuator_info *)client->dev.platform_data;
	if (!act_info) {
		pr_err("%s:%d failed platform data NULL\n", __func__, __LINE__);
		rc = -EINVAL;
		goto probe_failure;
	}
	act_ctrl_t->cam_name = act_info->cam_name;

	/* Assign name for sub device */
	snprintf(act_ctrl_t->msm_sd.sd.name, sizeof(act_ctrl_t->msm_sd.sd.name),
		"%s", act_ctrl_t->i2c_driver->driver.name);

	/* Initialize sub device */
	v4l2_i2c_subdev_init(&act_ctrl_t->msm_sd.sd,
		act_ctrl_t->i2c_client.client,
		act_ctrl_t->act_v4l2_subdev_ops);
	v4l2_set_subdevdata(&act_ctrl_t->msm_sd.sd, act_ctrl_t);
	act_ctrl_t->msm_sd.sd.internal_ops = &msm_actuator_internal_ops;
	act_ctrl_t->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	media_entity_init(&act_ctrl_t->msm_sd.sd.entity, 0, NULL, 0);
	act_ctrl_t->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	act_ctrl_t->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_ACTUATOR;
	msm_sd_register(&act_ctrl_t->msm_sd);
	CDBG("succeeded\n");
	CDBG("Exit\n");

probe_failure:
	return rc;
}

static int32_t msm_actuator_platform_probe(struct platform_device *pdev)
{
	int32_t rc = 0;
	struct msm_camera_cci_client *cci_client = NULL;
	CDBG("Enter\n");

	if (!pdev->dev.of_node) {
		pr_err("of_node NULL\n");
		return -EINVAL;
	}

	rc = of_property_read_u32((&pdev->dev)->of_node, "cell-index",
		&pdev->id);
	CDBG("cell-index %d, rc %d\n", pdev->id, rc);
	if (rc < 0) {
		pr_err("failed rc %d\n", rc);
		return rc;
	}

	rc = of_property_read_u32((&pdev->dev)->of_node, "qcom,cci-master",
		&msm_actuator_t.cci_master);
	CDBG("qcom,cci-master %d, rc %d\n", msm_actuator_t.cci_master, rc);
	if (rc < 0) {
		pr_err("failed rc %d\n", rc);
		return rc;
	}

	msm_actuator_t.cam_name = pdev->id;

	/* Set platform device handle */
	msm_actuator_t.pdev = pdev;
	/* Set device type as platform device */
	msm_actuator_t.act_device_type = MSM_CAMERA_PLATFORM_DEVICE;
	msm_actuator_t.i2c_client.i2c_func_tbl = &msm_sensor_cci_func_tbl;
	msm_actuator_t.i2c_client.cci_client = kzalloc(sizeof(
		struct msm_camera_cci_client), GFP_KERNEL);
	if (!msm_actuator_t.i2c_client.cci_client) {
		pr_err("failed no memory\n");
		return -ENOMEM;
	}

	cci_client = msm_actuator_t.i2c_client.cci_client;
	cci_client->cci_subdev = msm_cci_get_subdev();
	v4l2_subdev_init(&msm_actuator_t.msm_sd.sd,
		msm_actuator_t.act_v4l2_subdev_ops);
	v4l2_set_subdevdata(&msm_actuator_t.msm_sd.sd, &msm_actuator_t);
	msm_actuator_t.msm_sd.sd.internal_ops = &msm_actuator_internal_ops;
	msm_actuator_t.msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	snprintf(msm_actuator_t.msm_sd.sd.name,
		ARRAY_SIZE(msm_actuator_t.msm_sd.sd.name), "msm_actuator");
	media_entity_init(&msm_actuator_t.msm_sd.sd.entity, 0, NULL, 0);
	msm_actuator_t.msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	msm_actuator_t.msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_ACTUATOR;
	msm_sd_register(&msm_actuator_t.msm_sd);
	CDBG("Exit\n");
	return rc;
}

static int32_t msm_actuator_power_up(struct msm_actuator_ctrl_t *a_ctrl)
{
	int rc = 0;
	CDBG("%s called\n", __func__);

	CDBG("vcm info: %x %x\n", a_ctrl->vcm_pwd,
		a_ctrl->vcm_enable);
	if (a_ctrl->vcm_enable) {
		rc = gpio_request(a_ctrl->vcm_pwd, "msm_actuator");
		if (!rc) {
			CDBG("Enable VCM PWD\n");
			gpio_direction_output(a_ctrl->vcm_pwd, 1);
		}
	}
	CDBG("Exit\n");
	return rc;
}

static const struct i2c_device_id msm_actuator_i2c_id[] = {
	{"msm_actuator", (kernel_ulong_t)&msm_actuator_t},
	{ }
};

static struct i2c_driver msm_actuator_i2c_driver = {
	.id_table = msm_actuator_i2c_id,
	.probe  = msm_actuator_i2c_probe,
	.remove = __exit_p(msm_actuator_i2c_remove),
	.driver = {
		.name = "msm_actuator",
	},
};

static const struct of_device_id msm_actuator_dt_match[] = {
	{.compatible = "qcom,actuator", .data = &msm_actuator_t},
	{}
};

MODULE_DEVICE_TABLE(of, msm_actuator_dt_match);

static struct platform_driver msm_actuator_platform_driver = {
	.driver = {
		.name = "qcom,actuator",
		.owner = THIS_MODULE,
		.of_match_table = msm_actuator_dt_match,
	},
};

static int __init msm_actuator_init_module(void)
{
	int32_t rc = 0;
	CDBG("Enter\n");
	rc = platform_driver_probe(msm_actuator_t.pdriver,
		msm_actuator_platform_probe);
	if (!rc)
		return rc;
	CDBG("%s:%d rc %d\n", __func__, __LINE__, rc);
	return i2c_add_driver(msm_actuator_t.i2c_driver);
}

static long msm_actuator_subdev_ioctl(struct v4l2_subdev *sd,
			unsigned int cmd, void *arg)
{
	struct msm_actuator_ctrl_t *a_ctrl = v4l2_get_subdevdata(sd);
	void __user *argp = (void __user *)arg;
	CDBG("Enter\n");
	CDBG("%s:%d a_ctrl %p argp %p\n", __func__, __LINE__, a_ctrl, argp);
	switch (cmd) {
	case VIDIOC_MSM_SENSOR_GET_SUBDEV_ID:
		return msm_actuator_get_subdev_id(a_ctrl, argp);
	case VIDIOC_MSM_ACTUATOR_CFG:
		return msm_actuator_config(a_ctrl, argp);
	default:
		return -ENOIOCTLCMD;
	}
}

static int32_t msm_actuator_power(struct v4l2_subdev *sd, int on)
{
	int rc = 0;
	struct msm_actuator_ctrl_t *a_ctrl = v4l2_get_subdevdata(sd);
	CDBG("Enter\n");
	mutex_lock(a_ctrl->actuator_mutex);
	if (on)
		rc = msm_actuator_power_up(a_ctrl);
	else
		rc = msm_actuator_power_down(a_ctrl);
	mutex_unlock(a_ctrl->actuator_mutex);
	CDBG("Exit\n");
	return rc;
}

static struct v4l2_subdev_core_ops msm_actuator_subdev_core_ops = {
	.ioctl = msm_actuator_subdev_ioctl,
	.s_power = msm_actuator_power,
};

static struct v4l2_subdev_ops msm_actuator_subdev_ops = {
	.core = &msm_actuator_subdev_core_ops,
};

static struct msm_actuator_ctrl_t msm_actuator_t = {
	.i2c_driver = &msm_actuator_i2c_driver,
	.pdriver = &msm_actuator_platform_driver,
	.act_v4l2_subdev_ops = &msm_actuator_subdev_ops,

	.curr_step_pos = 0,
	.curr_region_index = 0,
	.actuator_mutex = &msm_actuator_mutex,

};

static struct msm_actuator msm_vcm_actuator_table = {
	.act_type = ACTUATOR_VCM,
	.func_tbl = {
#ifdef CONFIG_SEKONIX_LENS_ACT
		.actuator_init_step_table =
			msm_actuator_init_step_table_use_eeprom,
#else
		.actuator_init_step_table =
			msm_actuator_init_default_step_table,
#endif
		.actuator_move_focus = msm_actuator_move_focus,
		.actuator_write_focus = msm_actuator_write_focus,
		.actuator_set_default_focus = msm_actuator_set_default_focus,
		.actuator_init_focus = msm_actuator_init_focus,
		.actuator_parse_i2c_params = msm_actuator_parse_i2c_params,
	},
};

static struct msm_actuator msm_piezo_actuator_table = {
	.act_type = ACTUATOR_PIEZO,
	.func_tbl = {
		.actuator_init_step_table = NULL,
		.actuator_move_focus = msm_actuator_piezo_move_focus,
		.actuator_write_focus = NULL,
		.actuator_set_default_focus =
			msm_actuator_piezo_set_default_focus,
		.actuator_init_focus = msm_actuator_init_focus,
		.actuator_parse_i2c_params = msm_actuator_parse_i2c_params,
	},
};

module_init(msm_actuator_init_module);
MODULE_DESCRIPTION("MSM ACTUATOR");
MODULE_LICENSE("GPL v2");
