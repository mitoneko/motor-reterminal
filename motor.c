//#define DEBUG 1
#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <asm/current.h>
#include <asm/uaccess.h>

#define DRIVER_NAME "motor"

static const unsigned int MINOR_BASE = 0; // udev minor番号の始まり
static const unsigned int MINOR_NUM = 1;  // udev minorの個数
static const int WIDTH_MAX_INIT = 9;  // pwm widthの最大値
static const unsigned int PERIOD_INIT = 1000000; // PWM周期初期値

// デバイス全域で使用する変数達
// motor_probeでメモリ確保する。
struct motor_device_info {
    unsigned int major; // udev major番号
    struct cdev cdev;
    struct class *class;
    struct gpio_desc *forward;
    struct gpio_desc *backward;
    struct gpio_desc *cur_out;
    struct hrtimer pwm_timer;
    unsigned int period; // pwmベース周期　ns
    unsigned int width_max; // PWMのwidthの絶対値の最大
    unsigned int on_time; // 周期あたりのLED ON時間 ns
    int cur_pwm_width;
};

// /dev/ectled配下のアクセス関数

// デバイス情報を取得し、file構造体に保存する。
static int motor_open(struct inode *inode, struct file *file) {
    pr_devel("%s:motor open\n", __func__);
    struct motor_device_info *bdev = container_of(inode->i_cdev, struct motor_device_info, cdev);
    if (bdev==NULL) {
        pr_err("%s:デバイス情報取得失敗\n", __func__);
        return -EFAULT;
    }
    file->private_data = bdev;
    return 0;
}

static int motor_close(struct inode *inode, struct file *file) {
    //実質何もしない
    pr_devel("%s:motor closed\n", __func__);
    return 0;
}

static ssize_t motor_read(struct file *fp, char __user *buf, size_t count, loff_t *f_pos) {
    struct motor_device_info *bdev = fp->private_data;
    int result;

    if (count == 0) return 0;
    if (buf == NULL ) return -EFAULT;
    if (!bdev) {
        pr_err("%s:デバイス情報取得失敗\n", __func__);
        return -EBADF;
    }
    if (bdev->cur_out == NULL) {
        result = put_user('0', &buf[0]);
    } else {
        result = put_user(gpiod_get_value(bdev->cur_out)+'0', &buf[0]);
    }
    if (result != 0) return result;
    return 1;
}

static ssize_t motor_write(struct file *fp, const char __user *buf, size_t count, loff_t *f_pos) {
    struct motor_device_info *bdev = fp->private_data;
    char outValue_s[10];
    int outValue;
    int result;
    unsigned int pwm_width;

    if (bdev==NULL) return -EBADF;
    if (count == 0) return 0;
    if (copy_from_user(outValue_s, buf, min(count, (size_t)9)) != 0) {
        return -EFAULT;
    }
    outValue_s[min(count, (size_t)9)] = '\0';
    pr_devel("%s: recieve string [%s] \n", __func__, outValue_s);
    if (outValue_s[0] == 'b' || outValue_s[0] == 'B') {
        hrtimer_cancel(&bdev->pwm_timer);
        bdev->cur_out = NULL;
        bdev->cur_pwm_width = 0;
        gpiod_set_value(bdev->forward, 1);
        gpiod_set_value(bdev->backward, 1);
    } else {
        result = kstrtoint(outValue_s, 10, &outValue);
        if (result) {
            pr_devel("%s: other string [%s] \n", __func__, outValue_s);
            return count;  // 数値以外のものが来たら無視
        }

        hrtimer_cancel(&bdev->pwm_timer);
        if (outValue == 0) {
            bdev->cur_out = NULL;
            gpiod_set_value(bdev->forward, 0);
            gpiod_set_value(bdev->backward, 0);
            bdev->cur_pwm_width = 0;
        } else {
            if (bdev->cur_out) { // 前回の出力をリセットする。
                gpiod_set_value(bdev->cur_out,0);
            } else {
                gpiod_set_value(bdev->forward, 0);
                gpiod_set_value(bdev->backward, 0);
            }
            bdev->cur_out = (outValue>0) ? bdev->forward : bdev->backward;
            pwm_width = (outValue>=0)? outValue : -outValue;
            pwm_width = min(pwm_width, bdev->width_max);
            bdev->cur_pwm_width = pwm_width;
            if (pwm_width == bdev->width_max) {
                gpiod_set_value(bdev->cur_out, 1);
            } else {
                bdev->on_time = bdev->period / bdev->width_max * pwm_width;
                gpiod_set_value(bdev->cur_out, 1);
                hrtimer_start(&bdev->pwm_timer, 
                        ktime_set(0, bdev->on_time), HRTIMER_MODE_REL);
            }
        }
        pr_devel("%s: writed [%d] \n", __func__, outValue);
    }

    return count;
}

