#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "hw/isa/isa.h"

#include "hw/ide/pci.h"
#include "ide-internal.h"
#include "trace.h"

#define IDE_CFR1 0x40
#define IDE_CFR2 0x41 
#define IDE_CFR3 0x42 
#define IDE_WBS 0x43 



#define CFR_INTR_CH1 0x01 
#define CFR_INTR_CH2 0x02 



static uint64_t bmdma_read(void *opaque, hwaddr addr,
                           unsigned size)
{
    BMDMAState *bm = opaque;
    uint32_t val;

    if (size != 1) {
        return ((uint64_t)1 << (size * 8)) - 1;
    }

    switch(addr & 3) {
    case 0:
        val = bm->cmd;
        break;
    case 2:
        val = bm->status;
        break;
    default:
        val = 0xff;
        break;
    }

    trace_bmdma_read_pc87560(addr, val);
    return val;
}

static void pc87560_update_irq(PCIDevice *pd)
{
    PCIIDEState *d = PCI_IDE(pd);
    uint8_t cntrl2 = pd->config[IDE_CFR2];
    int ch0_level = 0, ch1_level = 0;

    if (!(cntrl2 & CFR_INTR_CH1)) {
        ch0_level = !!(d->bmdma[0].status & BM_STATUS_INT); // check backend pending state
    }
    if (!(cntrl2 & CFR_INTR_CH2)) {
        ch1_level = !!(d->bmdma[1].status & BM_STATUS_INT);
    }

    pci_set_irq(pd, ch0_level || ch1_level);
}

static void bmdma_write(void *opaque, hwaddr addr,
                        uint64_t val, unsigned size)
{
    BMDMAState *bm = opaque;
    
    if (size != 1) {
        return;
    }

    trace_bmdma_write_pc87560(addr, val);
    switch(addr & 3) {
    case 0:
        bmdma_cmd_writeb(bm, val);
        break;
    case 2:
        bm->status = (val & 0x60) | (bm->status & 1) | (bm->status & ~val & 0x60);
        break;
    default:
    break;
    }
}

static const MemoryRegionOps pc87560_bmdma_ops = {
    .read = bmdma_read,
    .write = bmdma_write,
};

static void bmdma_setup_bar(PCIIDEState *d)
{
    BMDMAState *bm;
    int i;

    memory_region_init(&d->bmdma_bar, OBJECT(d), "pc87560-bmdma", 16);
    for(i = 0;i < 2; i++) {
        bm = &d->bmdma[i];
        memory_region_init_io(&bm->extra_io, OBJECT(d), &pc87560_bmdma_ops, bm,
                              "pc87560-bmdma-bus", 4);
        memory_region_add_subregion(&d->bmdma_bar, i * 8, &bm->extra_io);
        memory_region_init_io(&bm->addr_ioport, OBJECT(d),
                              &bmdma_addr_ioport_ops, bm,
                              "pc87560-bmdma-ioport", 4);
        memory_region_add_subregion(&d->bmdma_bar, i * 8 + 4, &bm->addr_ioport);
    }
}


static void pc87560_set_irq(void *opaque, int channel, int level)
{
    PCIIDEState *d = opaque;
    PCIDevice *pd = PCI_DEVICE(d);
    
    if (channel == 0) {
        d->bmdma[0].status = (d->bmdma[0].status & ~BM_STATUS_INT) | (level ? BM_STATUS_INT : 0);
    } else {
        d->bmdma[1].status = (d->bmdma[1].status & ~BM_STATUS_INT) | (level ? BM_STATUS_INT : 0);
    }
    
    pc87560_update_irq(pd);
}

static void pc87560_reset(Object *dev, ResetType type)
{
    PCIIDEState *d = PCI_IDE(dev);
    unsigned int i;

    for (i = 0; i < 2; i++) {
        ide_bus_reset(&d->bus[i]);
    }
}


static void pc87560_pci_config_write(PCIDevice *d, uint32_t addr, uint32_t val,
                                    int l)
{
    uint32_t i;

    pci_default_write_config(d, addr, val, l);

    for (i = addr; i < addr + l; i++) {
        switch (i) {
        case IDE_CFR2:
            pc87560_update_irq(d);
            break;
        }
    }
}

