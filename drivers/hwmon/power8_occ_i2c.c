/*
 * OCC HWMON driver - read IBM Power8 On Chip Controller sensor data via
 * i2c.
 *
 * Copyright 2015 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/device.h>


#define OCC_I2C_ADDR 0x50
#define OCC_I2C_NAME "occ-i2c"

#define OCC_DATA_MAX	4096 /* 4KB at most */
/* i2c read and write occ sensors */
#define I2C_READ_ERROR	1
#define I2C_WRITE_ERROR	2

/* Defined in POWER8 Processor Registers Specification */
/* To generate attn to OCC */
#define ATTN_DATA	0x0006B035
/* For BMC to read/write SRAM */
#define OCB_ADDRESS		0x0006B070
#define OCB_DATA		0x0006B075
#define OCB_STATUS_CONTROL_AND	0x0006B072
#define OCB_STATUS_CONTROL_OR	0x0006B073
/* See definition in:
 * https://github.com/open-power/docs/blob/master/occ/OCC_OpenPwr_FW_Interfaces.pdf
 */
#define OCC_COMMAND_ADDR	0xFFFF6000
#define OCC_RESPONSE_ADDR	0xFFFF7000

/* OCC sensor data format */
struct occ_sensor {
	uint16_t sensor_id;
	uint16_t value;
};

struct power_sensor {
	uint16_t sensor_id;
	uint32_t update_tag;
	uint32_t accumulator;
	uint16_t value;
};

struct caps_sensor {
	uint16_t curr_powercap;
	uint16_t curr_powerreading;
	uint16_t norm_powercap;
	uint16_t max_powercap;
	uint16_t min_powercap;
	uint16_t user_powerlimit;
};

struct sensor_data_block {
	uint8_t sensor_type[4];
	uint8_t reserved0;
	uint8_t sensor_format;
	uint8_t sensor_length;
	uint8_t num_of_sensors;
	struct occ_sensor *sensor;
	struct power_sensor *power;
	struct caps_sensor *caps;
};

struct occ_poll_header {
	uint8_t status;
	uint8_t ext_status;
	uint8_t occs_present;
	uint8_t config;
	uint8_t occ_state;
	uint8_t reserved0;
	uint8_t reserved1;
	uint8_t error_log_id;
	uint32_t error_log_addr_start;
	uint16_t error_log_length;
	uint8_t reserved2;
	uint8_t reserved3;
	uint8_t occ_code_level[16];
	uint8_t sensor_eye_catcher[6];
	uint8_t sensor_block_num;
	uint8_t sensor_data_version;
};

struct occ_response {
	uint8_t sequence_num;
	uint8_t cmd_type;
	uint8_t rtn_status;
	uint16_t data_length;
	struct occ_poll_header header;
	struct sensor_data_block *blocks;
	uint16_t chk_sum;
	int temp_block_id;
	int freq_block_id;
	int power_block_id;
	int caps_block_id;
};

/* data private to each client */
struct occ_drv_data {
	struct i2c_client	*client;
	struct device		*hwmon_dev;
	struct mutex		update_lock;
	bool			valid;
	unsigned long		last_updated;
	/* Minimum timer interval for sampling In jiffies */
	unsigned long		update_interval;
	unsigned long		occ_online;
	uint16_t		user_powercap;
	struct occ_response	occ_resp;
};

enum sensor_t {
	freq,
	temp,
	power,
	caps
};

static void deinit_occ_resp_buf(struct occ_response *p)
{
	int i;

	if (!p)
		return;

	if (!p->blocks)
		return;

	for (i = 0; i < p->header.sensor_block_num; i++) {
		kfree(p->blocks[i].sensor);
		kfree(p->blocks[i].power);
		kfree(p->blocks[i].caps);
	}

	kfree(p->blocks);

	memset(p, 0, sizeof(*p));
	p->freq_block_id = -1;
	p->temp_block_id = -1;
	p->power_block_id = -1;
	p->caps_block_id = -1;
}

static ssize_t occ_i2c_read(struct i2c_client *client, void *buf, size_t count)
{
	WARN_ON(count > OCC_DATA_MAX);

	dev_dbg(&client->dev, "i2c_read: reading %zu bytes @0x%x.\n",
		count, client->addr);
	return i2c_master_recv(client, buf, count);
}

static ssize_t occ_i2c_write(struct i2c_client *client, const void *buf,
				size_t count)
{
	WARN_ON(count > OCC_DATA_MAX);

	dev_dbg(&client->dev, "i2c_write: writing %zu bytes @0x%x.\n",
		count, client->addr);
	return i2c_master_send(client, buf, count);
}

/* read 8-byte value and put into data[offset] */
static int occ_getscomb(struct i2c_client *client, uint32_t address,
		uint8_t *data, int offset)
{
	uint32_t ret;
	char buf[8];
	int i;

	/* P8 i2c slave requires address to be shifted by 1 */
	address = address << 1;

	ret = occ_i2c_write(client, &address,
		sizeof(address));

	if (ret != sizeof(address))
		return -I2C_WRITE_ERROR;

	ret = occ_i2c_read(client, buf, sizeof(buf));
	if (ret != sizeof(buf))
		return -I2C_READ_ERROR;

	for (i = 0; i < 8; i++)
		data[offset + i] = buf[7 - i];

	return 0;
}

static int occ_putscom(struct i2c_client *client, uint32_t address,
		uint32_t data0, uint32_t data1)
{
	uint32_t buf[3];
	uint32_t ret;

	/* P8 i2c slave requires address to be shifted by 1 */
	address = address << 1;

	buf[0] = address;
	buf[1] = data1;
	buf[2] = data0;

	ret = occ_i2c_write(client, buf, sizeof(buf));
	if (ret != sizeof(buf))
		return I2C_WRITE_ERROR;

	return 0;
}

static void *occ_get_sensor_by_type(struct occ_response *resp, enum sensor_t t)
{
	void *sensor;

	if (!resp->blocks)
		return NULL;

	switch (t) {
	case temp:
		sensor = (resp->temp_block_id == -1) ? NULL :
			resp->blocks[resp->temp_block_id].sensor;
		break;
	case freq:
		sensor = (resp->freq_block_id == -1) ? NULL :
			resp->blocks[resp->freq_block_id].sensor;
		break;
	case power:
		sensor = (resp->power_block_id == -1) ? NULL :
			resp->blocks[resp->power_block_id].power;
		break;
	case caps:
		sensor = (resp->caps_block_id == -1) ? NULL :
			resp->blocks[resp->caps_block_id].caps;
		break;
	default:
		sensor = NULL;
		break;
	}

	return sensor;
}

