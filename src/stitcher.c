
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "main.h"
// --- Configuration ---
#define MAX_DEVICES 10
#define MAX_GLOBAL_REPORT_IDS 255
#define MAX_STITCHED_MAP_SIZE 2048
#define MAX_LOCAL_IDS_PER_DEVICE 16


#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hid_stitcher, LOG_LEVEL_INF);

// Use your existing array
extern struct hid_instance devices[CONFIG_BT_MAX_CONN];

// --- Data Structures ---

/**
 * @struct reverse_lookup_entry_t
 * @brief Maps a global report ID back to a specific device and its local report ID.
 */
 typedef struct {
    int device_unique_id; // Using unique_id for easier identification
    uint8_t local_id;     // The original, local report ID from the device's map
} reverse_lookup_entry_t;

/**
 * @struct forward_lookup_entry_t
 * @brief Maps a device's unique ID and local report ID to a global report ID.
 */
 typedef struct {
    int device_unique_id;
    uint8_t local_id;
    uint8_t global_id;
} forward_lookup_entry_t;


/**
 * @struct device_info_t
 * @brief Holds information for a single HID device.
 */
typedef struct {
    uint8_t report_map[512]; // Pointer to the device's original report map
    size_t report_map_size;    // Size of the original report map
    int unique_id;
} device_info_t;

/**
 * @struct stitched_hid_info_t
 * @brief Contains the combined report map and all mapping information.
 */
 typedef struct {
    // The final, combined report map sent to the host
    uint8_t stitched_report_map[MAX_STITCHED_MAP_SIZE];
    size_t stitched_report_map_size;

    // Reverse lookup table: global_id -> {device_unique_id, local_id}
    reverse_lookup_entry_t reverse_lookup_table[MAX_GLOBAL_REPORT_IDS + 1];

    // --- NEW: Forward lookup table ---
    // {device_unique_id, local_id} -> global_id
    forward_lookup_entry_t forward_lookup_table[MAX_GLOBAL_REPORT_IDS];
    uint8_t forward_lookup_count;

    // Counters
    uint8_t next_global_id;
    uint8_t device_count;
} stitched_hid_info_t;


volatile stitched_hid_info_t stitched_info;
extern uint8_t usbTaskId;

// --- Core Functions ---

/**
 * @brief Initializes the stitched HID information structure.
 * @param stitched_info Pointer to the structure to initialize.
 */
void initialize_stitched_info(stitched_hid_info_t* stitched_info) {
    if (!stitched_info) return;
    memset(stitched_info, 0, sizeof(stitched_hid_info_t));
    // Start global IDs from 1, as 0 is often implicit.
    stitched_info->next_global_id = 1;
}


