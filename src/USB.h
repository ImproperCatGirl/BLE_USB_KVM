/* Forward declaration of the work item */
#include <stdint.h>
extern struct k_work usb_init_work;
void prep_usb_handler(struct k_work *work);

void USB_sub_report(int id, uint8_t *data, int size);