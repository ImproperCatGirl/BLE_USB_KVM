/* main.c - Application main entry point */

/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/gatt_dm.h>
#include <zephyr/sys/byteorder.h>
#include <bluetooth/scan.h>
#include <bluetooth/services/hogp.h>
#include <dk_buttons_and_leds.h>

#include <zephyr/logging/log.h>

#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/settings/settings.h>

#include "USB.h"
#include "zephyr/device.h"
#include "main.h"


LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);


/**
 * Switch between boot protocol and report protocol mode.
 */
#define KEY_BOOTMODE_MASK DK_BTN2_MSK
/**
 * Switch CAPSLOCK state.
 *
 * @note
 * For simplicity of the code it works only in boot mode.
 */
#define KEY_CAPSLOCK_MASK DK_BTN1_MSK
/**
 * Switch CAPSLOCK state with response
 *
 * Write CAPSLOCK with response.
 * Just for testing purposes.
 * The result should be the same like usine @ref KEY_CAPSLOCK_MASK
 */
#define KEY_CAPSLOCK_RSP_MASK DK_BTN3_MSK

/* Key used to accept or reject passkey value */
#define KEY_PAIRING_ACCEPT DK_BTN1_MSK
#define KEY_PAIRING_REJECT DK_BTN2_MSK

//static struct bt_conn *default_conn;
//static struct bt_hogp hogp;
static struct bt_conn *auth_conn;
static uint8_t capslock_state;

uint32_t mod = 0;

static void hids_on_ready(struct k_work *work);
static K_WORK_DEFINE(hids_ready_work, hids_on_ready);

#define MAX_HID_DEVICES CONFIG_BT_MAX_CONN



int total_map_finished_dev = 0;

static struct hid_instance devices[MAX_HID_DEVICES];


static void scan_filter_match(struct bt_scan_device_info *device_info,
			      struct bt_scan_filter_match *filter_match,
			      bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (!filter_match->uuid.match ||
	    (filter_match->uuid.count != 1)) {

		printk("Invalid device connected\n");

		return;
	}

	const struct bt_uuid *uuid = filter_match->uuid.uuid[0];

	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));

	printk("Filters matched on UUID 0x%04x.\nAddress: %s connectable: %s\n",
		BT_UUID_16(uuid)->val,
		addr, connectable ? "yes" : "no");
}

static void scan_connecting_error(struct bt_scan_device_info *device_info)
{
	printk("Connecting failed\n");
}

static void scan_connecting(struct bt_scan_device_info *device_info,
			    struct bt_conn *conn)
{
	//default_conn = bt_conn_ref(conn);
	for (int i = 0; i < MAX_HID_DEVICES; i++) {
        if (!devices[i].conn) {
            devices[i].conn = bt_conn_ref(conn);
            printk("Connecting to slot %d...\n", i);
            return;
        }
    }
    printk("No more HID slots available!\n");
}
/** .. include_startingpoint_scan_rst */
static void scan_filter_no_match(struct bt_scan_device_info *device_info,
				 bool connectable)
{
	/*int err;
	struct bt_conn *conn = NULL;
	char addr[BT_ADDR_LE_STR_LEN];

	if (device_info->recv_info->adv_type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		bt_addr_le_to_str(device_info->recv_info->addr, addr,
				  sizeof(addr));
		printk("Direct advertising received from %s\n", addr);
		bt_scan_stop();

		err = bt_conn_le_create(device_info->recv_info->addr,
					BT_CONN_LE_CREATE_CONN,
					device_info->conn_param, &conn);

		if (!err) {
			default_conn = bt_conn_ref(conn);
			bt_conn_unref(conn);
		}
	}*/
	int err;
    struct bt_conn *conn = NULL;
    char addr[BT_ADDR_LE_STR_LEN];
    int slot = -1;

    // We only care about Direct Advertising for reconnection
    if (device_info->recv_info->adv_type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
        return;
    }

    // 1. Find an available slot in your array
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        if (devices[i].conn == NULL) {
            slot = i;
            break;
        }
    }

    // If no slots are free, we can't connect to another device
    if (slot == -1) {
        printk("Direct advertising ignored: No free HID slots.\n");
        return;
    }

    bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
    printk("Direct advertising received from %s. Connecting to slot %d...\n", addr, slot);

    // 2. Stop scanning before initiating a manual connection
    bt_scan_stop();

    err = bt_conn_le_create(device_info->recv_info->addr,
                            BT_CONN_LE_CREATE_CONN,
                            device_info->conn_param, 
                            &conn);

    if (err) {
        printk("Create conn failed (err %d). Resuming scan.\n", err);
        bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
    } else {
        // 3. Store the connection reference in the found slot
        // Note: bt_conn_le_create returns a reference, so we don't need bt_conn_ref()
        // but we do need to unref the local pointer after assignment.
        devices[slot].conn = conn; 
        
        /* The 'connected' callback will now handle security and discovery 
         * using the connection stored in devices[slot]. */
    }
}
/** .. include_endpoint_scan_rst */
BT_SCAN_CB_INIT(scan_cb, scan_filter_match, scan_filter_no_match,
scan_connecting_error, scan_connecting);