// pwm_timer(hrtimer)用の割り込み処理関数
enum hrtimer_restart pwm_timer_handler(struct hrtimer *timer) {
    struct motor_device_info *bdev = from_timer(bdev, timer, pwm_timer);
    int next_period;

    if (!bdev) {
        pr_err("%s:デバイス情報取得失敗\n", __func__);
        return HRTIMER_NORESTART;
    }
    int cur_led = gpiod_get_value(bdev->cur_out);
    if (cur_led == 1) {
        next_period = bdev->period - bdev->on_time;
        gpiod_set_value(bdev->cur_out, 0);
    } else {
        next_period = bdev->on_time;
        gpiod_set_value(bdev->cur_out, 1);
    }
    hrtimer_forward_now(&bdev->pwm_timer, ktime_set(0, next_period));
    return HRTIMER_RESTART;
}

/* ハンドラ　テーブル */
struct file_operations motor_fops = {
    .open     = motor_open,
    .release  = motor_close,
    .read     = motor_read,
    .write    = motor_write,
};

// キャラクタデバイスの登録と、/dev/motor0の生成
static int make_udev(struct motor_device_info *bdev, const char* name) { 
    int ret = 0;
    dev_t dev;

    /* メジャー番号取得 */
    ret = alloc_chrdev_region(&dev, MINOR_BASE, MINOR_NUM, name);
    if (ret != 0) {
        pr_alert("%s: メジャー番号取得失敗(%d)\n", __func__, ret);
        goto err;
    }
    bdev->major = MAJOR(dev);

    /* カーネルへのキャラクタデバイスドライバ登録 */
    cdev_init(&bdev->cdev, &motor_fops);
    bdev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&bdev->cdev, dev, MINOR_NUM);
    if (ret != 0) {
        pr_alert("%s: キャラクタデバイス登録失敗(%d)\n", __func__, ret);
        goto err_cdev_add;
    }

    /* カーネルクラス登録 */
    bdev->class = class_create(THIS_MODULE, name);
    if (IS_ERR(bdev->class)) {
        pr_alert("%s: カーネルクラス登録失敗\n", __func__);
        ret =  -PTR_ERR(bdev->class);
        goto err_class_create;
    }

    /* /sys/class/mygpio の生成 */
    for (int minor = MINOR_BASE; minor < MINOR_BASE + MINOR_NUM; minor++) {
        device_create(bdev->class, NULL, MKDEV(bdev->major, minor), NULL, "motor%d", minor);
    }

    
    return 0;

err_class_create:
    cdev_del(&bdev->cdev);
err_cdev_add:
    unregister_chrdev_region(dev, MINOR_NUM);
err:
    return ret;
}

// キャラクタデバイス及び/dev/motor0の登録解除
static void remove_udev(struct motor_device_info *bdev) {
    dev_t dev = MKDEV(bdev->major, MINOR_BASE);
    for (int minor=MINOR_BASE; minor<MINOR_BASE+MINOR_NUM; minor++) {
        /* /sys/class/motor の削除 */
        device_destroy(bdev->class, MKDEV(bdev->major, minor));
    }
    class_destroy(bdev->class); /* クラス登録解除 */
    cdev_del(&bdev->cdev); /* デバイス除去 */
    unregister_chrdev_region(dev, MINOR_NUM); /* メジャー番号除去 */
}

// sysfs 関係
// 　period, max_pwm_widthの設定・読み出し
ssize_t period_show(struct device *dev, struct device_attribute *attr, char *buf) {
    struct motor_device_info *bdev = dev_get_drvdata(dev);
    if (!bdev) {
        pr_alert("%s: Driver data can't get.\n", __func__);
        return -ENODEV;
    }
    return snprintf(buf, PAGE_SIZE, "%u\n", bdev->period);
}

ssize_t period_store(struct device *dev, struct device_attribute *attr, 
        const char *buf, size_t count) {
    struct motor_device_info *bdev = dev_get_drvdata(dev);
    int result;
    unsigned int set_value;

    if (!bdev) {
        pr_alert("%s: Driver data can't get.\n", __func__);
        return -ENODEV;
    }
    
    result = kstrtouint(buf, 10, &set_value);
    if (result) {
        pr_err("%s: 数値以外のものはperiodにできない。[%s])\n", __func__,buf);
        return -EINVAL;
    }
    
    bdev->period = set_value;
    return (ssize_t)count;
}

ssize_t max_pwm_show(struct device *dev, struct device_attribute *attr, char *buf) {
    struct motor_device_info *bdev = dev_get_drvdata(dev);
    if (!bdev) {
        pr_alert("%s: Driver data can't get.\n", __func__);
        return -ENODEV;
    }
    return snprintf(buf, PAGE_SIZE, "%u\n", bdev->width_max);
}