static int occ_renew_sensor(struct occ_response *resp, uint8_t sensor_length,
	uint8_t num_of_sensors, enum sensor_t t, int block)
{
	void *sensor;
	int ret;

	sensor = occ_get_sensor_by_type(resp, t);

	/* empty sensor block, release older sensor data */
	if (num_of_sensors == 0 || sensor_length == 0) {
		kfree(sensor);
		return -1;
	}

	switch (t) {
	case temp:
		if (!sensor || num_of_sensors !=
			resp->blocks[resp->temp_block_id].num_of_sensors) {
			kfree(sensor);
			resp->blocks[block].sensor =
				kcalloc(num_of_sensors,
					sizeof(struct occ_sensor), GFP_KERNEL);
			if (!resp->blocks[block].sensor) {
				ret = -ENOMEM;
				goto err;
			}
		}
		break;
	case freq:
		if (!sensor || num_of_sensors !=
			resp->blocks[resp->freq_block_id].num_of_sensors) {
			kfree(sensor);
			resp->blocks[block].sensor =
				kcalloc(num_of_sensors,
					sizeof(struct occ_sensor), GFP_KERNEL);
			if (!resp->blocks[block].sensor) {
				ret = -ENOMEM;
				goto err;
			}
		}
		break;
	case power:
		if (!sensor || num_of_sensors !=
			resp->blocks[resp->power_block_id].num_of_sensors) {
			kfree(sensor);
			resp->blocks[block].power =
				kcalloc(num_of_sensors,
				sizeof(struct power_sensor), GFP_KERNEL);
			if (!resp->blocks[block].power) {
				ret = -ENOMEM;
				goto err;
			}
		}
		break;
	case caps:
		if (!sensor || num_of_sensors !=
			resp->blocks[resp->caps_block_id].num_of_sensors) {
			kfree(sensor);
			resp->blocks[block].caps =
				kcalloc(num_of_sensors,
					sizeof(struct caps_sensor), GFP_KERNEL);
			if (!resp->blocks[block].caps) {
				ret = -ENOMEM;
				goto err;
			}
		}
		break;
	default:
		sensor = NULL;
		break;
	}

	return 0;
err:
	deinit_occ_resp_buf(resp);
	return ret;
}

#define RESP_DATA_LENGTH	3
#define RESP_HEADER_OFFSET	5
#define SENSOR_STR_OFFSET	37
#define SENSOR_BLOCK_NUM_OFFSET	43
#define SENSOR_BLOCK_OFFSET	45

static inline uint16_t get_occdata_length(uint8_t *data)
{
	return be16_to_cpup((const __be16 *)&data[RESP_DATA_LENGTH]);
}

static int parse_occ_response(struct i2c_client *client,
		uint8_t *data, struct occ_response *resp)
{
	int b;
	int s;
	int ret;
	int dnum = SENSOR_BLOCK_OFFSET;
	struct occ_sensor *f_sensor;
	struct occ_sensor *t_sensor;
	struct power_sensor *p_sensor;
	struct caps_sensor *c_sensor;
	uint8_t sensor_block_num;
	uint8_t sensor_type[4];
	uint8_t sensor_format;
	uint8_t sensor_length;
	uint8_t num_of_sensors;

	/* check if the data is valid */
	if (strncmp(&data[SENSOR_STR_OFFSET], "SENSOR", 6) != 0) {
		dev_dbg(&client->dev,
			"ERROR: no SENSOR String in response\n");
		ret = -1;
		goto err;
	}

	sensor_block_num = data[SENSOR_BLOCK_NUM_OFFSET];
	if (sensor_block_num == 0) {
		dev_dbg(&client->dev, "ERROR: SENSOR block num is 0\n");
		ret = -1;
		goto err;
	}

	/* if sensor block has changed, re-malloc */
	if (sensor_block_num != resp->header.sensor_block_num) {
		deinit_occ_resp_buf(resp);
		resp->blocks = kcalloc(sensor_block_num,
			sizeof(struct sensor_data_block), GFP_KERNEL);
		if (!resp->blocks)
			return -ENOMEM;
	}

	memcpy(&resp->header, &data[RESP_HEADER_OFFSET], sizeof(resp->header));
	resp->header.error_log_addr_start =
		be32_to_cpu(resp->header.error_log_addr_start);
	resp->header.error_log_length =
		be16_to_cpu(resp->header.error_log_length);

	dev_dbg(&client->dev, "Reading %d sensor blocks\n",
		resp->header.sensor_block_num);
	for (b = 0; b < sensor_block_num; b++) {
		/* 8-byte sensor block head */
		strncpy(sensor_type, &data[dnum], 4);
		sensor_format = data[dnum+5];
		sensor_length = data[dnum+6];
		num_of_sensors = data[dnum+7];
		dnum = dnum + 8;

		dev_dbg(&client->dev,
			"sensor block[%d]: type: %s, num_of_sensors: %d\n",
			b, sensor_type, num_of_sensors);

		if (strncmp(sensor_type, "FREQ", 4) == 0) {
			ret = occ_renew_sensor(resp, sensor_length,
				num_of_sensors, freq, b);
			if (ret)
				continue;

			resp->freq_block_id = b;
			for (s = 0; s < num_of_sensors; s++) {
				f_sensor = &resp->blocks[b].sensor[s];
				f_sensor->sensor_id =
					be16_to_cpup((const __be16 *)
							&data[dnum]);
				f_sensor->value = be16_to_cpup((const __be16 *)
							&data[dnum+2]);
				dev_dbg(&client->dev,
					"sensor[%d]-[%d]: id: %u, value: %u\n",
					b, s, f_sensor->sensor_id,
					f_sensor->value);
				dnum = dnum + sensor_length;
			}
		} else if (strncmp(sensor_type, "TEMP", 4) == 0) {
			ret = occ_renew_sensor(resp, sensor_length,
				num_of_sensors, temp, b);
			if (ret)
				continue;

			resp->temp_block_id = b;
			for (s = 0; s < num_of_sensors; s++) {
				t_sensor = &resp->blocks[b].sensor[s];
				t_sensor->sensor_id =
					be16_to_cpup((const __be16 *)
							&data[dnum]);
				t_sensor->value = be16_to_cpup((const __be16 *)
							&data[dnum+2]);
				dev_dbg(&client->dev,
					"sensor[%d]-[%d]: id: %u, value: %u\n",
					b, s, t_sensor->sensor_id,
					t_sensor->value);
				dnum = dnum + sensor_length;
			}
		} else if (strncmp(sensor_type, "POWR", 4) == 0) {
			ret = occ_renew_sensor(resp, sensor_length,
				num_of_sensors, power, b);
			if (ret)
				continue;

			resp->power_block_id = b;
			for (s = 0; s < num_of_sensors; s++) {
				p_sensor = &resp->blocks[b].power[s];
				p_sensor->sensor_id =
					be16_to_cpup((const __be16 *)
							&data[dnum]);
				p_sensor->update_tag =
					be32_to_cpup((const __be32 *)
							&data[dnum+2]);
				p_sensor->accumulator =
					be32_to_cpup((const __be32 *)
							&data[dnum+6]);
				p_sensor->value = be16_to_cpup((const __be16 *)
							&data[dnum+10]);

				dev_dbg(&client->dev,
					"sensor[%d]-[%d]: id: %u, value: %u\n",
					b, s, p_sensor->sensor_id,
					p_sensor->value);

				dnum = dnum + sensor_length;
			}
		} else if (strncmp(sensor_type, "CAPS", 4) == 0) {
			ret = occ_renew_sensor(resp, sensor_length,
				num_of_sensors, caps, b);
			if (ret)
				continue;

			resp->caps_block_id = b;
			for (s = 0; s < num_of_sensors; s++) {
				c_sensor = &resp->blocks[b].caps[s];
				c_sensor->curr_powercap =
					be16_to_cpup((const __be16 *)
							&data[dnum]);
				c_sensor->curr_powerreading =
					be16_to_cpup((const __be16 *)
							&data[dnum+2]);
				c_sensor->norm_powercap =
					be16_to_cpup((const __be16 *)
							&data[dnum+4]);
				c_sensor->max_powercap =
					be16_to_cpup((const __be16 *)
							&data[dnum+6]);
				c_sensor->min_powercap =
					be16_to_cpup((const __be16 *)
							&data[dnum+8]);
				c_sensor->user_powerlimit =
					be16_to_cpup((const __be16 *)
							&data[dnum+10]);

				dnum = dnum + sensor_length;
				dev_dbg(&client->dev, "CAPS sensor #%d:\n", s);
				dev_dbg(&client->dev, "curr_powercap is %x\n",
					c_sensor->curr_powercap);
				dev_dbg(&client->dev,
					"curr_powerreading is %x\n",
					c_sensor->curr_powerreading);
				dev_dbg(&client->dev, "norm_powercap is %x\n",
					c_sensor->norm_powercap);
				dev_dbg(&client->dev, "max_powercap is %x\n",
					c_sensor->max_powercap);
				dev_dbg(&client->dev, "min_powercap is %x\n",
					c_sensor->min_powercap);
				dev_dbg(&client->dev, "user_powerlimit is %x\n",
					c_sensor->user_powerlimit);
			}

		} else {
			dev_dbg(&client->dev,
				"ERROR: sensor type %s not supported\n",
				resp->blocks[b].sensor_type);
			ret = -1;
			goto err;
		}

		strncpy(resp->blocks[b].sensor_type, sensor_type, 4);
		resp->blocks[b].sensor_format = sensor_format;
		resp->blocks[b].sensor_length = sensor_length;
		resp->blocks[b].num_of_sensors = num_of_sensors;
	}

	return 0;
err:
	deinit_occ_resp_buf(resp);
	return ret;
}


