/*
 * @file drivers/misc/dmt10.c
 * @brief DMT g-sensor Linux device driver
 * @author Domintech Technology Co., Ltd (http://www.domintech.com.tw)
 * @version 1.01
 *
 * @section LICENSE
 *
 *  Copyright 2012 Domintech Technology Co., Ltd
 *
 * 	This software is licensed under the terms of the GNU General Public
 * 	License version 2, as published by the Free Software Foundation, and
 * 	may be copied, distributed, and modified under those terms.
 *
 * 	This program is distributed in the hope that it will be useful,
 * 	but WITHOUT ANY WARRANTY; without even the implied warranty of
 * 	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * 	GNU General Public License for more details.
 *
 *  V1.00	D10 First Release									date 2012/09/21
 *  V1.01	static struct dmt_data devdata change t o *s_dmt	date 2012/10/15
 *			Memory optimization
 */
#include "dmt10.h"
#include <linux/module.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/earlysuspend.h>
#include <linux/wakelock.h>
#include <asm/uaccess.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/clk.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/string.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#define DMT_DEBUG_DATA	
static unsigned int interval;
void gsensor_write_offset_to_file(void);
void gsensor_read_offset_from_file(void);
char OffsetFileName[] = "/data/misc/dmt/offset.txt";	/* FILE offset.txt */
static raw_data offset;
static struct dmt_data *s_dmt;

/* Addresses to scan */
static union{
	unsigned short dirty_addr_buf[2];
	const unsigned short normal_i2c[2];
}u_i2c_addr = {{0x00},};
static __u32 twi_id = 0;

static int device_init(void);
static void device_exit(void);

static int device_open(struct inode*, struct file*);
//static ssize_t device_write(struct file*, const char*, size_t, loff_t*);
//static ssize_t device_read(struct file*, char*, size_t, loff_t*);
static long device_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
static int device_close(struct inode*, struct file*);

static int device_i2c_suspend(struct i2c_client *client, pm_message_t mesg);
static int device_i2c_resume(struct i2c_client *client);
static int __devinit device_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id);
static int __devexit device_i2c_remove(struct i2c_client *client);
void device_i2c_read_xyz(struct i2c_client *client, s16 *xyz);
static int device_i2c_rxdata(struct i2c_client *client, unsigned char *rxDat, int length);
static int device_i2c_txdata(struct i2c_client *client, unsigned char *txData, int length);

static int DMT_GetOpenStatus(void){	
	dmtprintk(KERN_INFO "start active=%d\n",s_dmt->active.counter);
	wait_event_interruptible(s_dmt->open_wq, (atomic_read(&s_dmt->active) != 0));
	return 0;
}

static int DMT_GetCloseStatus(void){
	dmtprintk(KERN_INFO "start active=%d\n",s_dmt->active.counter);
	wait_event_interruptible(s_dmt->open_wq, (atomic_read(&s_dmt->active) <= 0));
	return 0;
}

/**
 * gsensor_fetch_sysconfig_para - get config info from sysconfig.fex file.
 * return value:  
 *                    = 0; success;
 *                    < 0; err
 */