ssize_t max_pwm_store(struct device *dev, struct device_attribute *attr, 
        const char *buf, size_t count) {
    struct motor_device_info *bdev = dev_get_drvdata(dev);
    int result;
    unsigned int set_value;

    if (!bdev) {
        pr_alert("%s: Driver data can't get.\n", __func__);
        return -ENODEV;
    }
    
    result = kstrtouint(buf, 10, &set_value);
    if (result) {
        pr_err("%s: 数値以外のものはperiodにできない。[%s])\n", __func__,buf);
        return -EINVAL;
    }
    
    bdev->width_max = set_value;
    return (ssize_t)count;
}


DEVICE_ATTR(period, S_IRUGO | S_IWUSR | S_IWGRP, period_show, period_store);
DEVICE_ATTR(max_pwm, S_IRUGO | S_IWUSR | S_IWGRP, max_pwm_show, max_pwm_store);

static struct attribute *motor_attrs[] = {
    &dev_attr_period.attr,
    &dev_attr_max_pwm.attr,
    NULL,
};

static struct attribute_group motor_group = {
    .attrs = motor_attrs,
};

// ドライバの初期化　及び　後始末
static const struct of_device_id of_motor_ids[] = {
    { .compatible = "reterminal_motor" } ,
    { },
};

MODULE_DEVICE_TABLE(of, of_motor_ids);

static int motor_probe(struct platform_device *p_dev) {
    struct device *dev = &p_dev->dev;
    struct motor_device_info *bdev;
    int result;

    if (!dev->of_node) {
        pr_alert("%s:Not Exist of_node for BEEP DRIVER. Check DTB\n", __func__);
        result = -ENODEV;
        goto err;
    }

    // デバイス情報のメモリ確保と初期化
    bdev = (struct motor_device_info*)devm_kzalloc(dev, sizeof(struct motor_device_info), GFP_KERNEL);
    if (!bdev) {
        pr_alert("%s: デバイス情報メモリ確保失敗\n", __func__);
        result = -ENOMEM;
        goto err;
    }
    dev_set_drvdata(dev, bdev);

    // gpioの確保と初期化
    bdev->forward = devm_gpiod_get_index(dev, NULL, 0, GPIOD_OUT_LOW);
    if (IS_ERR(bdev->forward)) {
        result = -PTR_ERR(bdev->forward);
        pr_alert("%s: can not get GPIO.ERR(%d)(forward)\n", __func__, result);
        goto err;
    }
    bdev->backward = devm_gpiod_get_index(dev, NULL, 1, GPIOD_OUT_LOW);
    if (IS_ERR(bdev->backward)) {
        result = -PTR_ERR(bdev->backward);
        pr_alert("%s: can not get GPIO.ERR(%d)(backward)\n", __func__, result);
        goto err_backward;
    }
    bdev->cur_out = NULL;

    // udevの生成
    result = make_udev(bdev, p_dev->name);
    if (result != 0) {
        pr_alert("%s:Fail make udev. gpio desc dispose!!!\n", __func__);
        goto err_udev;
    }

    // hrtimerの初期化
    bdev->period = PERIOD_INIT;
    bdev->width_max = WIDTH_MAX_INIT;
    bdev->on_time = 0;
    hrtimer_init(&bdev->pwm_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL); 
    bdev->pwm_timer.function = pwm_timer_handler;
    
    // sysfsの初期化
    result = sysfs_create_group(&(dev->kobj), &motor_group);
    if (result != 0) {
        pr_alert("%s: Can not create sysfs nodes.\n", __func__);
        goto err_udev;
    }

    pr_info("%s:motor driver init\n",__func__);
    return 0;

err_udev:
    gpiod_put(bdev->backward);
err_backward:
    gpiod_put(bdev->forward);
err:
    return result;
}

static int motor_remove(struct platform_device *p_dev) {
    struct motor_device_info *bdev = dev_get_drvdata(&p_dev->dev);
    remove_udev(bdev);

    // sysfsの開放
    sysfs_remove_group(&(p_dev->dev.kobj), &motor_group);

    // gpioデバイスの開放
    if (bdev->forward) {
        gpiod_put(bdev->forward);
    }
    if (bdev->backward) {
        gpiod_put(bdev->backward);
    }

    pr_info("%s:motor driver unloaded\n",__func__);
    return 0;
} 
            

static struct platform_driver motor_driver = { 
    .probe = motor_probe,
    .remove = motor_remove,
    .driver = {
        .name = DRIVER_NAME,
        .owner = THIS_MODULE,
        .of_match_table = of_motor_ids,
    },
};

module_platform_driver(motor_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("This is external led drive for reterminal");
MODULE_AUTHOR("mito");