/**
 * @brief Stitches multiple HID report maps into a single map and creates lookup tables.
 *
 * This function iterates through a list of HID devices, parses their report maps,
 * and combines them into one large report map. It reassigns report IDs to a new
 * global namespace to prevent conflicts. It populates both a forward and reverse
 * lookup table.
 *
 * @param devices An array of device_info_t structures for each device to be included.
 * @param num_devices The number of devices in the array.
 * @param stitched_info A pointer to the structure where the final stitched map and lookup info will be stored.
 * @return 0 on success, -1 on failure (e.g., buffer overflow or invalid ID).
 */
 int stitch_hid_report_maps(device_info_t devices[], uint8_t num_devices, stitched_hid_info_t* stitched_info) {
    if (!devices || !stitched_info || num_devices > MAX_DEVICES) {
        return -1;
    }

    initialize_stitched_info(stitched_info);
    stitched_info->device_count = num_devices;

    size_t stitched_map_offset = 0;

    // Process each device
    for (uint8_t i = 0; i < num_devices; ++i) {
        if (devices[i].unique_id == 0) {
            fprintf(stderr, "Error: Device unique_id cannot be 0.\n");
            return -1;
        }

        const uint8_t* local_map = devices[i].report_map;
        size_t local_map_size = devices[i].report_map_size;
        size_t local_map_offset = 0;
        int has_explicit_id = 0;

        // Check if the device's map uses any explicit Report IDs
        for(size_t k = 0; k < local_map_size; ++k) {
            if (local_map[k] == 0x85) { // Report ID tag
                has_explicit_id = 1;
                break;
            }
        }

        // If no explicit ID is found, the whole report is implicitly ID 0.
        // We must assign it a global ID at the start.
        if (!has_explicit_id) {
            if (stitched_info->next_global_id > MAX_GLOBAL_REPORT_IDS ||
                stitched_info->forward_lookup_count >= MAX_GLOBAL_REPORT_IDS) {
                fprintf(stderr, "Error: Exceeded max global report IDs.\n");
                return -1;
            }
            if (stitched_map_offset + 2 > MAX_STITCHED_MAP_SIZE) {
                fprintf(stderr, "Error: Stitched map buffer overflow.\n");
                return -1;
            }
            
            uint8_t global_id = stitched_info->next_global_id++;
            uint8_t local_id = 0; // Implicit local ID is 0
            
            // Add the new global Report ID to the stitched map
            stitched_info->stitched_report_map[stitched_map_offset++] = 0x85; // Report ID Tag
            stitched_info->stitched_report_map[stitched_map_offset++] = global_id;

            // Populate the reverse lookup table
            stitched_info->reverse_lookup_table[global_id].device_unique_id = devices[i].unique_id;
            stitched_info->reverse_lookup_table[global_id].local_id = local_id;

            // --- NEW: Populate the forward lookup table ---
            stitched_info->forward_lookup_table[stitched_info->forward_lookup_count].device_unique_id = devices[i].unique_id;
            stitched_info->forward_lookup_table[stitched_info->forward_lookup_count].local_id = local_id;
            stitched_info->forward_lookup_table[stitched_info->forward_lookup_count].global_id = global_id;
            stitched_info->forward_lookup_count++;
        }

        // Parse the local report map and copy it to the stitched map
        while (local_map_offset < local_map_size) {
            uint8_t item = local_map[local_map_offset];

            // Check for Report ID item (prefix 100001, size 01, type 00 -> 0x85)
            if (item == 0x85) { // Report ID Tag
                if (stitched_info->next_global_id > MAX_GLOBAL_REPORT_IDS ||
                    stitched_info->forward_lookup_count >= MAX_GLOBAL_REPORT_IDS) {
                    fprintf(stderr, "Error: Exceeded max global report IDs.\n");
                    return -1;
                }
                
                local_map_offset++; // Move to the ID value
                if (local_map_offset >= local_map_size) break;

                uint8_t local_id = local_map[local_map_offset];
                uint8_t global_id = stitched_info->next_global_id++;
                
                // Populate the reverse lookup table
                stitched_info->reverse_lookup_table[global_id].device_unique_id = devices[i].unique_id;
                stitched_info->reverse_lookup_table[global_id].local_id = local_id;

                // --- NEW: Populate the forward lookup table ---
                stitched_info->forward_lookup_table[stitched_info->forward_lookup_count].device_unique_id = devices[i].unique_id;
                stitched_info->forward_lookup_table[stitched_info->forward_lookup_count].local_id = local_id;
                stitched_info->forward_lookup_table[stitched_info->forward_lookup_count].global_id = global_id;
                stitched_info->forward_lookup_count++;

                // Copy the Report ID item with the new global ID
                if (stitched_map_offset + 2 > MAX_STITCHED_MAP_SIZE) {
                    fprintf(stderr, "Error: Stitched map buffer overflow.\n");
                    return -1;
                }
                stitched_info->stitched_report_map[stitched_map_offset++] = 0x85;
                stitched_info->stitched_report_map[stitched_map_offset++] = global_id;

            } else {
                // Copy other items directly
                if (stitched_map_offset + 1 > MAX_STITCHED_MAP_SIZE) {
                    fprintf(stderr, "Error: Stitched map buffer overflow.\n");
                    return -1;
                }
                stitched_info->stitched_report_map[stitched_map_offset++] = item;
            }
            local_map_offset++;
        }
    }

    stitched_info->stitched_report_map_size = stitched_map_offset;
    return 0;
}


// --- NEW: Forward lookup function ---
/**
 * @brief Performs a forward lookup to find the global ID for a given device and local ID.
 * @param stitched_info Pointer to the populated stitched information structure.
 * @param device_unique_id The unique ID of the device sending the report.
 * @param local_id The local report ID from that device.
 * @return The corresponding global report ID, or 0 if not found.
 */
 uint8_t get_global_id(const stitched_hid_info_t* stitched_info, int device_unique_id, uint8_t local_id) {
    if (!stitched_info) return 0;

    for (uint8_t i = 0; i < stitched_info->forward_lookup_count; ++i) {
        const forward_lookup_entry_t* entry = &stitched_info->forward_lookup_table[i];
        if (entry->device_unique_id == device_unique_id && entry->local_id == local_id) {
            return entry->global_id;
        }
    }
    return 0; // Return 0 to indicate 'not found'
}