static int gsensor_fetch_sysconfig_para(void)
{
	int ret = -1;
	int device_used = -1;								
	__u32 twi_addr = 0;
	char name[I2C_NAME_SIZE];
	script_parser_value_type_t type = SCIRPT_PARSER_VALUE_TYPE_STRING;
		
	printk("========%s===================\n", __func__);
	 
	if(SCRIPT_PARSER_OK != (ret = script_parser_fetch("gsensor_para", "gsensor_used", &device_used, 1))){
	                pr_err("%s: script_parser_fetch err.ret = %d. \n", __func__, ret);
	                goto script_parser_fetch_err;
	}
	if(1 == device_used){
		if(SCRIPT_PARSER_OK != script_parser_fetch_ex("gsensor_para", "gsensor_name", (int *)(&name), &type, sizeof(name)/sizeof(int))){
			pr_err("%s: line: %d script_parser_fetch err. \n", __func__, __LINE__);
			goto script_parser_fetch_err;
		}
		if(strcmp(SENSOR_NAME, name)){
			pr_err("%s: name %s does not match SENSOR_NAME. \n", __func__, name);
			pr_err(SENSOR_NAME);
			//ret = 1;
			return ret;
		}
		if(SCRIPT_PARSER_OK != script_parser_fetch("gsensor_para", "gsensor_twi_addr", &twi_addr, sizeof(twi_addr)/sizeof(__u32))){
			pr_err("%s: line: %d: script_parser_fetch err. \n", name, __LINE__);
			goto script_parser_fetch_err;
		}
		u_i2c_addr.dirty_addr_buf[0] = twi_addr;
		u_i2c_addr.dirty_addr_buf[1] = I2C_CLIENT_END;
		printk("%s: after: gsensor_twi_addr is 0x%x, dirty_addr_buf: 0x%hx. dirty_addr_buf[1]: 0x%hx \n", \
			__func__, twi_addr, u_i2c_addr.dirty_addr_buf[0], u_i2c_addr.dirty_addr_buf[1]);

		if(SCRIPT_PARSER_OK != script_parser_fetch("gsensor_para", "gsensor_twi_id", &twi_id, 1)){
			pr_err("%s: script_parser_fetch err. \n", name);
			goto script_parser_fetch_err;
		}
		printk("%s: twi_id is %d. \n", __func__, twi_id);
		
		ret = 0;
		
	}else{
		pr_err("%s: gsensor_unused. \n",  __func__);
		ret = -1;
	}

	return ret;

script_parser_fetch_err:
	pr_notice("=========script_parser_fetch_err============\n");
	return ret;

}

/**
 * gsensor_detect - Device detection callback for automatic device creation
 * return value:  
 *                    = 0; success;
 *                    < 0; err
 */
static int gsensor_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;

	if(twi_id == adapter->nr){
		pr_info("%s: Detected chip %s at adapter %d, address 0x%02x\n",
			 __func__, SENSOR_NAME, i2c_adapter_id(adapter), client->addr);

		strlcpy(info->type, SENSOR_NAME, I2C_NAME_SIZE);
		return 0;
	}else{
		return -ENODEV;
	}
}

static void DMT_sysfs_update_active_status(int en){
	unsigned long dmt_delay;
	if(en){
		dmt_delay=msecs_to_jiffies(atomic_read(&s_dmt->delay));
		if(dmt_delay<1)
			dmt_delay=1;

		dmtprintk(KERN_INFO "schedule_delayed_work start with delay time=%lu\n",dmt_delay);
		schedule_delayed_work(&s_dmt->delaywork,dmt_delay);
	}
	else 
		cancel_delayed_work_sync(&s_dmt->delaywork);
}

static bool get_value_as_int(char const *buf, size_t size, int *value){
	long tmp;
	if (size == 0)
		return false;
	/* maybe text format value */
	if ((buf[0] == '0') && (size > 1)) {
		if ((buf[1] == 'x') || (buf[1] == 'X')) {
			/* hexadecimal format */
			if (0 != strict_strtol(buf, 16, &tmp))
				return false;
		} else {
			/* octal format */
			if (0 != strict_strtol(buf, 8, &tmp))
				return false;
		}
	} else {
		/* decimal format */
		if (0 != strict_strtol(buf, 10, &tmp))
			return false;
	}

	if (tmp > INT_MAX)
		return false;

	*value = tmp;
	return true;
}
static bool get_value_as_int64(char const *buf, size_t size, long long *value)
{
	long long tmp;
	if (size == 0)
		return false;
	/* maybe text format value */
	if ((buf[0] == '0') && (size > 1)) {
		if ((buf[1] == 'x') || (buf[1] == 'X')) {
			/* hexadecimal format */
			if (0 != strict_strtoll(buf, 16, &tmp))
				return false;
		} else {
			/* octal format */
			if (0 != strict_strtoll(buf, 8, &tmp))
				return false;
		}
	} else {
		/* decimal format */
		if (0 != strict_strtoll(buf, 10, &tmp))
			return false;
	}

	if (tmp > LLONG_MAX)
		return false;

	*value = tmp;
	return true;
}

static ssize_t DMT_enable_acc_show(struct device *dev, struct device_attribute *attr, char *buf){
	char str[2][16]={"ACC enable OFF","ACC enable ON"};
	int flag;
	flag=atomic_read(&s_dmt->enable); 	
	return sprintf(buf, "%s\n", str[flag]);
}

