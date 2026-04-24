
#include <zephyr/types.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>

#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/settings/settings.h>
static uint32_t kb_duration;
static bool kb_ready;


//struct usbd_context *sample_usbd;
const struct device *hid_dev0;
const struct device *hid_dev1;

#include <zephyr/usb/usbd.h>

#define MY_VID 0x2FE3  // Zephyr testing VID
#define MY_PID 0x0001  // Your custom PID

/* 1. Define String Descriptors (No sample Kconfig needed!) */
USBD_DESC_LANG_DEFINE(my_lang);
USBD_DESC_MANUFACTURER_DEFINE(my_mfr, "My KVM Project");
USBD_DESC_PRODUCT_DEFINE(my_product, "BLE-USB KVM Bridge");
/* 2. Define Configuration (Full Speed, Self-Powered, 100mA max) */

/* 3. Define the Device Context */
USBD_DEVICE_DEFINE(my_usbd_ctx, 
                   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 
                   MY_VID, MY_PID);

LOG_MODULE_REGISTER(USB, LOG_LEVEL_INF);

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(hs_cfg_desc, "HS Configuration");

/* doc configuration instantiation start */
static const uint8_t attributes = (IS_ENABLED(CONFIG_SAMPLE_USBD_SELF_POWERED) ?
				   USB_SCD_SELF_POWERED : 0) |
				  (IS_ENABLED(CONFIG_SAMPLE_USBD_REMOTE_WAKEUP) ?
				   USB_SCD_REMOTE_WAKEUP : 0);

/* Full speed configuration */
USBD_CONFIGURATION_DEFINE(sample_fs_config,
    attributes,
    1, &fs_cfg_desc);

/* Align the buffer to 4 bytes (pointer size) */
static uint8_t usb_report_buffer[64] __aligned(4);

int pre_init_usbd(void)
{
    int err;

    /* Add String Descriptors */
    err = usbd_add_descriptor(&my_usbd_ctx, &my_lang);
    err |= usbd_add_descriptor(&my_usbd_ctx, &my_mfr);
    err |= usbd_add_descriptor(&my_usbd_ctx, &my_product);
    if (err) {
        printk("Failed to add descriptors\n");
        return -1;
    }

    /* Add Full Speed Configuration */
    err = usbd_add_configuration(&my_usbd_ctx, USBD_SPEED_FS, &sample_fs_config);
    if (err) {
        printk("Failed to add FS configuration\n");
        return -1;
    }

    /* Register the HID Class specifically! 
     * In the new stack, it is identified as "hid_0" 
     * The last parameter '1' is the configuration number. */
    err = usbd_register_class(&my_usbd_ctx, "hid_0", USBD_SPEED_FS, 1);
    if (err) {
        printk("Failed to register HID class\n");
        return -1;
    }

	err = usbd_register_class(&my_usbd_ctx, "hid_1", USBD_SPEED_FS, 1);
    if (err) {
        printk("Failed to register HID class\n");
        return -1;
    }

    return 0; // Ready for usbd_init()!
}

static void kb_iface_ready(const struct device *dev, const bool ready)
{
	LOG_INF("HID device %s interface is %s",
		dev->name, ready ? "ready" : "not ready");
	kb_ready = ready;
}

static int kb_get_report(const struct device *dev,
			 const uint8_t type, const uint8_t id, const uint16_t len,
			 uint8_t *const buf)
{
    LOG_WRN("Host requested len: %u", len); // <--- LOG THIS
	LOG_WRN("Get Report Type %u ID %u", type, id);
	return 4;
}

static int kb_set_report(const struct device *dev,
			 const uint8_t type, const uint8_t id, const uint16_t len,
			 const uint8_t *const buf)
{
	if (type != HID_REPORT_TYPE_OUTPUT) {
		LOG_WRN("Unsupported report type");
		return -ENOTSUP;
	}
	return 0;
}

/* Idle duration is stored but not used to calculate idle reports. */
static void kb_set_idle(const struct device *dev,
			const uint8_t id, const uint32_t duration)
{
	LOG_INF("Set Idle %u to %u", id, duration);
	kb_duration = duration;
}

static uint32_t kb_get_idle(const struct device *dev, const uint8_t id)
{
	LOG_INF("Get Idle %u to %u", id, kb_duration);
	return kb_duration;
}

static void kb_set_protocol(const struct device *dev, const uint8_t proto)
{
	LOG_INF("Protocol changed to %s",
		proto == 0U ? "Boot Protocol" : "Report Protocol");
}

static void kb_output_report(const struct device *dev, const uint16_t len,
			     const uint8_t *const buf)
{
	LOG_HEXDUMP_DBG(buf, len, "o.r.");
	kb_set_report(dev, HID_REPORT_TYPE_OUTPUT, 0U, len, buf);
}


void input_done(const struct device *dev,
    const uint8_t *const report)
{
    //printk("input report done\n");
}

struct hid_device_ops kb_ops = {
	.iface_ready = kb_iface_ready,
	.get_report = kb_get_report,
	.set_report = kb_set_report,
	.set_idle = kb_set_idle,
	.get_idle = kb_get_idle,
	.set_protocol = kb_set_protocol,
	.output_report = kb_output_report,
    .input_report_done = input_done
};


extern uint8_t map[2][512];
extern int map_size[2];


extern bool usb_init_succ;
/* Outside of your functions */
void prep_usb_handler(struct k_work *work)
{
    printk("Starting USB Init for Dual HID Devices...\n");
    int ret;

    /* 1. Get and verify Device 0 */
    hid_dev0 = DEVICE_DT_GET(DT_NODELABEL(hid_dev_0));
    if (!device_is_ready(hid_dev0)) {
        printk("HID Device 0 is not ready\n");
        return;
    }

    /* 2. Get and verify Device 1 */
    hid_dev1 = DEVICE_DT_GET(DT_NODELABEL(hid_dev_1));
    if (!device_is_ready(hid_dev1)) {
        printk("HID Device 1 is not ready\n");
        return;
    }

    /* 3. Register Device 0 */
    ret = hid_device_register(hid_dev0, map[0], map_size[0], &kb_ops);
    if (ret != 0) {
        printk("Failed to register HID Device 0, %d\n", ret);
        return;
    }

    /* 4. Register Device 1 */
    /* Note: Ensure map1/kb_ops1 are defined if they differ from device 0 */
    ret = hid_device_register(hid_dev1, map[1], map_size[1], &kb_ops); 
    if (ret != 0) {
        printk("Failed to register HID Device 1, %d\n", ret);
        return;
    }

    /* 5. Global USB Stack Init */
    pre_init_usbd();

    int err = usbd_init(&my_usbd_ctx);
    if (err) {
        printk("Failed to initialize USB device support\n");
        return;
    }

    ret = usbd_enable(&my_usbd_ctx);
    if (ret) {
        printk("Failed to enable USB device support\n");
        return;
    }

    usb_init_succ = 1;
    printk("USB Dual HID Enabled!\n");
}

K_WORK_DEFINE(usb_init_work, prep_usb_handler);

void USB_sub_report(int dev_id, int report_id, uint8_t *data, int size)
{
    if(kb_ready)
    {
        usb_report_buffer[0] = report_id;
        memcpy(usb_report_buffer + 1, data, size);
		if(dev_id == 0)
		{
			hid_device_submit_report(hid_dev0, size + 1, usb_report_buffer);
		}
		if(dev_id == 1)
		{
			hid_device_submit_report(hid_dev1, size + 1, usb_report_buffer);
		}
	    //hid_device_submit_report(hid_dev, size + 1, usb_report_buffer);
    }
}