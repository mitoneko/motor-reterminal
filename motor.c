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
// デバイス全域で使用する変数達
// motor_probeでメモリ確保する。
struct motor_device_info {
    unsigned int major; // udev major番号
    struct cdev cdev;
    struct class *class;
    struct gpio_desc *gpio;
    struct hrtimer pwm_timer;
    unsigned int priod; // pwmベース周期　ns
    unsigned int on_time; // 周期あたりのLED ON時間 ns
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
    result = put_user(gpiod_get_value(bdev->gpio)+'0', &buf[0]);
    if (result != 0) return result;
    return 1;
}

static ssize_t motor_write(struct file *fp, const char __user *buf, size_t count, loff_t *f_pos) {
    struct motor_device_info *bdev = fp->private_data;
    char outValue;
    int result;
    int pwm_width;

    if (bdev==NULL) return -EBADF;
    if (count == 0) return 0;
    result = get_user(outValue, &buf[0]);
    if (result != 0) return result;
    
    hrtimer_cancel(&bdev->pwm_timer);
    switch (outValue) {
        case '0':
            gpiod_set_value(bdev->gpio, 0);
            pr_devel("%s: writed [%c] \n", __func__, outValue);
            break;
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
            pwm_width = outValue - '0';
            bdev->on_time = bdev->priod / 9 * pwm_width;
            gpiod_set_value(bdev->gpio, 1);
            hrtimer_start(&bdev->pwm_timer, 
                    ktime_set(0, bdev->on_time), HRTIMER_MODE_REL);
            pr_devel("%s: writed [%c] \n", __func__, outValue);
            break;
        case '9':
            gpiod_set_value(bdev->gpio, 1);
            pr_devel("%s: writed [%c] \n", __func__, outValue);
            break;
        default:
            pr_info("%s: no writed. arg=\"%c\"\n", __func__ , outValue);
    }

    return count;
}

// pwm_timer(hrtimer)用の割り込み処理関数
enum hrtimer_restart pwm_timer_handler(struct hrtimer *timer) {
    struct motor_device_info *bdev = from_timer(bdev, timer, pwm_timer);
    int next_priod;

    if (!bdev) {
        pr_err("%s:デバイス情報取得失敗\n", __func__);
        return HRTIMER_NORESTART;
    }
    int cur_led = gpiod_get_value(bdev->gpio);
    if (cur_led == 1) {
        next_priod = bdev->priod - bdev->on_time;
        gpiod_set_value(bdev->gpio, 0);
    } else {
        next_priod = bdev->on_time;
        gpiod_set_value(bdev->gpio, 1);
    }
    hrtimer_forward_now(&bdev->pwm_timer, ktime_set(0, next_priod));
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

    // hrtimerの初期化
    bdev->priod = 1000000;
    bdev->on_time = 0;
    hrtimer_init(&bdev->pwm_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL); 
    bdev->pwm_timer.function = pwm_timer_handler;
    
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
    bdev->gpio = devm_gpiod_get(dev, NULL, GPIOD_OUT_LOW);
    if (IS_ERR(bdev->gpio)) {
        result = -PTR_ERR(bdev->gpio);
        pr_alert("%s: can not get GPIO.ERR(%d)\n", __func__, result);
        goto err;
    }

    // udevの生成
    result = make_udev(bdev, p_dev->name);
    if (result != 0) {
        pr_alert("%s:Fail make udev. gpio desc dispose!!!\n", __func__);
        goto err_udev;
    }

    pr_info("%s:motor driver init\n",__func__);
    return 0;

err_udev:
    gpiod_put(bdev->gpio);
err:
    return result;
}

static int motor_remove(struct platform_device *p_dev) {
    struct motor_device_info *bdev = dev_get_drvdata(&p_dev->dev);
    remove_udev(bdev);

    // gpioデバイスの開放
    if (bdev->gpio) {
        gpiod_put(bdev->gpio);
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
        .of_match_table = of_beep_ids,
    },
};

module_platform_driver(motor_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("This is external led drive for reterminal");
MODULE_AUTHOR("mito");