static ssize_t DMT_enable_acc_store( struct device *dev, struct device_attribute *attr, char const *buf, size_t count){
	int en;
	dmtprintk(KERN_INFO "%s:buf=%x %x\n",__func__,buf[0],buf[1]);
	if (false == get_value_as_int(buf, count, &en))
		return -EINVAL;

	en = en ? 1 : 0;
	atomic_set(&s_dmt->enable,en);
	DMT_sysfs_update_active_status(en);
	return 1;
}

static ssize_t DMT_delay_acc_show( struct device *dev, struct device_attribute *attr, char *buf){
	return sprintf(buf, "%d\n", atomic_read(&s_dmt->delay));
}

static ssize_t DMT_delay_acc_store( struct device *dev, struct device_attribute *attr,char const *buf, size_t count){
	long long data;
	if (false == get_value_as_int64(buf, count, &data))
		return -EINVAL;
	atomic_set(&s_dmt->delay, (unsigned int) data);
	dmtprintk(KERN_INFO "Driver attribute set delay =%lld\n",data);
	return count;
}

static struct device_attribute DMT_attributes[] = {
	__ATTR(enable_acc, 0660, DMT_enable_acc_show, DMT_enable_acc_store),
	__ATTR(delay_acc,  0660, DMT_delay_acc_show,  DMT_delay_acc_store),
	__ATTR_NULL,
};

static char const *const ACCELEMETER_CLASS_NAME = "accelemeter";
static char const *const GSENSOR_DEVICE_NAME = "dmard06";
static char const *const device_link_name = "i2c";
static dev_t const dmt_device_dev_t = MKDEV(MISC_MAJOR, 240);

/***** dmt sysfs functions ******************************************/
static int create_device_attributes(struct device *dev,	struct device_attribute *attrs){
	int i;
	int err = 0;
	for (i = 0 ; NULL != attrs[i].attr.name ; ++i) {
		err = device_create_file(dev, &attrs[i]);
		if (0 != err)
			break;
	}

	if (0 != err) {
		for (; i >= 0 ; --i)
			device_remove_file(dev, &attrs[i]);
	}
	return err;
}

static void remove_device_attributes(struct device *dev, struct device_attribute *attrs){
	int i;
	for (i = 0 ; NULL != attrs[i].attr.name ; ++i)
		device_remove_file(dev, &attrs[i]);
}

static int create_sysfs_interfaces(struct dmt_data *dmt){
	int err;
	if (NULL == dmt)
		return -EINVAL;

	err = 0;
	dmt->class = class_create(THIS_MODULE, ACCELEMETER_CLASS_NAME);
	if (IS_ERR(dmt->class)) {
		err = PTR_ERR(dmt->class);
		goto exit_class_create_failed;
	}

	dmt->class_dev = device_create(
						dmt->class,
						NULL,
						dmt_device_dev_t,
						dmt,
						GSENSOR_DEVICE_NAME);
	if (IS_ERR(dmt->class_dev)) {
		err = PTR_ERR(dmt->class_dev);
		goto exit_class_device_create_failed;
	}

	err = sysfs_create_link(
			&dmt->class_dev->kobj,
			&dmt->client->dev.kobj,
			device_link_name);
	if (0 > err)
		goto exit_sysfs_create_link_failed;

	err = create_device_attributes(
			dmt->class_dev,
			DMT_attributes);
	if (0 > err)
		goto exit_device_attributes_create_failed;
#if 0
	err = create_device_binary_attributes(
			&dmt->class_dev->kobj,
			dmt_bin_attributes);
	if (0 > err)
		goto exit_device_binary_attributes_create_failed;
#endif

	return err;

#if 0
exit_device_binary_attributes_create_failed:
	remove_device_attributes(dmt->class_dev, dmt_attributes);
#endif
exit_device_attributes_create_failed:
	sysfs_remove_link(&dmt->class_dev->kobj, device_link_name);
exit_sysfs_create_link_failed:
	device_destroy(dmt->class, dmt_device_dev_t);
exit_class_device_create_failed:
	dmt->class_dev = NULL;
	class_destroy(dmt->class);
exit_class_create_failed:
	dmt->class = NULL;
	return err;
}