/* Refer to OCC interface document for OCC command format
 * https://github.com/open-power/docs/blob/master/occ/OCC_OpenPwr_FW_Interfaces.pdf
 */
static uint8_t occ_send_cmd(struct i2c_client *client, uint8_t seq,
		uint8_t type, uint16_t length, uint8_t *data, uint8_t *resp)
{
	uint32_t cmd1, cmd2;
	uint16_t checksum;
	int i;

	length = cpu_to_le16(length);
	cmd1 = (seq << 24) | (type << 16) | length;
	memcpy(&cmd2, data, length);
	cmd2 <<= ((4 - length) * 8);

	/* checksum: sum of every bytes of cmd1, cmd2 */
	checksum = 0;
	for (i = 0; i < 4; i++)
		checksum += (cmd1 >> (i * 8)) & 0xFF;
	for (i = 0; i < 4; i++)
		checksum += (cmd2 >> (i * 8)) & 0xFF;
	cmd2 |= checksum << ((2 - length) * 8);

	/* Init OCB */
	occ_putscom(client, OCB_STATUS_CONTROL_OR,  0x08000000, 0x00000000);
	occ_putscom(client, OCB_STATUS_CONTROL_AND, 0xFBFFFFFF, 0xFFFFFFFF);

	/* Send command */
	occ_putscom(client, OCB_ADDRESS, OCC_COMMAND_ADDR, 0x00000000);
	occ_putscom(client, OCB_ADDRESS, OCC_COMMAND_ADDR, 0x00000000);
	occ_putscom(client, OCB_DATA, cmd1, cmd2);

	/* Trigger attention */
	occ_putscom(client, ATTN_DATA, 0x01010000, 0x00000000);

	/* Get response data */
	occ_putscom(client, OCB_ADDRESS, OCC_RESPONSE_ADDR, 0x00000000);
	occ_getscomb(client, OCB_DATA, resp, 0);

	/* return status */
	return resp[2];
}

static int occ_get_all(struct i2c_client *client, struct occ_response *occ_resp)
{
	uint8_t *occ_data;
	uint16_t num_bytes;
	int i;
	int ret;
	uint8_t poll_cmd_data;

	poll_cmd_data = 0x10;

	/*
	 * TODO: fetch header, and then allocate the rest of the buffer based
	 * on the header size. Assuming the OCC has a fixed sized header
	 */
	occ_data = devm_kzalloc(&client->dev, OCC_DATA_MAX, GFP_KERNEL);

	ret = occ_send_cmd(client, 0, 0, 1, &poll_cmd_data, occ_data);
	if (ret) {
		dev_err(&client->dev, "ERROR: OCC Poll: 0x%x\n", ret);
		ret = -EINVAL;
		goto out;
	}

	num_bytes = get_occdata_length(occ_data);

	dev_dbg(&client->dev, "OCC data length: %d\n", num_bytes);

	if (num_bytes > OCC_DATA_MAX) {
		dev_dbg(&client->dev, "ERROR: OCC data length must be < 4KB\n");
		ret = -EINVAL;
		goto out;
	}

	if (num_bytes <= 0) {
		dev_dbg(&client->dev, "ERROR: OCC data length is zero\n");
		ret = -EINVAL;
		goto out;
	}

	/* read remaining data */
	for (i = 8; i < num_bytes + 8; i = i + 8)
		occ_getscomb(client, OCB_DATA, occ_data, i);

	ret = parse_occ_response(client, occ_data, occ_resp);

out:
	devm_kfree(&client->dev, occ_data);
	return ret;
}