/**
 * @brief Prints the stitched report map in a readable hex format.
 */
void print_stitched_map(const stitched_hid_info_t* stitched_info) {
    printf("--- Stitched HID Report Map (Size: %zu) ---\n", stitched_info->stitched_report_map_size);
    for (size_t i = 0; i < stitched_info->stitched_report_map_size; ++i) {
        printf("0x%02X ", stitched_info->stitched_report_map[i]);
        if ((i + 1) % 16 == 0) {
            printf("\n");
        }
    }
    printf("\n\n");
}

void print_raw_map(const device_info_t* raw_map) {
    printf("--- Raw HID Report Map (Size: %zu) ---\n", raw_map->report_map_size);
    for (size_t i = 0; i < raw_map->report_map_size; ++i) {
        printf("0x%02X ", raw_map->report_map[i]);
        if ((i + 1) % 16 == 0) {
            printf("\n");
        }
    }
    printf("\n\n");
}
/**
 * @brief Prints the reverse lookup table.
 */
void print_reverse_lookup_table(const stitched_hid_info_t* stitched_info) {
    printf("--- Reverse Lookup Table ---\n");
    printf("Global ID | Device UID | Local ID\n");
    printf("----------------------------------\n");
    for (int i = 1; i < stitched_info->next_global_id; ++i) {
        printf("%-9d | %-12d | %-8d\n",
               i,
               stitched_info->reverse_lookup_table[i].device_unique_id,
               stitched_info->reverse_lookup_table[i].local_id);
    }
    printf("\n");
}

// --- NEW: Function to print the forward lookup table ---
void print_forward_lookup_table(const stitched_hid_info_t* stitched_info) {
    printf("--- Forward Lookup Table ---\n");
    printf("Device Unique ID | Local ID | Global ID\n");
    printf("--------------------------------------\n");
    for (int i = 0; i < stitched_info->forward_lookup_count; ++i) {
        const forward_lookup_entry_t* entry = &stitched_info->forward_lookup_table[i];
        printf("%-16d | %-8d | %-9d\n",
               entry->device_unique_id,
               entry->local_id,
               entry->global_id);
    }
    printf("\n");
}

uint8_t device_local_2_global(int dev_unique_id, int local_report_id)
{
    uint8_t found_global_id = get_global_id(&stitched_info, dev_unique_id, local_report_id);
    return found_global_id;
}
uint8_t globalid_2_local_dev_uid(int global_id)
{
    reverse_lookup_entry_t entry = stitched_info.reverse_lookup_table[global_id];
    return entry.device_unique_id;
}
// --- Example Usage ---

