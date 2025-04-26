/*
 * mcp9808.c — Device-tree driven MCP9808 temperature sensor driver
 *
 * Fetches the I²C address from the device tree 'reg' property
 * and sets resolution to 0.125°C, exposing temperature reads
 * via a character device.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>

#define MCP9808_TEMP_REG     0x05    /* Temperature register */
#define MCP9808_RES_REG      0x08    /* Resolution register */
#define MCP9808_RESOLUTION   0x02    /* Set resolution to 0.125°C */
#define DEVICE_NAME          "mcp9808"

static dev_t mcp9808_dev;
static struct class *mcp9808_class;

struct mcp9808_data {
    struct i2c_client *client;
    struct cdev        cdev;
};

/* Set resolution of the MCP9808 to 0.125°C */
static int set_resolution(struct i2c_client *client)
{
    int ret = i2c_smbus_write_byte_data(client, MCP9808_RES_REG, MCP9808_RESOLUTION);
    if (ret < 0)
        dev_err(&client->dev, "Failed to set resolution\n");
    else
        dev_info(&client->dev, "Resolution set to 0.125°C\n");
    return ret;
}

/* Read temperature and return micro-degrees Celsius */
static int read_temperature(struct i2c_client *client)
{
    uint8_t reg = MCP9808_TEMP_REG, buf[2];
    int ret, temp_uc;
    uint8_t hi, lo;

    ret = i2c_master_send(client, &reg, 1);
    if (ret < 0) {
        dev_err(&client->dev, "Write temp reg failed\n");
        return ret;
    }

    ret = i2c_master_recv(client, buf, 2);
    if (ret < 0) {
        dev_err(&client->dev, "Read temp failed\n");
        return ret;
    }

    hi = buf[0]; lo = buf[1];
    if (hi & 0x80) dev_info(&client->dev, "TA >= TCRIT\n");
    if (hi & 0x40) dev_info(&client->dev, "TA > TUPPER\n");
    if (hi & 0x20) dev_info(&client->dev, "TA < TLOWER\n");

    hi &= 0x1F;  /* clear flag bits */

    /* convert to micro-°C (×10⁴ then ÷16) */
    if (hi & 0x10) {
        /* negative */
        hi &= 0x0F;
        temp_uc = -((hi << 4) * 10000 + (lo * 10000) / 16);
    } else {
        temp_uc = (hi << 4) * 10000 + (lo * 10000) / 16;
    }

    dev_info(&client->dev,
             "Raw temp: %02X %02X → %d.%04d°C\n",
             hi, lo, temp_uc/10000, abs(temp_uc)%10000);
    return temp_uc;
}

/* char dev read() */
static ssize_t mcp9808_read(struct file *file, char __user *buf,
                            size_t count, loff_t *offset)
{
    struct mcp9808_data *d = file->private_data;
    int temp_uc = read_temperature(d->client);
    char tmp[32];
    int len;

    if (temp_uc < 0)
        return temp_uc;

    len = snprintf(tmp, sizeof(tmp), "%d.%04d\n",
                   temp_uc/10000, abs(temp_uc)%10000);

    if (*offset >= len)
        return 0;
    if (copy_to_user(buf, tmp, len))
        return -EFAULT;

    *offset += len;
    return len;
}

/* open() */
static int mcp9808_open(struct inode *inode, struct file *file)
{
    struct mcp9808_data *d =
        container_of(inode->i_cdev, struct mcp9808_data, cdev);
    file->private_data = d;
    dev_info(&d->client->dev, "Device opened\n");
    return 0;
}

static const struct file_operations mcp9808_fops = {
    .owner   = THIS_MODULE,
    .open    = mcp9808_open,
    .read    = mcp9808_read,
};

/* Probe: read DT reg, init device, create char device */
static int mcp9808_probe(struct i2c_client *client)
{
    struct mcp9808_data *d;
    struct device_node *np = client->dev.of_node;
    u32 addr;
    int ret;

    if (!np) {
        dev_err(&client->dev, "No device tree node\n");
        return -EINVAL;
    }

    ret = of_property_read_u32(np, "reg", &addr);
    if (ret) {
        dev_err(&client->dev, "Missing 'reg' DT property\n");
        return ret;
    }
    if (addr != client->addr)
        dev_warn(&client->dev,
                 "DT reg=0x%02x != client->addr=0x%02x\n",
                 addr, client->addr);

    d = devm_kzalloc(&client->dev, sizeof(*d), GFP_KERNEL);
    if (!d)
        return -ENOMEM;

    d->client = client;
    i2c_set_clientdata(client, d);

    dev_info(&client->dev, "Probing MCP9808@0x%02x\n", client->addr);

    ret = set_resolution(client);
    if (ret)
        return ret;

    ret = alloc_chrdev_region(&mcp9808_dev, 0, 1, DEVICE_NAME);
    if (ret) {
        dev_err(&client->dev, "alloc_chrdev_region failed\n");
        return ret;
    }

    cdev_init(&d->cdev, &mcp9808_fops);
    d->cdev.owner = THIS_MODULE;
    ret = cdev_add(&d->cdev, mcp9808_dev, 1);
    if (ret) {
        dev_err(&client->dev, "cdev_add failed\n");
        unregister_chrdev_region(mcp9808_dev, 1);
        return ret;
    }

    mcp9808_class = class_create(DEVICE_NAME);
    if (IS_ERR(mcp9808_class)) {
        ret = PTR_ERR(mcp9808_class);
        dev_err(&client->dev, "class_create failed\n");
        cdev_del(&d->cdev);
        unregister_chrdev_region(mcp9808_dev, 1);
        return ret;
    }

    device_create(mcp9808_class, NULL, mcp9808_dev, NULL, DEVICE_NAME);
    dev_info(&client->dev, "%s initialized\n", DEVICE_NAME);
    return 0;
}

/* Remove: undo probe */
static void mcp9808_remove(struct i2c_client *client)
{
    struct mcp9808_data *d = i2c_get_clientdata(client);

    device_destroy(mcp9808_class, mcp9808_dev);
    class_destroy(mcp9808_class);
    cdev_del(&d->cdev);
    unregister_chrdev_region(mcp9808_dev, 1);

    dev_info(&client->dev, "%s removed\n", DEVICE_NAME);
}

/* DT match table */
static const struct of_device_id mcp9808_of_match[] = {
    { .compatible = "microchip,mcp9808" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mcp9808_of_match);

/* I2C driver */
static const struct i2c_device_id mcp9808_id[] = {
    { "mcp9808", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, mcp9808_id);

static struct i2c_driver mcp9808_driver = {
    .driver = {
        .name           = "mcp9808",
        .of_match_table = of_match_ptr(mcp9808_of_match),
    },
    .probe      = mcp9808_probe,
    .remove     = mcp9808_remove,
    .id_table   = mcp9808_id,
};

module_i2c_driver(mcp9808_driver);

MODULE_AUTHOR("XY");
MODULE_DESCRIPTION("MCP9808 temperature sensor driver");
MODULE_LICENSE("GPL");
