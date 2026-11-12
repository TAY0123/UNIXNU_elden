// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/extcon-provider.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>

static struct extcon_dev *usb_extcon;
static bool enable_host;
static struct clk *xhci_clock;

static int keep_usb_role(struct notifier_block *notifier, unsigned long state, void *data)
{
    return NOTIFY_STOP;
}

static int keep_usb_host_role(struct notifier_block *notifier, unsigned long state, void *data)
{
    return state ? NOTIFY_DONE : NOTIFY_STOP;
}

/* The manually bound xHCI driver must be unbound before a role change. */
static struct notifier_block usb_events = {
    .notifier_call = keep_usb_role,
    .priority = 100,
};
static struct notifier_block host_events = {
    .notifier_call = keep_usb_host_role,
    .priority = 100,
};

module_param(enable_host, bool, 0400);
MODULE_PARM_DESC(enable_host, "Assert the USB host role when there is no incoming VBUS");

static int __init elden_otg_init(void)
{
    struct device *controller;
    int device, host, ret;

    usb_extcon = extcon_get_extcon_dev("extcon_iddig");
    if (IS_ERR(usb_extcon))
        return PTR_ERR(usb_extcon);
    if (!usb_extcon)
        return -ENODEV;

    device = extcon_get_state(usb_extcon, EXTCON_USB);
    host = extcon_get_state(usb_extcon, EXTCON_USB_HOST);
    if (device < 0)
        return device;
    if (host < 0)
        return host;
    if (!enable_host)
        return 0;
    if (device || host)
        return -EBUSY;

    controller = bus_find_device_by_name(&platform_bus_type, NULL, "11211000.usb");
    if (!controller)
        return -ENODEV;
    xhci_clock = clk_get(controller, "xhci_ck");
    put_device(controller);
    if (IS_ERR(xhci_clock))
        return PTR_ERR(xhci_clock);

    ret = clk_prepare_enable(xhci_clock);
    if (ret) {
        clk_put(xhci_clock);
        return ret;
    }
    ret = extcon_register_notifier(usb_extcon, EXTCON_USB, &usb_events);
    if (ret)
        goto release_clock;
    ret = extcon_register_notifier(usb_extcon, EXTCON_USB_HOST, &host_events);
    if (ret)
        goto unregister_usb;

    ret = extcon_set_state_sync(usb_extcon, EXTCON_USB_HOST, true);
    if (!ret)
        return 0;
    extcon_unregister_notifier(usb_extcon, EXTCON_USB_HOST, &host_events);

unregister_usb:
    extcon_unregister_notifier(usb_extcon, EXTCON_USB, &usb_events);
release_clock:
    clk_disable_unprepare(xhci_clock);
    clk_put(xhci_clock);
    return ret;
}

static void __exit elden_otg_exit(void)
{
    if (!enable_host)
        return;
    extcon_unregister_notifier(usb_extcon, EXTCON_USB_HOST, &host_events);
    extcon_unregister_notifier(usb_extcon, EXTCON_USB, &usb_events);
    extcon_set_state(usb_extcon, EXTCON_USB_HOST, false);
    extcon_sync(usb_extcon, EXTCON_USB_HOST);
    if (extcon_get_state(usb_extcon, EXTCON_USB) > 0)
        extcon_sync(usb_extcon, EXTCON_USB);
    clk_disable_unprepare(xhci_clock);
    clk_put(xhci_clock);
}

module_init(elden_otg_init);
module_exit(elden_otg_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Temporary USB host role probe for Kindle Bellatrix");