static int occ_update_device(struct device *dev)
{
	struct occ_drv_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret = 0;

	mutex_lock(&data->update_lock);

	if (time_after(jiffies, data->last_updated + data->update_interval)
	    || !data->valid) {
		data->valid = 1;
		ret = occ_get_all(client, &data->occ_resp);
		if (ret)
			data->valid = 0;
		data->last_updated = jiffies;
	}
	mutex_unlock(&data->update_lock);

	return ret;
}


static void *occ_get_sensor(struct device *hwmon_dev, enum sensor_t t)
{
	struct device *dev = hwmon_dev->parent;
	struct occ_drv_data *data = dev_get_drvdata(dev);
	int ret;

	ret = occ_update_device(dev);
	if (ret != 0) {
		dev_dbg(dev, "ERROR: cannot get occ sensor data: %d\n", ret);
		return NULL;
	}

	return occ_get_sensor_by_type(&data->occ_resp, t);
}

/* sysfs attributes for hwmon */
static ssize_t show_occ_temp_input(struct device *hwmon_dev,
		struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	int n = attr->index;
	struct occ_sensor *sensor;
	int val;

	sensor = occ_get_sensor(hwmon_dev, temp);
	if (!sensor)
		val = -1;
	else
		/* in millidegree Celsius */
		val = sensor[n].value * 1000;

	return snprintf(buf, PAGE_SIZE - 1, "%d\n", val);
}

static ssize_t show_occ_temp_label(struct device *hwmon_dev,
		struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	int n = attr->index;
	struct occ_sensor *sensor;
	int val;

	sensor = occ_get_sensor(hwmon_dev, temp);
	if (!sensor)
		val = -1;
	else
		val = sensor[n].sensor_id;

	return snprintf(buf, PAGE_SIZE - 1, "%d\n", val);
}

static ssize_t show_occ_power_label(struct device *hwmon_dev,
		struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	int n = attr->index;
	struct power_sensor *sensor;
	int val;

	sensor = occ_get_sensor(hwmon_dev, power);
	if (!sensor)
		val = -1;
	else
		val = sensor[n].sensor_id;

	return snprintf(buf, PAGE_SIZE - 1, "%d\n", val);
}


static ssize_t show_occ_power_input(struct device *hwmon_dev,
		struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	int n = attr->index;
	struct power_sensor *sensor;
	int val;

	sensor = occ_get_sensor(hwmon_dev, power);
	if (!sensor)
		val = -1;
	else
		val = sensor[n].value;

	return snprintf(buf, PAGE_SIZE - 1, "%d\n", val);

}


static ssize_t show_occ_freq_label(struct device *hwmon_dev,
		struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	int n = attr->index;
	struct occ_sensor *sensor;
	int val;

	sensor = occ_get_sensor(hwmon_dev, freq);
	if (!sensor)
		val = -1;
	else
		val = sensor[n].sensor_id;

	return snprintf(buf, PAGE_SIZE - 1, "%d\n", val);
}


static ssize_t show_occ_freq_input(struct device *hwmon_dev,
		struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	int n = attr->index;
	struct occ_sensor *sensor;
	int val;

	sensor = occ_get_sensor(hwmon_dev, freq);
	if (!sensor)
		val = -1;
	else
		val = sensor[n].value;

	return snprintf(buf, PAGE_SIZE - 1, "%d\n", val);
}

static ssize_t show_occ_caps(struct device *hwmon_dev,
		struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute_2 *attr = to_sensor_dev_attr_2(da);
	int nr = attr->nr;
	int n = attr->index;
	struct caps_sensor *sensor;
	int val;

	sensor = occ_get_sensor(hwmon_dev, caps);
	if (!sensor) {
		val = -1;
		return snprintf(buf, PAGE_SIZE - 1, "%d\n", val);
	}

	switch (nr) {
	case 0:
		val = sensor[n].curr_powercap;
		break;
	case 1:
		val = sensor[n].curr_powerreading;
		break;
	case 2:
		val = sensor[n].norm_powercap;
		break;
	case 3:
		val = sensor[n].max_powercap;
		break;
	case 4:
		val = sensor[n].min_powercap;
		break;
	case 5:
		val = sensor[n].user_powerlimit;
		break;
	default:
		val = -1;
	}

	return snprintf(buf, PAGE_SIZE - 1, "%d\n", val);
}

static struct sensor_device_attribute temp_input[] = {
	SENSOR_ATTR(temp1_input, S_IRUGO, show_occ_temp_input, NULL, 0),
	SENSOR_ATTR(temp2_input, S_IRUGO, show_occ_temp_input, NULL, 1),
	SENSOR_ATTR(temp3_input, S_IRUGO, show_occ_temp_input, NULL, 2),
	SENSOR_ATTR(temp4_input, S_IRUGO, show_occ_temp_input, NULL, 3),
	SENSOR_ATTR(temp5_input, S_IRUGO, show_occ_temp_input, NULL, 4),
	SENSOR_ATTR(temp6_input, S_IRUGO, show_occ_temp_input, NULL, 5),
	SENSOR_ATTR(temp7_input, S_IRUGO, show_occ_temp_input, NULL, 6),
	SENSOR_ATTR(temp8_input, S_IRUGO, show_occ_temp_input, NULL, 7),
	SENSOR_ATTR(temp9_input, S_IRUGO, show_occ_temp_input, NULL, 8),
	SENSOR_ATTR(temp10_input, S_IRUGO, show_occ_temp_input, NULL, 9),
	SENSOR_ATTR(temp11_input, S_IRUGO, show_occ_temp_input, NULL, 10),
	SENSOR_ATTR(temp12_input, S_IRUGO, show_occ_temp_input, NULL, 11),
	SENSOR_ATTR(temp13_input, S_IRUGO, show_occ_temp_input, NULL, 12),
	SENSOR_ATTR(temp14_input, S_IRUGO, show_occ_temp_input, NULL, 13),
	SENSOR_ATTR(temp15_input, S_IRUGO, show_occ_temp_input, NULL, 14),
	SENSOR_ATTR(temp16_input, S_IRUGO, show_occ_temp_input, NULL, 15),
	SENSOR_ATTR(temp17_input, S_IRUGO, show_occ_temp_input, NULL, 16),
	SENSOR_ATTR(temp18_input, S_IRUGO, show_occ_temp_input, NULL, 17),
	SENSOR_ATTR(temp19_input, S_IRUGO, show_occ_temp_input, NULL, 18),
	SENSOR_ATTR(temp20_input, S_IRUGO, show_occ_temp_input, NULL, 19),
	SENSOR_ATTR(temp21_input, S_IRUGO, show_occ_temp_input, NULL, 20),
	SENSOR_ATTR(temp22_input, S_IRUGO, show_occ_temp_input, NULL, 21),
};