static void discovery_completed_cb(struct bt_gatt_dm *dm, void *context)
{
	struct hid_instance *inst = context; 
	int err;

	err = bt_hogp_handles_assign(dm, &inst->hogp);
	if (err) {
		printk("Could not init HIDS client object, error: %d\n", err);
	} else {
		// Trigger the specific work item for THIS device
		k_work_submit(&inst->hids_ready_work);
	}

	bt_gatt_dm_data_release(dm);
	bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
}

static void discovery_service_not_found_cb(struct bt_conn *conn,
					   void *context)
{
	printk("The service could not be found during the discovery\n");
}

static void discovery_error_found_cb(struct bt_conn *conn,
				     int err,
				     void *context)
{
	printk("The discovery procedure failed with %d\n", err);
}

static const struct bt_gatt_dm_cb discovery_cb = {
	.completed = discovery_completed_cb,
	.service_not_found = discovery_service_not_found_cb,
	.error_found = discovery_error_found_cb,
};
static void gatt_discover(struct bt_conn *conn)
{
    struct hid_instance *inst = NULL;

    // Find which instance matches this connection
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        if (devices[i].conn == conn) {
            inst = &devices[i];
            break;
        }
    }

    if (!inst) return;

    // Pass the pointer to the instance as context
    int err = bt_gatt_dm_start(conn, BT_UUID_HIDS, &discovery_cb, inst);
    if (err) {
        printk("Discovery error: %d\n", err);
    }
}
static void connected(struct bt_conn *conn, uint8_t conn_err)
{
    int err;
    char addr[BT_ADDR_LE_STR_LEN];
    struct hid_instance *inst = NULL;
    int slot = -1;

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    // 1. Find which slot this connection belongs to
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        if (devices[i].conn == conn) {
            inst = &devices[i];
            slot = i;
            break;
        }
    }

    if (conn_err) {
        printk("Failed to connect to %s, 0x%02x %s\n", addr, conn_err,
               bt_hci_err_to_str(conn_err));

        // If we found the slot, clean it up
        if (inst) {
            bt_conn_unref(inst->conn);
            inst->conn = NULL;
        }

        /* Re-enable scanning so we can try to find other devices or retry */
        err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
        if (err) {
            printk("Scanning failed to start (err %d)\n", err);
        }

        return;
    }

    printk("Connected to slot [%d]: %s\n", slot, addr);

    // 2. Set security for this specific connection
    // HIDS usually requires at least Level 2 (Encryption)
    err = bt_conn_set_security(conn, BT_SECURITY_L2);
    if (err) {
        /* If security fails to initiate (not a security error, but a function error),
         * fall back to discovery. Usually, this happens if security is already met. */
        printk("Failed to initiate security: %d. Starting discovery anyway.\n", err);
        gatt_discover(conn);
    }
    
    /* NOTE: If bt_conn_set_security is successful, the 'security_changed' 
     * callback will be triggered. You should call gatt_discover() there 
     * to ensure the link is encrypted before reading HID data. */
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    char addr[BT_ADDR_LE_STR_LEN];
    int err;
    bool slot_cleared = false;

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("Disconnected: %s, reason 0x%02x %s\n", addr, reason, bt_hci_err_to_str(reason));

    // 1. Identify which slot this connection occupied
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        if (devices[i].conn == conn) {
            
            // 2. Release the HIDS client handles for this specific instance
            if (bt_hogp_assign_check(&devices[i].hogp)) {
                printk("HIDS client in slot [%d] active - releasing\n", i);
                bt_hogp_release(&devices[i].hogp);
            }

            // 3. Cleanup the connection reference
            bt_conn_unref(devices[i].conn);
            devices[i].conn = NULL;
            slot_cleared = true;
            devices[i].subscribed = false; // Reset here!
            break;
            break; 
        }
    }

    // 4. Handle auth_conn if it matches the disconnecting device
    if (auth_conn == conn) {
        bt_conn_unref(auth_conn);
        auth_conn = NULL;
    }

    // 5. Always attempt to restart scanning
    // This allows the host to find a replacement device for the empty slot.
    err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
    if (err && err != -EALREADY) {
        printk("Scanning failed to start (err %d)\n", err);
    }
}
static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		printk("Security changed: %s level %u\n", addr, level);
	} else {
		printk("Security failed: %s level %u err %d %s\n", addr, level, err,
		       bt_security_err_to_str(err));
	}

	gatt_discover(conn);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected        = connected,
	.disconnected     = disconnected,
	.security_changed = security_changed
};