int mian(device_info_t* raw_map, int dev_cnt) {
    // Example Mouse Report Map (Implicit Report ID 0, but we will treat it as 1 for clarity in example)
    // For this example, let's pretend it has an explicit ID 1.
    const uint8_t mouse_report_map[] = {
        0x05, 0x01,       // Usage Page (Generic Desktop)
        0x09, 0x02,       // Usage (Mouse)
        0xA1, 0x01,       // Collection (Application)
        0x85, 0x01,       //   Report ID (1)
        0x09, 0x01,       //   Usage (Pointer)
        0xA1, 0x00,       //   Collection (Physical)
        0x05, 0x09,       //     Usage Page (Button)
        0x19, 0x01,       //     Usage Minimum (1)
        0x29, 0x03,       //     Usage Maximum (3)
        0x15, 0x00,       //     Logical Minimum (0)
        0x25, 0x01,       //     Logical Maximum (1)
        0x95, 0x03,       //     Report Count (3)
        0x75, 0x01,       //     Report Size (1)
        0x81, 0x02,       //     Input (Data,Var,Abs)
        0x95, 0x01,       //     Report Count (1)
        0x75, 0x05,       //     Report Size (5)
        0x81, 0x03,       //     Input (Cnst,Var,Abs)
        0x05, 0x01,       //     Usage Page (Generic Desktop)
        0x09, 0x30,       //     Usage (X)
        0x09, 0x31,       //     Usage (Y)
        0x15, 0x81,       //     Logical Minimum (-127)
        0x25, 0x7F,       //     Logical Maximum (127)
        0x75, 0x08,       //     Report Size (8)
        0x95, 0x02,       //     Report Count (2)
        0x81, 0x06,       //     Input (Data,Var,Rel)
        0xC0,             //   End Collection
        0xC0              // End Collection
    };

    // Example Keyboard Report Map (Report IDs 1 and 2)
    const uint8_t keyboard_report_map[] = {
        0x05, 0x01,       // Usage Page (Generic Desktop)
        0x09, 0x06,       // Usage (Keyboard)
        0xA1, 0x01,       // Collection (Application)
        // --- Regular Keys (Report ID 1) ---
        0x85, 0x01,       //   Report ID (1)
        0x05, 0x07,       //   Usage Page (Key Codes)
        0x19, 0xE0,       //   Usage Minimum (224)
        0x29, 0xE7,       //   Usage Maximum (231)
        0x15, 0x00,       //   Logical Minimum (0)
        0x25, 0x01,       //   Logical Maximum (1)
        0x75, 0x01,       //   Report Size (1)
        0x95, 0x08,       //   Report Count (8)
        0x81, 0x02,       //   Input (Data,Var,Abs) - Modifier keys
        0x95, 0x01,       //   Report Count (1)
        0x75, 0x08,       //   Report Size (8)
        0x81, 0x01,       //   Input (Cnst,Ary,Abs) - Reserved
        0x95, 0x06,       //   Report Count (6)
        0x75, 0x08,       //   Report Size (8)
        0x15, 0x00,       //   Logical Minimum (0)
        0x25, 0x65,       //   Logical Maximum (101)
        0x05, 0x07,       //   Usage Page (Key Codes)
        0x19, 0x00,       //   Usage Minimum (0)
        0x29, 0x65,       //   Usage Maximum (101)
        0x81, 0x00,       //   Input (Data,Ary,Abs) - Key array
        // --- Media Keys (Report ID 2) ---
        0x85, 0x02,       //   Report ID (2)
        0x05, 0x0C,       //   Usage Page (Consumer)
        0x09, 0x01,       //   Usage (Consumer Control)
        0xA1, 0x01,       //   Collection (Application)
        0x19, 0x00,       //   Usage Minimum (0)
        0x2A, 0x3C, 0x02, //   Usage Maximum (572)
        0x15, 0x00,       //   Logical Minimum (0)
        0x26, 0x3C, 0x02, //   Logical Maximum (572)
        0x95, 0x01,       //   Report Count (1)
        0x75, 0x10,       //   Report Size (16)
        0x81, 0x00,       //   Input (Data,Ary,Abs)
        0xC0,             //   End Collection
        0xC0              // End Collection
    };

    // Array of devices to stitch
    /*device_info_t devices_to_stitch[2];
    devices_to_stitch[0].report_map = mouse_report_map;
    devices_to_stitch[0].report_map_size = sizeof(mouse_report_map);

    devices_to_stitch[1].report_map = keyboard_report_map;
    devices_to_stitch[1].report_map_size = sizeof(keyboard_report_map);
*/
    // The structure to hold the final result

    for(int i = 0; i < dev_cnt; i++)
    {
        print_raw_map(&raw_map[i]);
    }
    // Perform the stitching
    printf("Stitching HID report maps...\n\n");
    if (stitch_hid_report_maps(raw_map, dev_cnt, &stitched_info) == 0) {
        printf("Stitching successful!\n\n");

        // Print the results
        print_stitched_map(&stitched_info);
        print_reverse_lookup_table(&stitched_info);
        print_forward_lookup_table(&stitched_info);
        // --- Example Reverse Lookup ---
        /*printf("--- Reverse Lookup Example ---\n");
        uint8_t incoming_global_id = 2; // Let's say we receive a report with global ID 2
        
        reverse_lookup_entry_t entry = stitched_info.reverse_lookup_table[incoming_global_id];
        printf("Host received a report with Global ID: %d\n", incoming_global_id);
        printf("This maps to Device Unique ID: %d, with Local ID: %d\n", entry.device_unique_id, entry.local_id);

*/  

    } else {
        fprintf(stderr, "Failed to stitch HID report maps.\n");
    }

    return 0;
}
