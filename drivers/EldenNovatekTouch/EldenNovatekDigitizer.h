// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef ELDEN_NOVATEK_DIGITIZER_H
#define ELDEN_NOVATEK_DIGITIZER_H

class IOService;
struct elden_touch_spi4;

// Attach a built-in HID child after NT36536 firmware and event setup.
// The provider owns the bus and serializes sample and release calls on
// its worker. Stop that worker before terminating and releasing the child.
IOService *elden_digitizer_attach(IOService *provider, struct elden_touch_spi4 *bus);
int elden_digitizer_sample(IOService *digitizer);
int elden_digitizer_release_contacts(IOService *digitizer);

#endif