static void scan_init(void)
{
	int err;

	struct bt_scan_init_param scan_init = {
		.connect_if_match = 1,
		.scan_param = NULL,
		.conn_param = BT_LE_CONN_PARAM_DEFAULT
	};

	bt_scan_init(&scan_init);
	bt_scan_cb_register(&scan_cb);

	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_HIDS);
	if (err) {
		printk("Scanning filters cannot be set (err %d)\n", err);

		return;
	}

	err = bt_scan_filter_enable(BT_SCAN_UUID_FILTER, false);
	if (err) {
		printk("Filters cannot be turned on (err %d)\n", err);
	}
}

bool usb_init_succ = 0;

static uint8_t hogp_notify_cb(struct bt_hogp *hogp,
			     struct bt_hogp_rep_info *rep,
			     uint8_t err,
			     const uint8_t *data)
{
	uint8_t size = bt_hogp_rep_size(rep);
	uint8_t i;

	if (!data) {
		return BT_GATT_ITER_STOP;
	}
	int device_idx = -1;
	for(int i = 0; i < MAX_HID_DEVICES; i++)
	{
		if(&devices[i].hogp == hogp)
		{
			device_idx = i;
			break;
		}
	}
	if(mod++ % 29 == 0)
	{
		printk("Notification, device %d, id: %u, size: %u, data:",device_idx,
			bt_hogp_rep_id(rep),
			size);
		for (i = 0; i < size; ++i) {
			printk(" 0x%x", data[i]);
		}
		printk("\n");
	}
	
	if(usb_init_succ)
	{
		//USB_sub_report(0, data, size);
		USB_sub_report(device_idx, bt_hogp_rep_id(rep), data, size);
	}
	return BT_GATT_ITER_CONTINUE;
}

static uint8_t hogp_boot_mouse_report(struct bt_hogp *hogp,
				     struct bt_hogp_rep_info *rep,
				     uint8_t err,
				     const uint8_t *data)
{
	uint8_t size = bt_hogp_rep_size(rep);
	uint8_t i;

	if (!data) {
		return BT_GATT_ITER_STOP;
	}
	printk("Notification, mouse boot, size: %u, data:", size);
	for (i = 0; i < size; ++i) {
		printk(" 0x%x", data[i]);
	}
	printk("\n");
	return BT_GATT_ITER_CONTINUE;
}

static uint8_t hogp_boot_kbd_report(struct bt_hogp *hogp,
				   struct bt_hogp_rep_info *rep,
				   uint8_t err,
				   const uint8_t *data)
{
	uint8_t size = bt_hogp_rep_size(rep);
	uint8_t i;

	if (!data) {
		return BT_GATT_ITER_STOP;
	}
	printk("Notification, keyboard boot, size: %u, data:", size);
	for (i = 0; i < size; ++i) {
		printk(" 0x%x", data[i]);
	}
	printk("\n");
	return BT_GATT_ITER_CONTINUE;
}

