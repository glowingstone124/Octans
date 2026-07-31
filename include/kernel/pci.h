#ifndef LAMP_KERNEL_PCI_H
#define LAMP_KERNEL_PCI_H

#include "types.h"

void pci_init(void);
uint32_t pci_ether_bar0(void);
uint32_t pci_gpu_bar0(void);
uint32_t pci_gpu_bar1(void);
uint32_t pci_audio_bar0(void);
uint32_t pci_device_count(void);

#endif