/* pc87560 PCI IDE controller */
static void pci_pc87560_ide_realize(PCIDevice *dev, Error **errp)
{
    
    PCIIDEState *d = PCI_IDE(dev);
    DeviceState *ds = DEVICE(dev);
    uint8_t *pci_conf = dev->config;
    int i;
    
    dev->cap_present |= QEMU_PCI_CAP_MULTIFUNCTION;
    pci_conf[PCI_HEADER_TYPE] |= PCI_HEADER_TYPE_MULTI_FUNCTION;
    
    PCIBus *pci_bus = pci_get_bus(dev);
    int dev_slot = PCI_SLOT(dev->devfn);

    // realize the other 2 functions on the same slot
    PCIDevice *f2_ohci = pci_new(PCI_DEVFN(dev_slot, 2), "pc87560-ohci");
    if (!qdev_realize_and_unref(DEVICE(f2_ohci), BUS(pci_bus), errp)) {
        return;
    }

    PCIDevice *f1_isa = pci_new(PCI_DEVFN(dev_slot, 1), "pc87560-superio");
    if (!qdev_realize_and_unref(DEVICE(f1_isa), BUS(pci_bus), errp)) {
        return;
    }
    
    
    pci_conf[PCI_CLASS_PROG] = 0x8a;
    
    dev->wmask[PCI_CLASS_PROG] = 0xff;
    
    
    // TODO make all of this with defines instead of values and confirm they are correct
    memset(&dev->wmask[IDE_CFR1],0xff,3);
    dev->wmask[IDE_WBS] = 0x00;;
    
    // 0x44 to 0x55 are read/write timing configuration registers 
    for (i = 0x44; i <= 0x55; i++) {
        dev->wmask[i] = 0xff;
    }
    // 0x58 to 0x5D are Read-Only values 
    for (i = 0x58; i <= 0x5d; i++) {
        dev->wmask[i] = 0x00;
    }
    
    pci_conf[IDE_CFR1] = 0x00;
    pci_conf[IDE_CFR2] = 0x00;
    pci_conf[IDE_CFR3] = 0x00;
    pci_conf[IDE_WBS]    = 0x00;
    
    for (i = 0x44; i <= 0x51; i++) {
        if (i != 0x46 && i != 0x47 &&
                i != 0x4A && i != 0x4B && i != 0x4E && i != 0x4F) {
            pci_conf[i] = 0x85;
        }
    }
    pci_conf[0x54] = 0xB7;
    pci_conf[0x55] = 0xEE;
    
    
    memory_region_init_io(&d->data_bar[0], OBJECT(d), &pci_ide_data_le_ops,
                          &d->bus[0], "pc87560-data0", 8);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &d->data_bar[0]);

    memory_region_init_io(&d->cmd_bar[0], OBJECT(d), &pci_ide_cmd_le_ops,
                          &d->bus[0], "pc87560-cmd0", 4);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &d->cmd_bar[0]);

    memory_region_init_io(&d->data_bar[1], OBJECT(d), &pci_ide_data_le_ops,
                          &d->bus[1], "pc87560-data1", 8);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_IO, &d->data_bar[1]);

    memory_region_init_io(&d->cmd_bar[1], OBJECT(d), &pci_ide_cmd_le_ops,
                          &d->bus[1], "pc87560-cmd1", 4);
    pci_register_bar(dev, 3, PCI_BASE_ADDRESS_SPACE_IO, &d->cmd_bar[1]);

    bmdma_setup_bar(d);
    pci_register_bar(dev, 4, PCI_BASE_ADDRESS_SPACE_IO, &d->bmdma_bar);

    pci_conf[PCI_INTERRUPT_PIN] = 0x01; // interrupt on pin 1

    qdev_init_gpio_in(ds, pc87560_set_irq, 2);
    for (i = 0; i < 2; i++) {
        ide_bus_init(&d->bus[i], sizeof(d->bus[i]), ds, i, 2);
        ide_bus_init_output_irq(&d->bus[i], qdev_get_gpio_in(ds, i));

        bmdma_init(&d->bus[i], &d->bmdma[i], d);
        d->bmdma[i].bus = &d->bus[i];
        ide_bus_register_restart_cb(&d->bus[i]);
    }
    
    
}

static void pci_pc87560_ide_exitfn(PCIDevice *dev)
{
    PCIIDEState *d = PCI_IDE(dev);
    unsigned i;

    for (i = 0; i < 2; ++i) {
        memory_region_del_subregion(&d->bmdma_bar, &d->bmdma[i].extra_io);
        memory_region_del_subregion(&d->bmdma_bar, &d->bmdma[i].addr_ioport);
    }
}


static void pc87560_ide_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = pc87560_reset;
    dc->vmsd = &vmstate_ide_pci;
    dc->user_creatable = true;
    k->realize = pci_pc87560_ide_realize;
    k->exit = pci_pc87560_ide_exitfn;
    k->vendor_id = 0x100B;
    k->device_id = 0x0002;
    k->revision = 0x02;
    k->subsystem_vendor_id = 0x103C; // HP susystem id 
    k->subsystem_id     = 0x10A7; 
    k->class_id = PCI_CLASS_STORAGE_IDE;
    k->config_write = pc87560_pci_config_write;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo pc87560_ide_info = {
    .name          = "pc87560-ide",
    .parent        = TYPE_PCI_IDE,
    .class_init    = pc87560_ide_class_init,
};

static void pc87560_ide_register_types(void)
{
    type_register_static(&pc87560_ide_info);
}

type_init(pc87560_ide_register_types)

