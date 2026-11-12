#define _POSIX_C_SOURCE 200809L

#include <libusb-1.0/libusb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

enum {
    CDC_ACM_SUBCLASS = 2,
    CDC_SET_LINE_CODING = 0x20,
    CDC_SET_CONTROL_LINE_STATE = 0x22,
    CDC_DTR_RTS = 3,
};

static volatile sig_atomic_t stopping;

static void stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int read_console(libusb_device_handle *device)
{
    struct libusb_config_descriptor *config;
    int err = libusb_get_active_config_descriptor(libusb_get_device(device), &config);
    if (err < 0)
        return err;

    int control = -1, data = -1, alternate = -1;
    uint8_t bulk_in = 0;
    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface *interface = &config->interface[i];
        for (int j = 0; j < interface->num_altsetting; j++) {
            const struct libusb_interface_descriptor *descriptor = &interface->altsetting[j];
            if (descriptor->bInterfaceClass == LIBUSB_CLASS_COMM &&
                descriptor->bInterfaceSubClass == CDC_ACM_SUBCLASS)
                control = descriptor->bInterfaceNumber;
            if (descriptor->bInterfaceClass != LIBUSB_CLASS_DATA)
                continue;
            for (int k = 0; k < descriptor->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor *endpoint = &descriptor->endpoint[k];
                if ((endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN) &&
                    (endpoint->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
                    data = descriptor->bInterfaceNumber;
                    alternate = descriptor->bAlternateSetting;
                    bulk_in = endpoint->bEndpointAddress;
                }
            }
        }
    }
    libusb_free_config_descriptor(config);
    if (control < 0 || data < 0 || !bulk_in)
        return LIBUSB_ERROR_NOT_FOUND;

    err = libusb_claim_interface(device, control);
    if (err < 0)
        return err;
    err = libusb_claim_interface(device, data);
    if (err < 0) {
        libusb_release_interface(device, control);
        return err;
    }

    if (alternate)
        err = libusb_set_interface_alt_setting(device, data, alternate);
    const uint8_t request_type = LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE;
    unsigned char line_coding[] = {0x00, 0xc2, 0x01, 0x00, 0, 0, 8};
    if (err >= 0)
        err = libusb_control_transfer(device, request_type, CDC_SET_LINE_CODING, 0, control,
                                      line_coding, sizeof line_coding, 1000);
    if (err >= 0)
        err = libusb_control_transfer(device, request_type, CDC_SET_CONTROL_LINE_STATE,
                                      CDC_DTR_RTS, control, NULL, 0, 1000);
    if (err >= 0) {
        puts("\n[Elden USB CDC connected]");
        fflush(stdout);
        while (!stopping) {
            unsigned char buffer[4096];
            int count = 0;
            err = libusb_bulk_transfer(device, bulk_in, buffer, sizeof buffer, &count, 500);
            if (count && (fwrite(buffer, 1, (size_t)count, stdout) != (size_t)count ||
                          fflush(stdout) == EOF)) {
                err = LIBUSB_ERROR_IO;
                break;
            }
            if (err < 0 && err != LIBUSB_ERROR_TIMEOUT)
                break;
        }
    }
    libusb_release_interface(device, data);
    libusb_release_interface(device, control);
    return err == LIBUSB_ERROR_NO_DEVICE || err == LIBUSB_ERROR_TIMEOUT ? 0 : err;
}

int main(void)
{
    if (access("/dev/bus/usb", X_OK) != 0)
        return 2;
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    libusb_context *context = NULL;
    int err = libusb_init(&context);
    if (err == LIBUSB_ERROR_NO_DEVICE)
        return 2;
    if (err < 0) {
        fprintf(stderr, "libusb init: %s\n", libusb_error_name(err));
        return 1;
    }
    libusb_device_handle *device = libusb_open_device_with_vid_pid(context, 0x1209, 0x7004);
    if (!device) {
        libusb_exit(context);
        return 2;
    }
    err = read_console(device);
    libusb_close(device);
    libusb_exit(context);
    if (err < 0)
        fprintf(stderr, "Elden CDC: %s\n", libusb_error_name(err));
    return err < 0;
}