static void remove_sysfs_interfaces(struct dmt_data *dmt)
{
	if (NULL == dmt)
		return;

	if (NULL != dmt->class_dev) {

		remove_device_attributes(
			dmt->class_dev,
			DMT_attributes);
		sysfs_remove_link(
			&dmt->class_dev->kobj,
			device_link_name);
		dmt->class_dev = NULL;
	}
	if (NULL != dmt->class) {
		device_destroy(
			dmt->class,
			dmt_device_dev_t);
		class_destroy(dmt->class);
		dmt->class = NULL;
	}
}

int input_init(void){
	int err=0;
	s_dmt->input=input_allocate_device();
	if (!s_dmt->input)
	{
		printk("input device allocate ERROR !!\n");
		return -ENOMEM;
	}
	else
	  printk("input device allocate Success !!\n");
	/* Setup input device */
	set_bit(EV_ABS, s_dmt->input->evbit);
	/* Accelerometer [-78.5, 78.5]m/s2 in Q16 */
	input_set_abs_params(s_dmt->input, ABS_X, -5144576, 5144576, 0, 0);
	input_set_abs_params(s_dmt->input, ABS_Y, -5144576, 5144576, 0, 0);
	input_set_abs_params(s_dmt->input, ABS_Z, -5144576, 5144576, 0, 0);

	/* Set InputDevice Name */
	s_dmt->input->name = INPUT_NAME_ACC;

	/* Register */
	err = input_register_device(s_dmt->input);
	if (err) {
	  printk("input_register_device ERROR !!\n");
		input_free_device(s_dmt->input);
		return err;
	}
	  printk("input_register_device SUCCESS %d !! \n",err);

	return err;
}

int gsensor_calibrate(void)
{	
	raw_data avg;
	int i, j;
	long xyz_acc[SENSOR_DATA_SIZE];   
  	s16 xyz[SENSOR_DATA_SIZE];
		
	/* initialize the accumulation buffer */
  	for(i = 0; i < SENSOR_DATA_SIZE; ++i) 
		xyz_acc[i] = 0;

	for(i = 0; i < AVG_NUM; i++) {      
		device_i2c_read_xyz(s_dmt->client, (s16 *)&xyz);
		for(j = 0; j < SENSOR_DATA_SIZE; ++j) 
			xyz_acc[j] += xyz[j];
  	}
	/* calculate averages */
  	for(i = 0; i < SENSOR_DATA_SIZE; ++i) 
		avg.v[i] = (s16) (xyz_acc[i] / AVG_NUM);
		
	if(avg.v[2] < 0){
		offset.u.x =  avg.v[0] ;    
		offset.u.y =  avg.v[1] ;
		offset.u.z =  avg.v[2] + DEFAULT_SENSITIVITY;
		return CONFIG_GSEN_CALIBRATION_GRAVITY_ON_Z_POSITIVE;
	}
	else{	
		offset.u.x =  avg.v[0] ;    
		offset.u.y =  avg.v[1] ;
		offset.u.z =  avg.v[2] - DEFAULT_SENSITIVITY;
		return CONFIG_GSEN_CALIBRATION_GRAVITY_ON_Z_NEGATIVE;
	}
	return 0;
}

int gsensor_reset(struct i2c_client *client){
	unsigned char buffer[7], buffer2[2];
	/* 1. check D10 , VALUE_STADR = 0x55 , VALUE_STAINT = 0xAA */
	buffer[0] = REG_STADR;
	buffer2[0] = REG_STAINT;
	
	device_i2c_rxdata(client, buffer, 2);
	device_i2c_rxdata(client, buffer2, 2);
		
	if( buffer[0] == VALUE_STADR || buffer2[0] == VALUE_STAINT){
		dmtprintk(KERN_INFO " REG_STADR_VALUE = %d , REG_STAINT_VALUE = %d\n", buffer[0], buffer2[0]);
		dmtprintk(KERN_INFO " %s DMT_DEVICE_NAME registered I2C driver!\n",__FUNCTION__);
	}
	else{
		dmtprintk(KERN_INFO " %s gsensor I2C err @@@ REG_STADR_VALUE = %d , REG_STAINT_VALUE = %d \n", __func__, buffer[0], buffer2[0]);
		return -1;
	}
	/* 2. Powerdown reset */
	buffer[0] = REG_PD;
	buffer[1] = VALUE_PD_RST;
	device_i2c_txdata(client, buffer, 2);
	/* 3. ACTR => Standby mode => Download OTP to parameter reg => Standby mode => Reset data path => Standby mode */
	buffer[0] = REG_ACTR;
	buffer[1] = MODE_Standby;
	buffer[2] = MODE_ReadOTP;
	buffer[3] = MODE_Standby;
	buffer[4] = MODE_ResetDataPath;
	buffer[5] = MODE_Standby;
	device_i2c_txdata(client, buffer, 6);
	/* 4. OSCA_EN = 1 ,TSTO = b'000(INT1 = normal, TEST0 = normal) */
	buffer[0] = REG_MISC2;
	buffer[1] = VALUE_MISC2_OSCA_EN;
	device_i2c_txdata(client, buffer, 2);
	/* 5. AFEN = 1(AFE will powerdown after ADC) */
	buffer[0] = REG_AFEM;
	buffer[1] = VALUE_AFEM_AFEN_Normal;	
	buffer[2] = VALUE_CKSEL_ODR_100;	
	buffer[3] = VALUE_INTC;	
	buffer[4] = VALUE_TAPNS_Ave_2;
	buffer[5] = 0x00;	// DLYC, no delay timing
	buffer[6] = 0x07;	// INTD=1 (push-pull), INTA=1 (active high), AUTOT=1 (enable T)
	device_i2c_txdata(client, buffer, 7);
	/* 6. Activation mode */
	buffer[0] = REG_ACTR;
	buffer[1] = MODE_Active;
	device_i2c_txdata(client, buffer, 2);
	return 0;
}

