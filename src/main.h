#include "bluetooth/services/hogp.h"
#include <stdbool.h>
struct hid_instance {
    struct bt_conn *conn;
    struct bt_hogp hogp;
    bool busy; // To track if this slot is active
	struct k_work hids_ready_work; // Individual work item per device
	bool subscribed;
	uint8_t map[512];
	int map_size;
	bool map_cplt;
};