static void hogp_ready_cb(struct bt_hogp *hogp)
{
    // Find the parent instance that contains this specific hogp struct
    struct hid_instance *inst = CONTAINER_OF(hogp, struct hid_instance, hogp);

    printk("HOGP instance %p is ready, submitting work...\n", inst);

    // Submit the work item for THIS specific device instance
    k_work_submit(&inst->hids_ready_work);
}

uint8_t map[2][512];
int map_size[2];



static void map_cb(struct bt_hogp *hogp, uint8_t err,
                   const uint8_t *data, size_t size, size_t offset)
{

    if (err) {
        printk("Map read failed (err %d)\n", err);
        return;
    }

	int device_idx = -1;
	for(int i = 0; i < MAX_HID_DEVICES; i++)
	{
		if(&devices[i].hogp == hogp)
		{
			device_idx = i;
			break;
		}
	}
	if(device_idx < 0)
	{
		printk("ERROR: DEVICE NOT FOUND\n");
		return;
	}


    // If size is 0 or data is NULL, the device has no more data to send
    if (size == 0 || data == NULL) {
        printk("Full Report Map received!\n");
		//k_work_submit(&usb_init_work);
		devices[device_idx].map_cplt = 1;
		total_map_finished_dev ++;
		struct bt_hogp_rep_info *rep = NULL;
		if (devices[device_idx].subscribed) {
			printk("Instance %p already subscribed, skipping.\n", devices[device_idx]);
			return;
		}
		while (NULL != (rep = bt_hogp_rep_next(hogp, rep))) {
			if (bt_hogp_rep_type(rep) == BT_HIDS_REPORT_TYPE_INPUT) {
				printk("Subscribe to report id: %u for instance %p\n",
					   bt_hogp_rep_id(rep), devices[device_idx]);
				
				err = bt_hogp_rep_subscribe(hogp, rep, hogp_notify_cb);
				if (err) {
					printk("Subscribe error (%d)\n", err);
				}
			}
		}
	
		if (hogp->rep_boot.kbd_inp) {
			printk("Subscribe to boot keyboard report\n");
			err = bt_hogp_rep_subscribe(hogp, hogp->rep_boot.kbd_inp, hogp_boot_kbd_report);
			if (err) { printk("Subscribe error (%d)\n", err); }
		}
	
		if (hogp->rep_boot.mouse_inp) {
			printk("Subscribe to boot mouse report\n");
			err = bt_hogp_rep_subscribe(hogp, hogp->rep_boot.mouse_inp, hogp_boot_mouse_report);
			if (err) { printk("Subscribe error (%d)\n", err); }
		}
		devices[device_idx].subscribed = true;
        return;
    }

    // Print the current chunk
    printk("Chunk at offset %d (size %d):\n", offset, size);
    for (int i = 0; i < size; i++) {
		devices[device_idx].map[devices[device_idx].map_size ++]= data[i];
    }
    printk("\n");

    // TRIGGER THE NEXT CHUNK
    // We increment the offset and ask for the next part
    int next_err = bt_hogp_map_read(hogp, map_cb, offset + size, K_MSEC(100));
    if (next_err) {
        printk("Failed to request next chunk (err %d)\n", next_err);
    }
}

static void hids_on_ready(struct k_work *work)
{
    // Retrieve the specific instance this work belongs to
    struct hid_instance *inst = CONTAINER_OF(work, struct hid_instance, hids_ready_work);
    struct bt_hogp *hogp_ptr = &inst->hogp;
    int err;

    printk("HIDS instance %p is ready to work\n", inst);
    // Use hogp_ptr instead of the global hogp
	err = bt_hogp_map_read(hogp_ptr, map_cb, 0, K_MSEC(100));

	
    printk("HOGP Read MAP result = %d\n", err);

	
}

static void hogp_prep_fail_cb(struct bt_hogp *hogp, int err)
{
	printk("ERROR: HIDS client preparation failed!\n");
}