void gsensor_set_offset(int val[3]){
	int i;
	for(i = 0; i < SENSOR_DATA_SIZE; ++i)
		offset.v[i] = (s16) val[i];
}

struct file_operations dmt_g_sensor_fops = {
	.owner = THIS_MODULE,
	//.read = device_read,
	//.write = device_write,
	.unlocked_ioctl = device_ioctl,
	.open = device_open,
	.release = device_close,
};

static struct miscdevice dmt_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = SENSOR_NAME,
	.fops = &dmt_g_sensor_fops,
};

static int sensor_close_dev(struct i2c_client *client){    	
	char buffer[3];
	buffer[0] = REG_ACTR;
	buffer[1] = MODE_Standby;
	buffer[2] = MODE_Off;	
	return device_i2c_txdata(client,buffer, 3);
}

static int device_i2c_suspend(struct i2c_client *client, pm_message_t mesg){
	dmtprintk("Gsensor enter 2 level suspend!!\n");
	return sensor_close_dev(client);
}

static int device_i2c_resume(struct i2c_client *client){
	dmtprintk("Gsensor 2 level resume!!\n");
	return gsensor_reset(client);
}

static int __devexit device_i2c_remove(struct i2c_client *client){
	return 0;
}

static const struct i2c_device_id device_i2c_ids[] = {
	{SENSOR_NAME, 0},
	{}   
};

static struct i2c_driver device_i2c_driver = 
{
	.driver	= {
		.owner = THIS_MODULE,
		.name = SENSOR_NAME,
		},
	.class = I2C_CLASS_HWMON,
	.id_table = device_i2c_ids,
	.probe = device_i2c_probe,
	.remove	= __devexit_p(device_i2c_remove),
	.suspend = device_i2c_suspend,
	.resume	= device_i2c_resume,
	.address_list	= u_i2c_addr.normal_i2c,
};

