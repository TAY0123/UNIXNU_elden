// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef ELDEN_NOVATEK_DIGITIZER_H
#define ELDEN_NOVATEK_DIGITIZER_H

class IOService;
struct geni_spi;

// Attach a built-in HID child to a provider with a running NT36536.
// The provider owns the bus and serializes sample and release calls on
// its worker. Stop that worker before terminating and releasing the child.
IOService *elden_digitizer_attach(IOService *provider, struct geni_spi *spi);
int elden_digitizer_sample(IOService *digitizer);
int elden_digitizer_release_contacts(IOService *digitizer);

#endif