static struct sensor_device_attribute temp_label[] = {
	SENSOR_ATTR(temp1_label, S_IRUGO, show_occ_temp_label, NULL, 0),
	SENSOR_ATTR(temp2_label, S_IRUGO, show_occ_temp_label, NULL, 1),
	SENSOR_ATTR(temp3_label, S_IRUGO, show_occ_temp_label, NULL, 2),
	SENSOR_ATTR(temp4_label, S_IRUGO, show_occ_temp_label, NULL, 3),
	SENSOR_ATTR(temp5_label, S_IRUGO, show_occ_temp_label, NULL, 4),
	SENSOR_ATTR(temp6_label, S_IRUGO, show_occ_temp_label, NULL, 5),
	SENSOR_ATTR(temp7_label, S_IRUGO, show_occ_temp_label, NULL, 6),
	SENSOR_ATTR(temp8_label, S_IRUGO, show_occ_temp_label, NULL, 7),
	SENSOR_ATTR(temp9_label, S_IRUGO, show_occ_temp_label, NULL, 8),
	SENSOR_ATTR(temp10_label, S_IRUGO, show_occ_temp_label, NULL, 9),
	SENSOR_ATTR(temp11_label, S_IRUGO, show_occ_temp_label, NULL, 10),
	SENSOR_ATTR(temp12_label, S_IRUGO, show_occ_temp_label, NULL, 11),
	SENSOR_ATTR(temp13_label, S_IRUGO, show_occ_temp_label, NULL, 12),
	SENSOR_ATTR(temp14_label, S_IRUGO, show_occ_temp_label, NULL, 13),
	SENSOR_ATTR(temp15_label, S_IRUGO, show_occ_temp_label, NULL, 14),
	SENSOR_ATTR(temp16_label, S_IRUGO, show_occ_temp_label, NULL, 15),
	SENSOR_ATTR(temp17_label, S_IRUGO, show_occ_temp_label, NULL, 16),
	SENSOR_ATTR(temp18_label, S_IRUGO, show_occ_temp_label, NULL, 17),
	SENSOR_ATTR(temp19_label, S_IRUGO, show_occ_temp_label, NULL, 18),
	SENSOR_ATTR(temp20_label, S_IRUGO, show_occ_temp_label, NULL, 19),
	SENSOR_ATTR(temp21_label, S_IRUGO, show_occ_temp_label, NULL, 20),
	SENSOR_ATTR(temp22_label, S_IRUGO, show_occ_temp_label, NULL, 21),

};

#define TEMP_UNIT_ATTRS(X)                      \
{	&temp_input[X].dev_attr.attr,           \
	&temp_label[X].dev_attr.attr,          \
	NULL                                    \
}

/* 10-core CPU, occ has 22 temp sensors, more socket, more sensors */
static struct attribute *occ_temp_attr[][3] = {
	TEMP_UNIT_ATTRS(0),
	TEMP_UNIT_ATTRS(1),
	TEMP_UNIT_ATTRS(2),
	TEMP_UNIT_ATTRS(3),
	TEMP_UNIT_ATTRS(4),
	TEMP_UNIT_ATTRS(5),
	TEMP_UNIT_ATTRS(6),
	TEMP_UNIT_ATTRS(7),
	TEMP_UNIT_ATTRS(8),
	TEMP_UNIT_ATTRS(9),
	TEMP_UNIT_ATTRS(10),
	TEMP_UNIT_ATTRS(11),
	TEMP_UNIT_ATTRS(12),
	TEMP_UNIT_ATTRS(13),
	TEMP_UNIT_ATTRS(14),
	TEMP_UNIT_ATTRS(15),
	TEMP_UNIT_ATTRS(16),
	TEMP_UNIT_ATTRS(17),
	TEMP_UNIT_ATTRS(18),
	TEMP_UNIT_ATTRS(19),
	TEMP_UNIT_ATTRS(20),
	TEMP_UNIT_ATTRS(21),
};

static const struct attribute_group occ_temp_attr_group[] = {
	{ .attrs = occ_temp_attr[0] },
	{ .attrs = occ_temp_attr[1] },
	{ .attrs = occ_temp_attr[2] },
	{ .attrs = occ_temp_attr[3] },
	{ .attrs = occ_temp_attr[4] },
	{ .attrs = occ_temp_attr[5] },
	{ .attrs = occ_temp_attr[6] },
	{ .attrs = occ_temp_attr[7] },
	{ .attrs = occ_temp_attr[8] },
	{ .attrs = occ_temp_attr[9] },
	{ .attrs = occ_temp_attr[10] },
	{ .attrs = occ_temp_attr[11] },
	{ .attrs = occ_temp_attr[12] },
	{ .attrs = occ_temp_attr[13] },
	{ .attrs = occ_temp_attr[14] },
	{ .attrs = occ_temp_attr[15] },
	{ .attrs = occ_temp_attr[16] },
	{ .attrs = occ_temp_attr[17] },
	{ .attrs = occ_temp_attr[18] },
	{ .attrs = occ_temp_attr[19] },
	{ .attrs = occ_temp_attr[20] },
	{ .attrs = occ_temp_attr[21] },
};