static int device_open(struct inode *inode, struct file *filp)
{
	return 0; 
}
/*
static ssize_t device_write(struct file *filp, const char *buf, size_t count, loff_t *f_pos){
	return 0;
}

static ssize_t device_read(struct file *filp, char *buf, size_t count, loff_t *f_pos){
	return count;
}
*/
static long device_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	//void __user *argp = (void __user *)arg;
	struct dmt_data *dmt = filp->private_data;
	
	int err = 0, ret = 0, i;
	int intBuf[SENSOR_DATA_SIZE];
	s16 xyz[SENSOR_DATA_SIZE];
	/* check type */
	if (_IOC_TYPE(cmd) != IOCTL_MAGIC) return -ENOTTY;

	/* check user space pointer is valid */
	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err =  !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	if (err) return -EFAULT;
	
	switch(cmd) 
	{
		case SENSOR_RESET:
			gsensor_reset(dmt->client);
			return ret;

		case SENSOR_CALIBRATION:
			/* get orientation info */
			//if(copy_from_user(&intBuf, (int*)arg, sizeof(intBuf))) return -EFAULT;
			gsensor_calibrate();
			dmtprintk("Sensor_calibration:%d %d %d\n",offset.u.x,offset.u.y,offset.u.z);
			/* save file */
			gsensor_write_offset_to_file();
			
			/* return the offset */
			for(i = 0; i < SENSOR_DATA_SIZE; ++i)
				intBuf[i] = offset.v[i];

			ret = copy_to_user((int *)arg, &intBuf, sizeof(intBuf));
			return ret;
		
		case SENSOR_GET_OFFSET:
			/* get data from file */ 
			gsensor_read_offset_from_file();
			for(i = 0; i < SENSOR_DATA_SIZE; ++i)
				intBuf[i] = offset.v[i];

			ret = copy_to_user((int *)arg, &intBuf, sizeof(intBuf));
			return ret;

		case SENSOR_SET_OFFSET:
			ret = copy_from_user(&intBuf, (int *)arg, sizeof(intBuf));
			gsensor_set_offset(intBuf);
			/* write in to file */
			gsensor_write_offset_to_file();
			return ret;
		
		case SENSOR_READ_ACCEL_XYZ:
			device_i2c_read_xyz(dmt->client, (s16 *)&xyz);
			for(i = 0; i < SENSOR_DATA_SIZE; ++i)
				intBuf[i] = xyz[i] - offset.v[i];
			
		  	ret = copy_to_user((int*)arg, &intBuf, sizeof(intBuf));
			//PRINT_X_Y_Z(intBuf[0], intBuf[1], intBuf[2]);
			return ret;
		case SENSOR_SETYPR:
			if(copy_from_user(&intBuf, (int*)arg, sizeof(intBuf))) 
			{
				dmtprintk("%s:copy_from_user(&intBuf, (int*)arg, sizeof(intBuf)) ERROR, -EFAULT\n",__func__);			
				return -EFAULT;
			}
			input_report_abs(s_dmt->input, ABS_X, intBuf[0]);
			input_report_abs(s_dmt->input, ABS_Y, -intBuf[1]);
			input_report_abs(s_dmt->input, ABS_Z, -intBuf[2]);
			input_sync(s_dmt->input);
			dmtprintk(KERN_INFO "%s:SENSOR_SETYPR OK! x=%d,y=%d,z=%d\n",__func__,intBuf[0],intBuf[1],intBuf[2]);
			return 1;
		case SENSOR_GET_OPEN_STATUS:
			dmtprintk(KERN_INFO "%s:Going into DMT_GetOpenStatus()\n",__func__);
			DMT_GetOpenStatus();
			dmtprintk(KERN_INFO "%s:DMT_GetOpenStatus() finished\n",__func__);
			return 1;
			break;
		case SENSOR_GET_CLOSE_STATUS:
			dmtprintk(KERN_INFO "%s:Going into DMT_GetCloseStatus()\n",__func__);
			DMT_GetCloseStatus();	
			dmtprintk(KERN_INFO "%s:DMT_GetCloseStatus() finished\n",__func__);
			return 1;
			break;		
		case SENSOR_GET_DELAY:
		  	ret = copy_to_user((int*)arg, &interval, sizeof(interval));
			return 1;
			break;
		
		default:  /* redundant, as cmd was checked against MAXNR */
			return -ENOTTY;
	}
	
	return 0;
}
	
static int device_close(struct inode *inode, struct file *filp)
{
	return 0;
}

/***** I2C I/O function ***********************************************/
static int device_i2c_rxdata( struct i2c_client *client, unsigned char *rxData, int length)
{
	struct i2c_msg msgs[] = 
	{
		{.addr = client->addr, .flags = 0, .len = 1, .buf = rxData,}, 
		{.addr = client->addr, .flags = I2C_M_RD, .len = length, .buf = rxData,},
	};
	//unsigned char addr = rxData[0];
	if (i2c_transfer(client->adapter, msgs, 2) < 0) {
		dev_err(&client->dev, "%s: transfer failed.", __func__);
		return -EIO;
	}
	//DMT_DATA(&client->dev, "RxData: len=%02x, addr=%02x, data=%02x\n",
		//length, addr, rxData[0]);

	return 0;
}

