/*
 * An hwmon driver for accton as45xx Power Module
 *
 * Copyright (C) 2014 Accton Technology Corporation.
 * Brandon Chuang <brandon_chuang@accton.com.tw>
 *
 * Based on ad7414.c
 * Copyright 2006 Stefan Roese <sr at denx.de>, DENX Software Engineering
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/dmi.h>

#define DRVNAME "as45xx_psu"

#define PSU_STATUS_I2C_ADDR			0x42

#define AS4500_PSU_STATUS_I2C_ADDR	0x42
#define AS4581_PSU_STATUS_I2C_ADDR	0x61

#define MODEL_NAME_LEN				13
#define MODEL_NAME_REG_OFFSET		0x15

#define SERIAL_NUM_LEN				19
#define SERIAL_NUM_REG_OFFSET		0x3d

#define IS_PRESENT(id, value)		(!(value & BIT(id)))
#define IS_POWER_GOOD(id, value)	(!!(value & BIT(id+4)))

#define FAN_DIR_LEN 3
const char FAN_DIR_F2B[] = "F2B";
const char FAN_DIR_B2F[] = "B2F";

static ssize_t show_status(struct device *dev, struct device_attribute *da, char *buf);
static ssize_t show_string(struct device *dev, struct device_attribute *da, char *buf);
extern int as45xx_cpld_read(unsigned short cpld_addr, u8 reg);

/* Addresses scanned
 */
static const unsigned short normal_i2c[] = { I2C_CLIENT_END };

/* Each client has this additional data
 */
struct as45xx_psu_data {
	struct device *hwmon_dev;
	struct mutex update_lock;
	char valid;		   /* !=0 if registers are valid */
	unsigned long last_updated;	/* In jiffies */
	u8  index;  /* PSU index */
	u8  status[2]; /* Status(present/power_good) register read from CPLD */
	char model_name[MODEL_NAME_LEN+1]; /* Model name, read from eeprom */
	char serial[SERIAL_NUM_LEN+1]; /* Serial number, read from eeprom*/
	char fan_dir[FAN_DIR_LEN+1];
	unsigned short cpld_addr;
};

u8 status_regs[] = { 0x3, 0x22 };

static struct as45xx_psu_data *as45xx_psu_update_device(struct device *dev);

enum as45xx_psu_sysfs_attributes {
	PSU_POWER_GOOD,
	PSU_PRESENT,
	PSU_MODEL_NAME,
	PSU_SERIAL_NUMBER,
	PSU_FAN_DIR
};

/* sysfs attributes for hwmon
 */
static SENSOR_DEVICE_ATTR(psu_present,	S_IRUGO, show_status,	NULL, PSU_PRESENT);
static SENSOR_DEVICE_ATTR(psu_model_name, S_IRUGO, show_string,	NULL, PSU_MODEL_NAME);
static SENSOR_DEVICE_ATTR(psu_power_good, S_IRUGO, show_status,	NULL, PSU_POWER_GOOD);
static SENSOR_DEVICE_ATTR(psu_serial_number, S_IRUGO, show_string,	NULL, PSU_SERIAL_NUMBER);
static SENSOR_DEVICE_ATTR(psu_fan_dir, S_IRUGO, show_string, NULL, PSU_FAN_DIR);

static struct attribute *as45xx_psu_attributes[] = {
	&sensor_dev_attr_psu_present.dev_attr.attr,
	&sensor_dev_attr_psu_model_name.dev_attr.attr,
	&sensor_dev_attr_psu_power_good.dev_attr.attr,
	&sensor_dev_attr_psu_serial_number.dev_attr.attr,
	&sensor_dev_attr_psu_fan_dir.dev_attr.attr,
	NULL
};

static ssize_t show_status(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct as45xx_psu_data *data = i2c_get_clientdata(client);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	u8 status = 0;

	mutex_lock(&data->update_lock);

	data = as45xx_psu_update_device(dev);
	if (!data->valid) {
		mutex_unlock(&data->update_lock);
		return sprintf(buf, "0\n");
	}

	if (attr->index == PSU_PRESENT) {
		status = IS_PRESENT(data->index, data->status[PSU_PRESENT]);
	}
	else { /* PSU_POWER_GOOD */
		status = IS_POWER_GOOD(data->index, data->status[PSU_POWER_GOOD]);
	}

	mutex_unlock(&data->update_lock);
	return sprintf(buf, "%d\n", status);
}

static ssize_t show_string(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct as45xx_psu_data *data = i2c_get_clientdata(client);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	char *ptr = NULL;
	int ret = 0;

	mutex_lock(&data->update_lock);

	data = as45xx_psu_update_device(dev);
	if (!data->valid) {
		ret = -EIO;
		goto exit;
	}

	switch (attr->index) {
	case PSU_MODEL_NAME:
		ptr = data->model_name;
		break;
	case PSU_SERIAL_NUMBER:
		ptr = data->serial;
		break;
	case PSU_FAN_DIR:
		ptr = data->fan_dir;
		break;
	default:
		ret = -EINVAL;
		goto exit;
	}

	ret = sprintf(buf, "%s\n", ptr);

exit:
	mutex_unlock(&data->update_lock);
	return ret;
}

static const struct attribute_group as45xx_psu_group = {
	.attrs = as45xx_psu_attributes,
};