static struct sensor_device_attribute freq_input[] = {
	SENSOR_ATTR(freq1_input, S_IRUGO, show_occ_freq_input, NULL, 0),
	SENSOR_ATTR(freq2_input, S_IRUGO, show_occ_freq_input, NULL, 1),
	SENSOR_ATTR(freq3_input, S_IRUGO, show_occ_freq_input, NULL, 2),
	SENSOR_ATTR(freq4_input, S_IRUGO, show_occ_freq_input, NULL, 3),
	SENSOR_ATTR(freq5_input, S_IRUGO, show_occ_freq_input, NULL, 4),
	SENSOR_ATTR(freq6_input, S_IRUGO, show_occ_freq_input, NULL, 5),
	SENSOR_ATTR(freq7_input, S_IRUGO, show_occ_freq_input, NULL, 6),
	SENSOR_ATTR(freq8_input, S_IRUGO, show_occ_freq_input, NULL, 7),
	SENSOR_ATTR(freq9_input, S_IRUGO, show_occ_freq_input, NULL, 8),
	SENSOR_ATTR(freq10_input, S_IRUGO, show_occ_freq_input, NULL, 9),
};

static struct sensor_device_attribute freq_label[] = {
	SENSOR_ATTR(freq1_label, S_IRUGO, show_occ_freq_label, NULL, 0),
	SENSOR_ATTR(freq2_label, S_IRUGO, show_occ_freq_label, NULL, 1),
	SENSOR_ATTR(freq3_label, S_IRUGO, show_occ_freq_label, NULL, 2),
	SENSOR_ATTR(freq4_label, S_IRUGO, show_occ_freq_label, NULL, 3),
	SENSOR_ATTR(freq5_label, S_IRUGO, show_occ_freq_label, NULL, 4),
	SENSOR_ATTR(freq6_label, S_IRUGO, show_occ_freq_label, NULL, 5),
	SENSOR_ATTR(freq7_label, S_IRUGO, show_occ_freq_label, NULL, 6),
	SENSOR_ATTR(freq8_label, S_IRUGO, show_occ_freq_label, NULL, 7),
	SENSOR_ATTR(freq9_label, S_IRUGO, show_occ_freq_label, NULL, 8),
	SENSOR_ATTR(freq10_label, S_IRUGO, show_occ_freq_label, NULL, 9),

};

#define FREQ_UNIT_ATTRS(X)                      \
{	&freq_input[X].dev_attr.attr,           \
	&freq_label[X].dev_attr.attr,          \
	NULL                                    \
}

/* 10-core CPU, occ has 22 freq sensors, more socket, more sensors */
static struct attribute *occ_freq_attr[][3] = {
	FREQ_UNIT_ATTRS(0),
	FREQ_UNIT_ATTRS(1),
	FREQ_UNIT_ATTRS(2),
	FREQ_UNIT_ATTRS(3),
	FREQ_UNIT_ATTRS(4),
	FREQ_UNIT_ATTRS(5),
	FREQ_UNIT_ATTRS(6),
	FREQ_UNIT_ATTRS(7),
	FREQ_UNIT_ATTRS(8),
	FREQ_UNIT_ATTRS(9),
};

static const struct attribute_group occ_freq_attr_group[] = {
	{ .attrs = occ_freq_attr[0] },
	{ .attrs = occ_freq_attr[1] },
	{ .attrs = occ_freq_attr[2] },
	{ .attrs = occ_freq_attr[3] },
	{ .attrs = occ_freq_attr[4] },
	{ .attrs = occ_freq_attr[5] },
	{ .attrs = occ_freq_attr[6] },
	{ .attrs = occ_freq_attr[7] },
	{ .attrs = occ_freq_attr[8] },
	{ .attrs = occ_freq_attr[9] },
};

static struct sensor_device_attribute_2 caps_curr_powercap[] = {
	SENSOR_ATTR_2(caps_curr_powercap, S_IRUGO, show_occ_caps, NULL, 0, 0),
};
static struct sensor_device_attribute_2 caps_curr_powerreading[] = {
	SENSOR_ATTR_2(caps_curr_powerreading, S_IRUGO,
		show_occ_caps, NULL, 1, 0),
};
static struct sensor_device_attribute_2 caps_norm_powercap[] = {
	SENSOR_ATTR_2(caps_norm_powercap, S_IRUGO, show_occ_caps,
		NULL, 2, 0),
};
static struct sensor_device_attribute_2 caps_max_powercap[] = {
	SENSOR_ATTR_2(caps_max_powercap, S_IRUGO, show_occ_caps, NULL, 3, 0),
};
static struct sensor_device_attribute_2 caps_min_powercap[] = {
	SENSOR_ATTR_2(caps_min_powercap, S_IRUGO, show_occ_caps, NULL, 4, 0),
};
static struct sensor_device_attribute_2 caps_user_powerlimit[] = {
	SENSOR_ATTR_2(caps_user_powerlimit, S_IRUGO, show_occ_caps, NULL, 5, 0),
};
#define CAPS_UNIT_ATTRS(X)                      \
{	&caps_curr_powercap[X].dev_attr.attr,           \
	&caps_curr_powerreading[X].dev_attr.attr,           \
	&caps_norm_powercap[X].dev_attr.attr,           \
	&caps_max_powercap[X].dev_attr.attr,           \
	&caps_min_powercap[X].dev_attr.attr,           \
	&caps_user_powerlimit[X].dev_attr.attr,           \
	NULL                                    \
}

/* 10-core CPU, occ has 1 caps sensors */
static struct attribute *occ_caps_attr[][7] = {
	CAPS_UNIT_ATTRS(0),
};
static const struct attribute_group occ_caps_attr_group[] = {
	{ .attrs = occ_caps_attr[0] },
};

static struct sensor_device_attribute power_input[] = {
	SENSOR_ATTR(power1_input, S_IRUGO, show_occ_power_input, NULL, 0),
	SENSOR_ATTR(power2_input, S_IRUGO, show_occ_power_input, NULL, 1),
	SENSOR_ATTR(power3_input, S_IRUGO, show_occ_power_input, NULL, 2),
	SENSOR_ATTR(power4_input, S_IRUGO, show_occ_power_input, NULL, 3),
	SENSOR_ATTR(power5_input, S_IRUGO, show_occ_power_input, NULL, 4),
	SENSOR_ATTR(power6_input, S_IRUGO, show_occ_power_input, NULL, 5),
	SENSOR_ATTR(power7_input, S_IRUGO, show_occ_power_input, NULL, 6),
	SENSOR_ATTR(power8_input, S_IRUGO, show_occ_power_input, NULL, 7),
	SENSOR_ATTR(power9_input, S_IRUGO, show_occ_power_input, NULL, 8),
	SENSOR_ATTR(power10_input, S_IRUGO, show_occ_power_input, NULL, 9),
	SENSOR_ATTR(power11_input, S_IRUGO, show_occ_power_input, NULL, 10),
};