static int device_i2c_txdata( struct i2c_client *client, unsigned char *txData, int length)
{
	struct i2c_msg msg[] = 
	{
		{.addr = client->addr, .flags = 0, .len = length, .buf = txData,}, 
	};

	if (i2c_transfer(client->adapter, msg, 1) < 0) {
		dev_err(&client->dev, "%s: transfer failed.", __func__);
		return -EIO;
	}
	//DMT_DATA(&client->dev, "TxData: len=%02x, addr=%02x data=%02x\n",
		//length, txData[0], txData[1]);
	return 0;
}
/* 1g = 128 becomes 1g = 1024 */
static inline void device_i2c_correct_accel_sign(s16 *val){
	*val<<= 3;
}

void device_i2c_merge_register_values(struct i2c_client *client, s16 *val, u8 msb, u8 lsb){
	*val = (((u16)msb) << 8) | (u16)lsb; 
	device_i2c_correct_accel_sign(val);
}

void device_i2c_read_xyz(struct i2c_client *client, s16 *xyz_p){	
	u8 buffer[11];
	s16 xyzTmp[SENSOR_DATA_SIZE];
	int i, j;
	/* get xyz high/low bytes, 0x12 */
	buffer[0] = REG_STADR;
	device_i2c_rxdata(client, buffer, 10);
    
	/* merge to 10-bits value */
	for(i = 0; i < SENSOR_DATA_SIZE; ++i){
		xyz_p[i] = 0;
		device_i2c_merge_register_values(client, (xyzTmp + i), buffer[2*(i+1)+1], buffer[2*(i+1)]);
	/* transfer to the default layout */
		for(j = 0; j < 3; j++)
			xyz_p[i] += sensorlayout[i][j] * xyzTmp[j];
	}
	dmtprintk(KERN_INFO "@DMT@ xyz_p: %04d , %04d , %04d\n", xyz_p[0], xyz_p[1], xyz_p[2]);
}

static void DMT_work_func(struct work_struct *delaywork)
{
	struct dmt_data *dmt = container_of(delaywork, struct dmt_data, delaywork.work);
	int i;
	static int firsttime=0;
	s16 xyz[SENSOR_DATA_SIZE];
	
	unsigned long t=atomic_read(&s_dmt->delay);
  	unsigned long dmt_delay = msecs_to_jiffies(t);
	if(!firsttime){
		gsensor_read_offset_from_file();	
	 	firsttime=1;
	}
	
	dmtprintk(KERN_INFO "t=%lu , dmt_delay=%lu\n", t, dmt_delay);
  	device_i2c_read_xyz(s_dmt->client, (s16 *)&xyz);
  	for(i = 0; i < SENSOR_DATA_SIZE; ++i)
     		xyz[i] -= offset.v[i];

	PRINT_X_Y_Z(xyz[0], xyz[1], xyz[2]);
	PRINT_X_Y_Z(offset.u.x, offset.u.y, offset.u.z);
	input_report_abs(s_dmt->input, ABS_X, xyz[0]);
	input_report_abs(s_dmt->input, ABS_Y, -xyz[1]);
	input_report_abs(s_dmt->input, ABS_Z, -xyz[2]);
	input_sync(s_dmt->input);
		
	if(dmt_delay < 1)
		dmt_delay = 1;
	schedule_delayed_work(&dmt->delaywork, dmt_delay);
}

