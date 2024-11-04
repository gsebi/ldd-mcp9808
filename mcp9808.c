#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>

#define MCP9808_I2C_ADDRESS  0x18    // I2C address for MCP9808
#define MCP9808_TEMP_REG     0x05    // Temperature register
#define MCP9808_RES_REG      0x08    // Resolution register
#define MCP9808_RESOLUTION   0x02    // Set resolution to 0.125°C
#define DEVICE_NAME "mcp9808"

static dev_t mcp9808_dev;
static struct class *mcp9808_class;

struct mcp9808_data {
    struct i2c_client *client;
    struct cdev cdev;
};

// Function to set resolution of the MCP9808 to 0.125°C
static int set_resolution(struct i2c_client *client)
{
    int ret;

    // Write 0x02 to the resolution register (0x08)
    ret = i2c_smbus_write_byte_data(client, MCP9808_RES_REG, MCP9808_RESOLUTION);
    if (ret < 0) {
        dev_err(&client->dev, "Failed to set resolution to 0.125°C\n");
        return ret;
    }

    dev_info(&client->dev, "Resolution set to 0.125°C\n");
    return 0;
}

// Function to read temperature from the MCP9808
static int read_temperature(struct i2c_client *client)
{
    uint8_t reg = MCP9808_TEMP_REG;
    uint8_t temp_data[2];
    int ret;
    int temp_micro_c;
    uint8_t upper_byte, lower_byte;

    // Write register address and read two bytes of temperature data
    dev_info(&client->dev, "Reading temperature data from MCP9808\n");
    ret = i2c_master_send(client, &reg, 1);
    if (ret < 0) {
        dev_err(&client->dev, "Failed to write to MCP9808 register\n");
        return ret;
    }

    ret = i2c_master_recv(client, temp_data, 2);
    if (ret < 0) {
        dev_err(&client->dev, "Failed to read temperature data from MCP9808\n");
        return ret;
    }

    // Extract upper and lower bytes
    upper_byte = temp_data[0];
    lower_byte = temp_data[1];


    // Check and clear flag bits in upper byte
    if (upper_byte & 0x80)
        dev_info(&client->dev, "TA >= TCRIT\n");
    if (upper_byte & 0x40)
        dev_info(&client->dev, "TA > TUPPER\n");
    if (upper_byte & 0x20)
        dev_info(&client->dev, "TA < TLOWER\n");

    upper_byte &= 0x1F;  // Clear flag bits

    dev_info(&client->dev, "Temperature raw is %02X %02X\n", upper_byte, lower_byte);
    
    // NOTE> After devision decimal fractions wil be lost should multiple by 10000 before devision -> stupid ChatGPT...
    // Convert temperature to milli-degrees Celsius
    if (upper_byte & 0x10) {
        // Temperature < 0°C
        upper_byte &= 0x0F;  // Clear the sign bit
        temp_micro_c = (256 * 100000 - (upper_byte * 16 * 10000 + (lower_byte * 10000) / 16)) * -1;
    } else {
        // Temperature >= 0°C
        temp_micro_c = (upper_byte * 16 * 10000 + (lower_byte * 10000) / 16);
    }

    dev_info(&client->dev, "Temperature stored as %d Celsius\n", temp_micro_c);
    dev_info(&client->dev, "Temperature read as %d.%03d Celsius\n", temp_micro_c / 10000, temp_micro_c % 10000);
    return temp_micro_c;
}

// Character device read function
static ssize_t mcp9808_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
    struct mcp9808_data *data = file->private_data;
    int temperature_micro_c;
    char temp_str[16];
    int len;

    dev_info(&data->client->dev, "User reading temperature from /dev/mcp9808\n");

    temperature_micro_c = read_temperature(data->client);
    if (temperature_micro_c < 0)
        return temperature_micro_c;

    // Format temperature with thousandths (three decimal places)
    len = snprintf(temp_str, sizeof(temp_str), "%d.%03d\n", temperature_micro_c / 10000, temperature_micro_c % 10000);

    if (*offset >= len)
        return 0;

    if (copy_to_user(buf, temp_str, len))
        return -EFAULT;

    *offset += len;
    return len;
}

// Character device open function
static int mcp9808_open(struct inode *inode, struct file *file)
{
    struct mcp9808_data *data = container_of(inode->i_cdev, struct mcp9808_data, cdev);
    file->private_data = data;

    dev_info(&data->client->dev, "Opened /dev/mcp9808\n");
    return 0;
}

// Character device file operations
static const struct file_operations mcp9808_fops = {
    .owner = THIS_MODULE,
    .open = mcp9808_open,
    .read = mcp9808_read,
};

// Probe function for initializing the device
static int mcp9808_probe(struct i2c_client *client)
{
    struct mcp9808_data *data;
    int ret;

    dev_info(&client->dev, "Probing MCP9808 sensor\n");

    data = devm_kzalloc(&client->dev, sizeof(struct mcp9808_data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->client = client;
    i2c_set_clientdata(client, data);

    // Set resolution to 0.125°C
    ret = set_resolution(client);
    if (ret) {
        dev_err(&client->dev, "Failed to set resolution\n");
        return ret;
    }

    // Allocate character device number
    ret = alloc_chrdev_region(&mcp9808_dev, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        dev_err(&client->dev, "Failed to allocate character device region\n");
        return ret;
    }

    // Initialize and add character device
    cdev_init(&data->cdev, &mcp9808_fops);
    data->cdev.owner = THIS_MODULE;
    ret = cdev_add(&data->cdev, mcp9808_dev, 1);
    if (ret) {
        dev_err(&client->dev, "Failed to add cdev\n");
        unregister_chrdev_region(mcp9808_dev, 1);
        return ret;
    }

    // Create device class in /dev
    mcp9808_class = class_create(DEVICE_NAME);
    if (IS_ERR(mcp9808_class)) {
        dev_err(&client->dev, "Failed to create device class\n");
        cdev_del(&data->cdev);
        unregister_chrdev_region(mcp9808_dev, 1);
        return PTR_ERR(mcp9808_class);
    }

    device_create(mcp9808_class, NULL, mcp9808_dev, NULL, DEVICE_NAME);

    dev_info(&client->dev, "MCP9808 temperature sensor initialized and /dev/%s created\n", DEVICE_NAME);
    return 0;
}

// Remove function for cleaning up the device
static void mcp9808_remove(struct i2c_client *client)
{
    struct mcp9808_data *data = i2c_get_clientdata(client);

    dev_info(&client->dev, "Removing MCP9808 temperature sensor\n");

    device_destroy(mcp9808_class, mcp9808_dev);
    class_destroy(mcp9808_class);
    cdev_del(&data->cdev);
    unregister_chrdev_region(mcp9808_dev, 1);

    dev_info(&client->dev, "MCP9808 temperature sensor and /dev/%s removed\n", DEVICE_NAME);
}

// I2C device ID table
static const struct i2c_device_id mcp9808_id[] = {
    { "mcp9808", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, mcp9808_id);

// I2C driver structure
static struct i2c_driver mcp9808_driver = {
    .driver = {
        .name = "mcp9808",
    },
    .probe = mcp9808_probe,
    .remove = mcp9808_remove,
    .id_table = mcp9808_id,
    .address_list = (const unsigned short[]){ MCP9808_I2C_ADDRESS, I2C_CLIENT_END },
};

module_i2c_driver(mcp9808_driver);

MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("MCP9808 Temperature Sensor Driver with 0.125°C Resolution and Thousandths Precision");
MODULE_LICENSE("GPL");