static struct sensor_device_attribute power_label[] = {
	SENSOR_ATTR(power1_label, S_IRUGO, show_occ_power_label, NULL, 0),
	SENSOR_ATTR(power2_label, S_IRUGO, show_occ_power_label, NULL, 1),
	SENSOR_ATTR(power3_label, S_IRUGO, show_occ_power_label, NULL, 2),
	SENSOR_ATTR(power4_label, S_IRUGO, show_occ_power_label, NULL, 3),
	SENSOR_ATTR(power5_label, S_IRUGO, show_occ_power_label, NULL, 4),
	SENSOR_ATTR(power6_label, S_IRUGO, show_occ_power_label, NULL, 5),
	SENSOR_ATTR(power7_label, S_IRUGO, show_occ_power_label, NULL, 6),
	SENSOR_ATTR(power8_label, S_IRUGO, show_occ_power_label, NULL, 7),
	SENSOR_ATTR(power9_label, S_IRUGO, show_occ_power_label, NULL, 8),
	SENSOR_ATTR(power10_label, S_IRUGO, show_occ_power_label, NULL, 9),
	SENSOR_ATTR(power11_label, S_IRUGO, show_occ_power_label, NULL, 10),
};

#define POWER_UNIT_ATTRS(X)                      \
{	&power_input[X].dev_attr.attr,           \
	&power_label[X].dev_attr.attr,          \
	NULL                                    \
}

/* 10-core CPU, occ has 11 power sensors, more socket, more sensors */
static struct attribute *occ_power_attr[][3] = {
	POWER_UNIT_ATTRS(0),
	POWER_UNIT_ATTRS(1),
	POWER_UNIT_ATTRS(2),
	POWER_UNIT_ATTRS(3),
	POWER_UNIT_ATTRS(4),
	POWER_UNIT_ATTRS(5),
	POWER_UNIT_ATTRS(6),
	POWER_UNIT_ATTRS(7),
	POWER_UNIT_ATTRS(8),
	POWER_UNIT_ATTRS(9),
	POWER_UNIT_ATTRS(10),
};

static const struct attribute_group occ_power_attr_group[] = {
	{ .attrs = occ_power_attr[0] },
	{ .attrs = occ_power_attr[1] },
	{ .attrs = occ_power_attr[2] },
	{ .attrs = occ_power_attr[3] },
	{ .attrs = occ_power_attr[4] },
	{ .attrs = occ_power_attr[5] },
	{ .attrs = occ_power_attr[6] },
	{ .attrs = occ_power_attr[7] },
	{ .attrs = occ_power_attr[8] },
	{ .attrs = occ_power_attr[9] },
	{ .attrs = occ_power_attr[10] },
};

static ssize_t show_update_interval(struct device *hwmon_dev,
				struct device_attribute *attr, char *buf)
{
	struct device *dev = hwmon_dev->parent;
	struct occ_drv_data *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE - 1, "%u\n",
		jiffies_to_msecs(data->update_interval));
}

static ssize_t set_update_interval(struct device *hwmon_dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct device *dev = hwmon_dev->parent;
	struct occ_drv_data *data = dev_get_drvdata(dev);
	unsigned long val;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return err;

	data->update_interval = msecs_to_jiffies(val);
	return count;
}
static DEVICE_ATTR(update_interval, S_IWUSR | S_IRUGO,
		show_update_interval, set_update_interval);

static ssize_t show_name(struct device *hwmon_dev,
				struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE - 1, "%s\n", OCC_I2C_NAME);
}
static DEVICE_ATTR(name, S_IRUGO, show_name, NULL);

static ssize_t show_user_powercap(struct device *hwmon_dev,
				struct device_attribute *attr, char *buf)
{
	struct device *dev = hwmon_dev->parent;
	struct occ_drv_data *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE - 1, "%u\n", data->user_powercap);
}


static ssize_t set_user_powercap(struct device *hwmon_dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct device *dev = hwmon_dev->parent;
	struct occ_drv_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	uint16_t val;
	uint8_t resp[8];
	int err;

	err = kstrtou16(buf, 10, &val);
	if (err)
		return err;

	dev_dbg(dev, "set user powercap to: %u\n", val);
	val = cpu_to_le16(val);
	err = occ_send_cmd(client, 0, 0x22, 2, (uint8_t *)&val, resp);
	if (err != 0) {
		dev_dbg(dev,
			"ERROR: Set User Powercap: wrong return status: %x\n",
			err);
		if (err == 0x13)
			dev_info(dev,
				"ERROR: set invalid powercap value: %x\n", val);
		return -EINVAL;
	}
	data->user_powercap = val;
	return count;
}
static DEVICE_ATTR(user_powercap, S_IWUSR | S_IRUGO,
		show_user_powercap, set_user_powercap);

static void occ_remove_sysfs_files(struct device *dev)
{
	int i;

	if (!dev)
		return;

	device_remove_file(dev, &dev_attr_update_interval);
	device_remove_file(dev, &dev_attr_name);
	device_remove_file(dev, &dev_attr_user_powercap);

	for (i = 0; i < ARRAY_SIZE(occ_temp_attr_group); i++)
		sysfs_remove_group(&dev->kobj, &occ_temp_attr_group[i]);

	for (i = 0; i < ARRAY_SIZE(occ_freq_attr_group); i++)
		sysfs_remove_group(&dev->kobj, &occ_freq_attr_group[i]);

	for (i = 0; i < ARRAY_SIZE(occ_power_attr_group); i++)
		sysfs_remove_group(&dev->kobj, &occ_power_attr_group[i]);

	for (i = 0; i < ARRAY_SIZE(occ_caps_attr_group); i++)
		sysfs_remove_group(&dev->kobj, &occ_caps_attr_group[i]);
}