static void hogp_pm_update_cb(struct bt_hogp *hogp)
{
	printk("Protocol mode updated: %s\n",
	      bt_hogp_pm_get(hogp) == BT_HIDS_PM_BOOT ?
	      "BOOT" : "REPORT");
}


/* HIDS client initialization parameters */
static const struct bt_hogp_init_params hogp_init_params = {
	.ready_cb      = hogp_ready_cb,
	.prep_error_cb = hogp_prep_fail_cb,
	.pm_update_cb  = hogp_pm_update_cb,
};


static void hidc_write_cb(struct bt_hogp *hidc,
			  struct bt_hogp_rep_info *rep,
			  uint8_t err)
{
	printk("Caps lock sent\n");
}



static uint8_t capslock_read_cb(struct bt_hogp *hogp,
			     struct bt_hogp_rep_info *rep,
			     uint8_t err,
			     const uint8_t *data)
{
	if (err) {
		printk("Capslock read error (err: %u)\n", err);
		return BT_GATT_ITER_STOP;
	}
	if (!data) {
		printk("Capslock read - no data\n");
		return BT_GATT_ITER_STOP;
	}
	printk("Received data (size: %u, data[0]: 0x%x)\n",
	       bt_hogp_rep_size(rep), data[0]);

	return BT_GATT_ITER_STOP;
}


static void capslock_write_cb(struct bt_hogp *hogp,
			      struct bt_hogp_rep_info *rep,
			      uint8_t err)
{
	int ret;

	printk("Capslock write result: %u\n", err);

	ret = bt_hogp_rep_read(hogp, rep, capslock_read_cb);
	if (ret) {
		printk("Cannot read capslock value (err: %d)\n", ret);
	}
}





static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, passkey);
}


static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	auth_conn = bt_conn_ref(conn);

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, passkey);

	if (IS_ENABLED(CONFIG_SOC_SERIES_NRF54HX) || IS_ENABLED(CONFIG_SOC_SERIES_NRF54LX)) {
		printk("Press Button 0 to confirm, Button 1 to reject.\n");
	} else {
		printk("Press Button 1 to confirm, Button 2 to reject.\n");
	}
}


static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing cancelled: %s\n", addr);
}


static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing completed: %s, bonded: %d\n", addr, bonded);
}


static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing failed conn: %s, reason %d %s\n", addr, reason,
	       bt_security_err_to_str(reason));
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
	.passkey_display = auth_passkey_display,
	.passkey_confirm = auth_passkey_confirm,
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};


void bt_ctlr_assert_handle(char *x, int len)
{
	printk("BT ERR!\n%s", x);
}
int main(void)
{
	int err;

	printk("Starting Bluetooth Central HIDS sample\n");

	for (int i = 0; i < MAX_HID_DEVICES; i++) {
		k_work_init(&devices[i].hids_ready_work, hids_on_ready);
	}

	for(int i = 0; i < MAX_HID_DEVICES; i++)
	{
		bt_hogp_init(&devices[i].hogp, &hogp_init_params);
	}
	//bt_hogp_init(&hogp, &hogp_init_params);

	err = bt_conn_auth_cb_register(&conn_auth_callbacks);
	if (err) {
		printk("failed to register authorization callbacks.\n");
		return 0;
	}

	err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
	if (err) {
		printk("Failed to register authorization info callbacks.\n");
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	printk("Bluetooth initialized\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	scan_init();


	err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
		return 0;
	}

	printk("Scanning successfully started\n");

	bool usb_rdy = 0;
	while(1)
	{
		k_msleep(1000);
		if(total_map_finished_dev == 2)
		{
			for(int i = 0; i < MAX_HID_DEVICES; i++)
			{
				if(devices[i].map_cplt)
				{
					printk("MAP DEV %d:\n", i);
					for(int j = 0; j < devices[i].map_size; j++)
					{
						printk("%02X ", devices[i].map[j]);
						map[i][j] = devices[i].map[j];
						if(j%16 ==0)
						{
							printk("\n");
						}
					}
					printk("\n");
					map_size[i] = devices[i].map_size;
				}
			}

			if(usb_rdy == 0)
			{
				usb_rdy = 1;
				k_work_submit(&usb_init_work);
			}
			break;
		}
	}
	return 0;
}
