#include <stdio.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include "diagnostics.h"

static const struct i2c_dt_spec imu = I2C_DT_SPEC_GET(DT_NODELABEL(imu));
static atomic_t imu_ok;
static bool imu_initialized;

static int init_imu(void)
{
    uint8_t id = 0;
    int ret = i2c_reg_read_byte(imu.bus, imu.addr, 0x75, &id);

    printk("BOX3_IMU whoami=0x%x read=%d expected=0x60 ICM42607P\n", id, ret);
    if (ret < 0 || id != 0x60) {
        return ret < 0 ? ret : -ENODEV;
    }
    /* ICM42607P: power off, 100 Hz / 4 g / 500 dps, big-endian data, low-noise. */
    ret = i2c_reg_write_byte(imu.bus, imu.addr, 0x1f, 0x00);
    if (ret == 0) {
        ret = i2c_reg_write_byte(imu.bus, imu.addr, 0x20, 0x49);
    }
    if (ret == 0) {
        ret = i2c_reg_write_byte(imu.bus, imu.addr, 0x21, 0x49);
    }
    if (ret == 0) {
        ret = i2c_reg_update_byte(imu.bus, imu.addr, 0x35, BIT(4), BIT(4));
    }
    if (ret == 0) {
        ret = i2c_reg_write_byte(imu.bus, imu.addr, 0x1f, 0x0f);
    }
    k_msleep(50);
    return ret;
}
static atomic_t sd_result = ATOMIC_INIT(-EINPROGRESS);
static atomic_t radar_level;
static atomic_t ir_edges;
static struct gpio_callback ir_cb;

void dock_summary(char *buf, size_t len)
{
    snprintf(buf, len, "IMU: %s | SD: %ld\nRadar: %ld | IR edges: %ld",
             atomic_get(&imu_ok) ? "OK" : "OFF", (long)atomic_get(&sd_result),
             (long)atomic_get(&radar_level), (long)atomic_get(&ir_edges));
}

static void probe_sd(void)
{
    uint32_t sectors = 0;
    uint32_t sector_size = 0;
    static uint8_t sector[512] __aligned(4);
    int ret = disk_access_init("SD");

    printk("BOX3_SD init=%d\n", ret);
    atomic_set(&sd_result, ret);
    if (ret != 0) {
        return;
    }
    ret = disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_COUNT, &sectors);
    if (ret == 0) {
        ret = disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_SIZE, &sector_size);
    }
    printk("BOX3_SD geometry=%d sectors=%u sector_size=%u\n", ret, sectors, sector_size);
    if (ret == 0 && sector_size == sizeof(sector)) {
        ret = disk_access_read("SD", sector, 0, 1);
        printk("BOX3_SD read_sector0=%d readonly=1\n", ret);
    } else if (ret == 0) {
        ret = -ENOTSUP;
    }
    atomic_set(&sd_result, ret);
    ret = disk_access_ioctl("SD", DISK_IOCTL_CTRL_DEINIT, NULL);
    printk("BOX3_SD deinit=%d\n", ret);
}

static void ir_edge(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);
    atomic_inc(&ir_edges);
}

static void probe_ir(const struct device *gpio)
{
    const struct device *pwm = DEVICE_DT_GET(DT_NODELABEL(ledc0));
    /* SENSOR-02 schematic: RXD (GPIO44) drives active-low IR supply gate. */
    int ret = gpio_pin_configure(gpio, 12, GPIO_OUTPUT_LOW);

    if (ret == 0) {
        ret = gpio_pin_configure(gpio, 6, GPIO_INPUT);
    }
    if (ret == 0) {
        gpio_init_callback(&ir_cb, ir_edge, BIT(6));
        ret = gpio_add_callback(gpio, &ir_cb);
    }
    if (ret == 0) {
        ret = gpio_pin_interrupt_configure(gpio, 6, GPIO_INT_EDGE_BOTH);
    }
    printk("BOX3_IR receiver_init=%d\n", ret);
    if (!device_is_ready(pwm) || ret != 0) {
        return;
    }
    k_msleep(100);
    atomic_val_t before = atomic_get(&ir_edges);
    /* Bare 38 kHz carrier, not a remote-control command. */
    ret = pwm_set(pwm, 0, 26316, 13158, 0);
    k_msleep(20);
    int stop = pwm_set(pwm, 0, 26316, 0, 0);
    k_msleep(100);
    printk("BOX3_IR carrier_start=%d stop=%d loopback_edges=%ld\n",
           ret, stop, (long)(atomic_get(&ir_edges) - before));
}

static void read_imu(void)
{
    uint8_t data[12];
    int ret = imu_initialized ?
        i2c_burst_read(imu.bus, imu.addr, 0x0b, data, sizeof(data)) : -ENODEV;

    /* A transient NACK was observed during a tone; the data read is idempotent. */
    if (ret == -EIO) {
        k_msleep(2);
        ret = i2c_burst_read(imu.bus, imu.addr, 0x0b, data, sizeof(data));
        printk("BOX3_IMU read_retry_result=%d\n", ret);
    }
    int32_t accel[3] = {0};
    int32_t gyro[3] = {0};

    if (ret == 0) {
        for (int axis = 0; axis < 3; ++axis) {
            int16_t raw_a = (int16_t)sys_get_be16(&data[axis * 2]);
            int16_t raw_g = (int16_t)sys_get_be16(&data[6 + axis * 2]);

            if (raw_a == INT16_MIN || raw_g == INT16_MIN) {
                ret = -EAGAIN;
                break;
            }
            accel[axis] = (int32_t)raw_a * 4000 / 32768;
            gyro[axis] = (int32_t)((int64_t)raw_g * 500000 / 32768);
        }
    }
    atomic_set(&imu_ok, ret == 0);
    if (ret == 0) {
        printk("BOX3_IMU accel_mg=%d,%d,%d gyro_mdps=%d,%d,%d\n",
               accel[0], accel[1], accel[2], gyro[0], gyro[1], gyro[2]);
    } else {
        printk("BOX3_IMU sample_error=%d\n", ret);
    }
}

static void dock_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    const struct device *low = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    const struct device *high = DEVICE_DT_GET(DT_NODELABEL(gpio1));
    int ret = gpio_pin_configure(low, 21, GPIO_INPUT);

    printk("BOX3_RADAR input_config=%d\n", ret);
    int radar_ret = ret;
    probe_ir(high);
    int imu_ret = init_imu();
    imu_initialized = imu_ret == 0;
    printk("BOX3_IMU init=%d\n", imu_ret);
    probe_sd();
    int previous_radar = -1;
    int64_t next_imu = 0;
    while (1) {
        int radar = radar_ret == 0 ? gpio_pin_get(low, 21) : radar_ret;

        atomic_set(&radar_level, radar);
        if (radar != previous_radar) {
            printk("BOX3_DOCK radar_out=%d ir_edges=%ld\n",
                   radar, (long)atomic_get(&ir_edges));
            previous_radar = radar;
        }
        if (k_uptime_get() >= next_imu) {
            read_imu();
            next_imu = k_uptime_get() + 2000;
        }
        k_msleep(50);
    }
}

K_THREAD_DEFINE(dock_probe, 3072, dock_thread, NULL, NULL, NULL, 8, 0, 2000);