static int occ_create_hwmon_attribute(struct device *dev)
{
	/* The sensor number varies for different
	 * platform depending on core number. We'd better
	 * create them dynamically
	 */
	struct occ_drv_data *drv_data = dev_get_drvdata(dev);
	int i;
	int num_of_sensors;
	int ret;
	struct occ_response *rsp;

	/* get sensor number from occ. */
	rsp = &drv_data->occ_resp;

	rsp->freq_block_id = -1;
	rsp->temp_block_id = -1;
	rsp->power_block_id = -1;
	rsp->caps_block_id = -1;

	ret = occ_update_device(dev);
	if (ret != 0) {
		dev_dbg(dev, "ERROR: cannot get occ sensor data: %d\n", ret);
		return ret;
	}

	if (!rsp->blocks)
		return -1;

	ret = device_create_file(drv_data->hwmon_dev,
			&dev_attr_name);
	if (ret)
		goto error;

	ret = device_create_file(drv_data->hwmon_dev,
			&dev_attr_update_interval);
	if (ret)
		goto error;

	/* temp sensors */
	if (rsp->temp_block_id >= 0) {
		num_of_sensors =
			rsp->blocks[rsp->temp_block_id].num_of_sensors;
		for (i = 0; i < num_of_sensors; i++) {
			ret = sysfs_create_group(&drv_data->hwmon_dev->kobj,
				&occ_temp_attr_group[i]);
			if (ret) {
				dev_dbg(dev,
					"ERROR: cannot create sysfs entry\n");
				goto error;
			}
		}
	}

	/* freq sensors */
	if (rsp->freq_block_id >= 0) {
		num_of_sensors =
			rsp->blocks[rsp->freq_block_id].num_of_sensors;
		for (i = 0; i < num_of_sensors; i++) {
			ret = sysfs_create_group(&drv_data->hwmon_dev->kobj,
				&occ_freq_attr_group[i]);
			if (ret) {
				dev_dbg(dev,
					"ERROR: cannot create sysfs entry\n");
				goto error;
			}
		}
	}

	/* power sensors */
	if (rsp->power_block_id >= 0) {
		num_of_sensors =
			rsp->blocks[rsp->power_block_id].num_of_sensors;
		for (i = 0; i < num_of_sensors; i++) {
			ret = sysfs_create_group(&drv_data->hwmon_dev->kobj,
				&occ_power_attr_group[i]);
			if (ret) {
				dev_dbg(dev,
					"ERROR: cannot create sysfs entry\n");
				goto error;
			}
		}
	}

	/* caps sensors */
	if (rsp->caps_block_id >= 0) {
		num_of_sensors =
			rsp->blocks[rsp->caps_block_id].num_of_sensors;
		for (i = 0; i < num_of_sensors; i++) {
			ret = sysfs_create_group(&drv_data->hwmon_dev->kobj,
				&occ_caps_attr_group[i]);
			if (ret) {
				dev_dbg(dev,
					"ERROR: cannot create sysfs entry\n");
				goto error;
			}
		}
		/* only for master OCC */
		ret = device_create_file(drv_data->hwmon_dev,
			&dev_attr_user_powercap);
		if (ret)
			goto error;
	}

	return 0;
error:
	occ_remove_sysfs_files(drv_data->hwmon_dev);
	return ret;
}

static ssize_t show_occ_online(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct occ_drv_data *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE - 1, "%lu\n", data->occ_online);
}

static ssize_t set_occ_online(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct occ_drv_data *data = dev_get_drvdata(dev);
	unsigned long val;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return err;

	if (val == 1) {
		if (data->occ_online == 1)
			return count;

		/* populate hwmon sysfs attr using sensor data */
		dev_dbg(dev, "occ register hwmon @0x%x\n", data->client->addr);

		data->hwmon_dev = hwmon_device_register(dev);
		if (IS_ERR(data->hwmon_dev))
			return PTR_ERR(data->hwmon_dev);

		err = occ_create_hwmon_attribute(dev);
		if (err) {
			hwmon_device_unregister(data->hwmon_dev);
			return err;
		}
		data->hwmon_dev->parent = dev;
		dev_dbg(dev, "%s: sensor '%s'\n",
			dev_name(data->hwmon_dev), data->client->name);
	} else if (val == 0) {
		if (data->occ_online == 0)
			return count;

		occ_remove_sysfs_files(data->hwmon_dev);
		hwmon_device_unregister(data->hwmon_dev);
		data->hwmon_dev = NULL;
	} else
		return -EINVAL;

	data->occ_online = val;
	return count;
}

static DEVICE_ATTR(online, S_IWUSR | S_IRUGO,
		show_occ_online, set_occ_online);

static int occ_create_sysfs_attribute(struct device *dev)
{
	/* create a sysfs attribute, to indicate whether OCC is active */
	return device_create_file(dev, &dev_attr_online);
}


/* device probe and removal */

enum occ_type {
	occ_id,
};

static int occ_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct occ_drv_data *data;

	data = devm_kzalloc(dev, sizeof(struct occ_drv_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	i2c_set_clientdata(client, data);
	mutex_init(&data->update_lock);
	data->update_interval = HZ;

	occ_create_sysfs_attribute(dev);

	dev_info(dev, "occ i2c driver ready: i2c addr@0x%x\n", client->addr);

	return 0;
}

static int occ_remove(struct i2c_client *client)
{
	struct occ_drv_data *data = i2c_get_clientdata(client);

	/* free allocated sensor memory */
	deinit_occ_resp_buf(&data->occ_resp);

	device_remove_file(&client->dev, &dev_attr_online);

	if (!data->hwmon_dev)
		return 0;

	occ_remove_sysfs_files(data->hwmon_dev);
	hwmon_device_unregister(data->hwmon_dev);
	return 0;
}

/* used by old-style board info. */
static const struct i2c_device_id occ_ids[] = {
	{ OCC_I2C_NAME, occ_id, },
	{ /* LIST END */ }
};
MODULE_DEVICE_TABLE(i2c, occ_ids);

/* use by device table */
static const struct of_device_id i2c_occ_of_match[] = {
	{.compatible = "ibm,occ-i2c"},
	{},
};
MODULE_DEVICE_TABLE(of, i2c_occ_of_match);

/* i2c-core uses i2c-detect() to detect device in bellow address list.
 *  If exists, address will be assigned to client.
 * It is also possible to read address from device table.
 */
static const unsigned short normal_i2c[] = {0x50, 0x51, I2C_CLIENT_END };

static struct i2c_driver occ_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver = {
		.name	= OCC_I2C_NAME,
		.pm	= NULL,
		.of_match_table = i2c_occ_of_match,
	},
	.probe		= occ_probe,
	.remove		= occ_remove,
	.id_table       = occ_ids,
	.address_list	= normal_i2c,
};

module_i2c_driver(occ_driver);

MODULE_AUTHOR("Li Yi <shliyi@cn.ibm.com>");
MODULE_DESCRIPTION("BMC OCC hwmon driver");
MODULE_LICENSE("GPL");