static int __devinit device_i2c_probe(struct i2c_client *client,const struct i2c_device_id *id){
	int i;
	int ret = 0;
	//struct dmt_data *s_dmt;
	//struct device *device;
	
	for(i = 0; i < SENSOR_DATA_SIZE; ++i)
		offset.v[i] = 0;
		
	if(!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)){
		dmtprintk(KERN_INFO "%s, check_functionality failed.\n", __func__);
		ret = -ENODEV;
		goto exit0;
  	}
  	
  	/* Allocate memory for driver data */
	s_dmt = kzalloc(sizeof(struct dmt_data), GFP_KERNEL);
	if (s_dmt == NULL) {
		dmtprintk(KERN_INFO "%s, alloc data failed.\n", __func__);
		ret = -ENOMEM;
		goto exit1;
	}
	
	/***** I2C initialization *****/
	s_dmt->client = client;
	/* set client data */
	i2c_set_clientdata(client, s_dmt);
	ret = gsensor_reset(client);
	if (ret < 0)
		goto exit2;
		
	/***** input *****/
	ret = input_init();
	if (ret) {
		dmtprintk(KERN_INFO "%s:input_init fail, error code= %d\n",__func__,ret);
		goto exit3;
	}
	
	/**** initialize variables in dmt_data *****/
	init_waitqueue_head(&s_dmt->open_wq);
	atomic_set(&s_dmt->active, 0);
	atomic_set(&s_dmt->enable, 0);
	atomic_set(&s_dmt->delay, 0);
	mutex_init(&s_dmt->sensor_mutex);
	/***** misc *****/
	ret = misc_register(&dmt_device);
	if (ret) 
	{
		dmtprintk(KERN_INFO "%s: dmt_dev register failed", __func__);
		goto exit5;
	}

	/***** sysfs *****/
    ret = create_sysfs_interfaces(s_dmt);
    if (ret < 0) 
	{
        dmtprintk(KERN_INFO  "%s: create sysfs failed.", __func__);
        goto exit6;
    }
	
	if(gsensor_fetch_sysconfig_para()){
		printk("%s: gsensor_fetch_sysconfig_para err.\n", __func__);
		return -ENODEV;
	}
	
	printk("%s: after fetch_sysconfig_para:  normal_i2c: 0x%hx. normal_i2c[1]: 0x%hx \n", \
	__func__, u_i2c_addr.normal_i2c[0], u_i2c_addr.normal_i2c[1]);
	
	INIT_DELAYED_WORK(&s_dmt->delaywork, DMT_work_func);
	dmtprintk(KERN_INFO "DMT: INIT_DELAYED_WORK\n");
	
	device_i2c_driver.detect = gsensor_detect;
	
	return 0;

exit6:
	misc_deregister(&dmt_device);
exit5:
	input_unregister_device(s_dmt->input);
exit3:
exit2:
	kfree(s_dmt);
exit1:	
exit0:
	return ret;
}

static int __init device_init(void){	
	return i2c_add_driver(&device_i2c_driver);
}

static void __exit device_exit(void){
	i2c_del_driver(&device_i2c_driver);
}

void gsensor_write_offset_to_file(void){
	char data[18];
	unsigned int orgfs;
	struct file *fp;

	sprintf(data,"%5d %5d %5d",offset.u.x,offset.u.y,offset.u.z);
	orgfs = get_fs();
	/* Set segment descriptor associated to kernel space */
	set_fs(KERNEL_DS);
	fp = filp_open(OffsetFileName, O_RDWR | O_CREAT, 0777);
	if(IS_ERR(fp)){
		dmtprintk(KERN_INFO "filp_open %s error!!.\n",OffsetFileName);
	}
	else{
		dmtprintk(KERN_INFO "filp_open %s SUCCESS!!.\n",OffsetFileName);
		fp->f_op->write(fp,data,18, &fp->f_pos);
 		filp_close(fp,NULL);
	}
	set_fs(orgfs);
}

void gsensor_read_offset_from_file(void){
	unsigned int orgfs;
	char data[18];
	struct file *fp;
	int ux,uy,uz;
	orgfs = get_fs();
	/* Set segment descriptor associated to kernel space */
	set_fs(KERNEL_DS);

	fp = filp_open(OffsetFileName, O_RDWR , 0);
	dmtprintk(KERN_INFO "%s,try file open !\n",__func__);
	if(IS_ERR(fp)){
		dmtprintk(KERN_INFO "%s:Sorry,file open ERROR !\n",__func__);
		offset.u.x=0;offset.u.y=0;offset.u.z=0;
#if AUTO_CALIBRATION
		/* get acceleration average reading */
		gsensor_calibrate();
		gsensor_write_offset_to_file();
#endif
	}
	else{
		dmtprintk("filp_open %s SUCCESS!!.\n",OffsetFileName);
		fp->f_op->read(fp,data,18, &fp->f_pos);
		dmtprintk("filp_read result %s\n",data);
		sscanf(data,"%d %d %d",&ux,&uy,&uz);
		offset.u.x=ux;
		offset.u.y=uy;
		offset.u.z=uz;
		filp_close(fp,NULL);
	}
	set_fs(orgfs);
}
//*********************************************************************************************************
MODULE_AUTHOR("DMT_RD");
MODULE_DESCRIPTION("DMT Gsensor Driver");
MODULE_LICENSE("GPL");

module_init(device_init);
module_exit(device_exit);
