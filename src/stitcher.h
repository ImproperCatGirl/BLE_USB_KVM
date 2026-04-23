
/**
 * @struct reverse_lookup_entry_t
 * @brief Maps a global report ID back to a specific device and its local report ID.
 */
 #include <stddef.h>
#include <stdint.h>


// --- Configuration ---
#define MAX_DEVICES 10
#define MAX_GLOBAL_REPORT_IDS 255
#define MAX_STITCHED_MAP_SIZE 2048
#define MAX_LOCAL_IDS_PER_DEVICE 16


typedef struct {
    uint8_t device_index; // Index of the device in the main device array
    uint8_t local_id;     // The original, local report ID from the device's map
} reverse_lookup_entry_t;

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

    // Reverse lookup table: global_id -> {device_index, local_id}
    reverse_lookup_entry_t reverse_lookup_table[MAX_GLOBAL_REPORT_IDS + 1];

    // Counters
    uint8_t next_global_id;
    uint8_t device_count;
} stitched_hid_info_t;



uint8_t device_local_2_global(int dev_unique_id, int local_report_id);
uint8_t globalid_2_local_dev_uid(int global_id);

int mian(device_info_t* raw_map, int dev_cnt);