static int as45xx_psu_probe(struct i2c_client *client,
			const struct i2c_device_id *dev_id)
{
	struct as45xx_psu_data *data;
	int status;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE)) {
		status = -EIO;
		goto exit;
	}

	data = kzalloc(sizeof(struct as45xx_psu_data), GFP_KERNEL);
	if (!data) {
		status = -ENOMEM;
		goto exit;
	}

	/* Detect CPLD address */
	status = as45xx_cpld_read(AS4500_PSU_STATUS_I2C_ADDR, 0);
	data->cpld_addr = (status >= 0) ? AS4500_PSU_STATUS_I2C_ADDR : AS4581_PSU_STATUS_I2C_ADDR;

	data->valid = 0;
	data->index = dev_id->driver_data;
	mutex_init(&data->update_lock);
	i2c_set_clientdata(client, data);

	/* Register sysfs hooks */
	status = sysfs_create_group(&client->dev.kobj, &as45xx_psu_group);
	if (status) {
		goto exit_free;
	}

	dev_info(&client->dev, "chip found\n");
	return 0;

exit_free:
	kfree(data);
exit:
	return status;
}

static int as45xx_psu_remove(struct i2c_client *client)
{
	struct as45xx_psu_data *data = i2c_get_clientdata(client);

	sysfs_remove_group(&client->dev.kobj, &as45xx_psu_group);
	kfree(data);

	return 0;
}

enum psu_index
{
	as45xx_psu1,
	as45xx_psu2
};

static const struct i2c_device_id as45xx_psu_id[] = {
	{ "as45xx_psu1", as45xx_psu1 },
	{ "as45xx_psu2", as45xx_psu2 },
	{}
};
MODULE_DEVICE_TABLE(i2c, as45xx_psu_id);

static struct i2c_driver as45xx_psu_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver = {
		.name	 = DRVNAME,
	},
	.probe		= as45xx_psu_probe,
	.remove	   = as45xx_psu_remove,
	.id_table	 = as45xx_psu_id,
	.address_list = normal_i2c,
};

static int as45xx_psu_read_byte(struct i2c_client *client, u8 command, u8 *data)
{
	int status = 0;
	int retry_count = 5;

	while (retry_count) {
		status = i2c_smbus_read_byte_data(client, command);
		if (unlikely(status < 0)) {
			msleep(10);
			retry_count--;
			continue;
		}

		break;
	}

	if (unlikely(status < 0)) {
		dev_dbg(&client->dev, "sfp read byte data failed, command(0x%2x), data(0x%2x)\r\n", command, status);
		goto abort;
	}

	*data  = (u8)status;

abort:
	return status;
}

static int as45xx_psu_read_bytes(struct i2c_client *client, u8 command, u8 *data,
			  int data_len)
{
	int ret = 0;

	while (data_len) {
		ssize_t status;

		status = as45xx_psu_read_byte(client, command, data);
		if (status <= 0) {
			ret = status;
			break;
		}

		data += 1;
		command  += 1;
		data_len -= 1;
	}

	return ret;
}

static struct as45xx_psu_data *as45xx_psu_update_device(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct as45xx_psu_data *data = i2c_get_clientdata(client);

	if (time_after(jiffies, data->last_updated + HZ + HZ / 2)
		|| !data->valid) {
		int i, status;

		dev_dbg(&client->dev, "Starting as45xx update\n");
		data->valid = 0;

		/* Read psu status */
		for (i = 0; i < ARRAY_SIZE(status_regs); i++) {
			status = as45xx_cpld_read(data->cpld_addr, status_regs[i]);
			if (status < 0) {
				dev_dbg(&client->dev, "cpld reg (0x%x) err %d\n", data->cpld_addr, status);
				goto exit;
			}
			else {
				data->status[i] = status;
			}
		}

		memset(data->model_name, 0, sizeof(data->model_name));
		memset(data->serial, 0, sizeof(data->serial));
		memset(data->fan_dir, 0, sizeof(data->fan_dir));

		if (IS_POWER_GOOD(data->index, data->status[PSU_POWER_GOOD])) {
			/* Read model name */
			status = as45xx_psu_read_bytes(client, MODEL_NAME_REG_OFFSET, data->model_name,
											   ARRAY_SIZE(data->model_name)-1);
			if (status < 0) {
				data->model_name[0] = '\0';
				dev_dbg(&client->dev, "unable to read model name from (0x%x)\n", client->addr);
				goto exit;
			}

			/* Read serial number */
			status = as45xx_psu_read_bytes(client, SERIAL_NUM_REG_OFFSET, data->serial,
											   ARRAY_SIZE(data->serial)-1);
			if (status < 0) {
				data->serial[0] = '\0';
				dev_dbg(&client->dev, "unable to read serial number from (0x%x)\n", client->addr);
				goto exit;
			}
			else {
				data->serial[ARRAY_SIZE(data->serial)-1] = '\0';
			}

			if (strncmp(data->model_name, "G1482-1600WNA", ARRAY_SIZE(data->model_name)-1) == 0)
				memcpy(data->fan_dir, FAN_DIR_F2B, sizeof(FAN_DIR_F2B));
			else
				data->fan_dir[0] = '\0';
		}

		data->last_updated = jiffies;
		data->valid = 1;
	}

exit:
	return data;
}

module_i2c_driver(as45xx_psu_driver);

MODULE_AUTHOR("Brandon Chuang <brandon_chuang@accton.com.tw>");
MODULE_DESCRIPTION("as45xx_psu driver");
MODULE_LICENSE("GPL");
