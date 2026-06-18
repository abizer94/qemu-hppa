#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/char/serial.h"             
#include "hw/char/parallel.h"           
#include "hw/block/fdc-internal.h"   
#include "hw/isa/pc87560.h"  


static uint64_t pc87560_pp_read(void *opaque, hwaddr addr, unsigned size)
{
    ParallelState *s = opaque;
    switch (addr) {
    case 0: return s->datar;
    case 1: return s->status;
    case 2: return s->control;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "pc87560-pp: read  offset 0x%" HWADDR_PRIx "\n", addr);
        return 0xFF;
    }
}

static void pic_update_irq(PC87560SuperioState *s)
{
    int pending = s->pic.irr & ~s->pic.imr & ~s->pic.isr;
    pci_set_irq(PCI_DEVICE(s), pending != 0);
}

static uint64_t pic1_read(void *opaque, hwaddr addr, unsigned size)
{
    PC87560SuperioState *s = opaque;
    if (addr == 0) {
        if (s->pic.poll_mode) {
            /* OCW3 poll: return 0x80|IRQ# of highest pending unmasked */
            s->pic.poll_mode = false;
            uint8_t pending = s->pic.irr & ~s->pic.imr;
            if (!pending) return 0x00;  /* bit7=0: no interrupt */
            int irq = ctz32(pending);   /* lowest set bit */
            s->pic.isr |= (1 << irq);
            s->pic.irr &= ~(1 << irq);
            return 0x80 | irq;
        }
        return s->pic.read_isr ? s->pic.isr : s->pic.irr;
    }
    return s->pic.imr; /* offset 1: IMR */
}

static void pic1_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    PC87560SuperioState *s = opaque;
    uint8_t v = (uint8_t)val;

    if (addr == 1) {  /* data port */
        if (s->pic.init_phase > 0) {
            /* absorb ICW2, ICW3, ICW4 — only ICW4 ends the sequence */
            if (s->pic.init_phase == 3)  /* ICW4 received */
                s->pic.init_phase = 0;
            else
                s->pic.init_phase++;
        } else {
            s->pic.imr = v;  /* OCW1: mask register */
            pic_update_irq(s);
        }
        return;
    }

    /* addr == 0: command port */
    if (v & 0x10) {
        /* ICW1 — begin initialisation */
        s->pic.init_phase = 1;
        s->pic.imr = 0xFF;
        s->pic.irr = 0;
        s->pic.isr = 0;
        s->pic.poll_mode = false;
    } else if (v & 0x08) {
        /* OCW3 */
        if (v & 0x04) s->pic.poll_mode = true;
        if (v & 0x02) s->pic.read_isr = (v & 0x01);
    } else {
        /* OCW2 — EOI variants; bits[7:5] encode command */
        uint8_t cmd = (v >> 5) & 0x07;
        uint8_t irq = v & 0x07;
        switch (cmd) {
        case 1: /* non-specific EOI */
        case 5: /* rotate + non-specific EOI */
            s->pic.isr &= s->pic.isr - 1;  /* clear lowest set bit */
            break;
        case 3: /* specific EOI (SEOI — what the driver uses) */
        case 7: /* rotate + specific EOI */
            s->pic.isr &= ~(1 << irq);
            break;
        }
        pic_update_irq(s);
    }
}

static const MemoryRegionOps pc87560_pic1_ops = {
    .read  = pic1_read,
    .write = pic1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void pc87560_pic_irq_in(void *opaque, int irq_n, int level)
{
    PC87560SuperioState *s = opaque;
    if (level)
        s->pic.irr |=  (1 << irq_n);
    else
        s->pic.irr &= ~(1 << irq_n);
    pic_update_irq(s);
}


static void pc87560_pp_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    ParallelState *s = opaque;
    switch (addr) {
    case 0:
        s->dataw = (uint8_t)val;
        if (qemu_chr_fe_backend_connected(&s->chr)) {
            unsigned char ch = (uint8_t)val;
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
        }
        break;
    case 2:
        s->control = (uint8_t)(val & 0x1F);
        if (s->control & 0x10) {
            qemu_irq_raise(s->irq);
        } else {
            qemu_irq_lower(s->irq);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "pc87560-pp: write offset 0x%" HWADDR_PRIx
                      " val=0x%" PRIx64 "\n", addr, val);
    }
}

static const MemoryRegionOps pc87560_pp_ops = {
    .read  = pc87560_pp_read,
    .write = pc87560_pp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static uint64_t pc87560_stub_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "pc87560-superio: stub read  0x%" HWADDR_PRIx "\n", addr);
    return 0xFF;
}
static void pc87560_stub_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "pc87560-superio: stub write 0x%" HWADDR_PRIx
                  " = 0x%" PRIx64 "\n", addr, val);
}
static const MemoryRegionOps pc87560_stub_ops = {
    .read = pc87560_stub_read, .write = pc87560_stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t pc87560_fdc_read(void *opaque, hwaddr addr, unsigned size)
{
    return fdctrl_read(opaque, (uint32_t)addr);
}
static void pc87560_fdc_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    fdctrl_write(opaque, (uint32_t)addr, (uint32_t)val);
}
static const MemoryRegionOps pc87560_fdc_ops = {
    .read  = pc87560_fdc_read,
    .write = pc87560_fdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};



static void pc87560_map_region(PC87560SuperioState *s, MemoryRegion *mr,
                               bool *mapped, hwaddr base)
{
    MemoryRegion *io = pci_address_space_io(PCI_DEVICE(s));
    if (*mapped) {
        memory_region_del_subregion(io, mr);
        *mapped = false;
    }
    if (base) {
        memory_region_add_subregion(io, base, mr);
        *mapped = true;
    }
}






static void pc87560_apply_funcen(PC87560SuperioState *s)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint16_t en = pci_get_word(pci->config + REG_FUNCEN);

    pc87560_map_region(s, &s->fdc_io, &s->fdc_mapped,
        (en & FUNCEN_FDC)
            ? (pci_get_long(pci->config + REG_FDCBAR) & ~(hwaddr)0x7) : 0);

    pc87560_map_region(s, &s->sp1_io, &s->sp1_mapped,
        (en & FUNCEN_SP1)
            ? (pci_get_long(pci->config + REG_SP1BAR) & ~(hwaddr)0x7) : 0);

    pc87560_map_region(s, &s->sp2_io, &s->sp2_mapped,
        (en & FUNCEN_SP2)
            ? (pci_get_long(pci->config + REG_SP2BAR) & ~(hwaddr)0x7) : 0);

    pc87560_map_region(s, &s->pp_io, &s->pp_mapped,
        (en & FUNCEN_PP)
            ? (pci_get_long(pci->config + REG_PPBAR) & ~(hwaddr)0x7) : 0);

    pc87560_map_region(s, &s->kbc_io, &s->kbc_mapped,
        (en & FUNCEN_KBC)
            ? (pci_get_long(pci->config + REG_KBCBAR) & ~(hwaddr)0x7) : 0);
}

static void pc87560_superio_config_write(PCIDevice *pci,
                                    uint32_t addr, uint32_t val, int len)
{
    PC87560SuperioState *s = PC87560_Superio(pci);

    pci_default_write_config(pci, addr, val, len);

    switch (addr) {
    case REG_FUNCEN:
    case REG_FUNCEN + 1:
        pc87560_apply_funcen(s);
        break;

    case REG_FDCBAR:
        if (pci_get_word(pci->config + REG_FUNCEN) & FUNCEN_FDC) {
            pc87560_map_region(s, &s->fdc_io, &s->fdc_mapped,
                               pci_get_long(pci->config + REG_FDCBAR) & ~(hwaddr)0x7);
        }
        break;

    case REG_SP1BAR:
        if (pci_get_word(pci->config + REG_FUNCEN) & FUNCEN_SP1) {
            pc87560_map_region(s, &s->sp1_io, &s->sp1_mapped,
                               pci_get_long(pci->config + REG_SP1BAR) & ~(hwaddr)0x7);
        }
        break;

    case REG_SP2BAR:
        if (pci_get_word(pci->config + REG_FUNCEN) & FUNCEN_SP2) {
            pc87560_map_region(s, &s->sp2_io, &s->sp2_mapped,
                               pci_get_long(pci->config + REG_SP2BAR) & ~(hwaddr)0x7);
        }
        break;

    case REG_PPBAR:
        if (pci_get_word(pci->config + REG_FUNCEN) & FUNCEN_PP) {
            pc87560_map_region(s, &s->pp_io, &s->pp_mapped,
                               pci_get_long(pci->config + REG_PPBAR) & ~(hwaddr)0x7);
        }
        break;

    case REG_KBCBAR:
        if (pci_get_word(pci->config + REG_FUNCEN) & FUNCEN_KBC) {
            pc87560_map_region(s, &s->kbc_io, &s->kbc_mapped,
                               pci_get_long(pci->config + REG_KBCBAR) & ~(hwaddr)0x7);
        }
        break;

    case REG_ACPIBAR: {
        uint32_t ab = pci_get_long(pci->config + REG_ACPIBAR);
        pc87560_map_region(s, &s->acpi_io, &s->acpi_mapped,
                           (ab & 1) ? (ab & ~(hwaddr)0x1F) : 0);
        break;
    }
    case REG_PMBAR: {
        uint32_t pb = pci_get_long(pci->config + REG_PMBAR);
        pc87560_map_region(s, &s->pm_io, &s->pm_mapped,
                           (pb & 1) ? (pb & ~(hwaddr)0xFF) : 0);
        break;
    }
    case REG_RSVD_CFG:
        if ((pci->config[REG_RSVD_CFG] & 0x0F) != 0x01) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pc87560-superio: offset 0x7E bits[3:0] must be 0x01\n");
        }
        break;
    }
}

static void pc87560_superio_realize(PCIDevice *pci, Error **errp)
{
    PC87560SuperioState *s = PC87560_Superio(pci);
    MemoryRegion   *io = pci_address_space_io(pci);
    
    // multi function bit
    //pci->cap_present |= QEMU_PCI_CAP_MULTIFUNCTION;
    //pci->config[PCI_HEADER_TYPE] |= PCI_HEADER_TYPE_MULTI_FUNCTION;

    pci_set_word(pci->config + REG_FUNCEN,  FUNCEN_DEFAULT);
    pci->config[REG_RSVD_CFG]   = 0x01;
    pci->config[REG_DMA_ROUTE1] = 0x67;
    pci->config[REG_PPDID]      = 0x10;

    pci_set_long(pci->config + REG_KBCBAR,  0x00000060);
    pci_set_long(pci->config + REG_ACPIBAR, 0x00004001);
    pci_set_long(pci->config + REG_PMBAR,   0xFFFFFF01); 
    pci_set_long(pci->config + REG_FDCBAR,  0x000003F0);
    pci_set_long(pci->config + REG_SP1BAR,  0x000003F8);
    pci_set_long(pci->config + REG_SP2BAR,  0x000002F8);
    pci_set_long(pci->config + REG_PPBAR,   0x00000378);
    pci_set_long(pci->config + REG_PMBAR,   0xFFFFFF01);


    pci_set_long(pci->wmask + REG_KBCBAR,  0xFFFFFFF8);
    pci_set_long(pci->wmask + REG_ACPIBAR, 0xFFFFFFE1);
    pci_set_long(pci->wmask + REG_PMBAR,   0xFFFFFF01);
    pci_set_long(pci->wmask + REG_FDCBAR,  0xFFFFFFF8);
    pci_set_long(pci->wmask + REG_SP1BAR,  0xFFFFFFF8);
    pci_set_long(pci->wmask + REG_SP2BAR,  0xFFFFFFF8);
    pci_set_long(pci->wmask + REG_PPBAR,   0xFFFFFFF8);

    //pci->config[PCI_INTERRUPT_PIN] = 1;   // maybe comment this out 
    //s->irq = pci_allocate_irq(pci);       // cause its a bridge no irq 
    //s->pic.parent_irq = pci_allocate_irq(pci);
    pci->config[PCI_INTERRUPT_PIN] = 0x04; /* use INTD — matches USB slot */

    /* Expose 8 GPIO inputs (PIC IRQ 0–7) */
    qdev_init_gpio_in_named(DEVICE(pci), pc87560_pic_irq_in, "pic-irq", 8);

    /* Register real PIC ops instead of stubs */
    memory_region_init_io(&s->pic1_io, OBJECT(s), &pc87560_pic1_ops,
                          s, "pc87560-pic1", 2);
    memory_region_add_subregion(io, IC_PIC1, &s->pic1_io);
    /* PIC2 still stub — driver inits it but the real chip cascades it to PIC1 */
    
    memory_region_init_io(&s->pic2_io, OBJECT(s), &pc87560_stub_ops,
                          s, "pc87560-pic2", 2);
    memory_region_add_subregion(io, IC_PIC2, &s->pic2_io);    
    
    s->serial[0].irq = qemu_allocate_irq(pc87560_pic_irq_in, s, 3);
    if (!qdev_realize(DEVICE(&s->serial[0]), NULL, errp)) {
        return;
    }
    memory_region_init_io(&s->sp1_io, OBJECT(s), &serial_io_ops,
                          &s->serial[0], "pc87560-sp1", 8);
    memory_region_add_subregion(io, 0x3F8, &s->sp1_io);
    s->sp1_mapped = true;

    s->serial[1].irq = qemu_allocate_irq(pc87560_pic_irq_in, s, 4);
    if (!qdev_realize(DEVICE(&s->serial[1]), NULL, errp)) {
        return;
    }
    memory_region_init_io(&s->sp2_io, OBJECT(s), &serial_io_ops,
                          &s->serial[1], "pc87560-sp2", 8);
    memory_region_add_subregion(io, 0x2F8, &s->sp2_io);
    s->sp2_mapped = true;

    s->pp.irq        = qemu_allocate_irq(pc87560_pic_irq_in, s, 5);
    memory_region_init_io(&s->pp_io, OBJECT(s), &pc87560_pp_ops,
                          &s->pp, "pc87560-pp", 8);
    memory_region_add_subregion(io, 0x378, &s->pp_io);
    s->pp_mapped = true;


    s->fdc.irq       = qemu_allocate_irq(pc87560_pic_irq_in, s, 6);
    s->fdc.dma_chann = -1;
    s->fdc.dma       = NULL;
    s->fdc.fallback  = FLOPPY_DRIVE_TYPE_144;
    fdctrl_realize_common(DEVICE(pci), &s->fdc, errp);
    if (*errp) {
        return;
    }
    memory_region_init_io(&s->fdc_io, OBJECT(s), &pc87560_fdc_ops,
                          &s->fdc, "pc87560-fdc", 8);
    memory_region_add_subregion(io, 0x3F0, &s->fdc_io);
    s->fdc_mapped = true;

    memory_region_init_io(&s->kbc_io, OBJECT(s), &pc87560_stub_ops,
                          s, "pc87560-kbc", 8);
    memory_region_add_subregion(io, 0x60, &s->kbc_io);
    s->kbc_mapped = true;

    memory_region_init_io(&s->acpi_io, OBJECT(s), &pc87560_stub_ops,
                          s, "pc87560-acpi", 32);
    memory_region_add_subregion(io, 0x4000, &s->acpi_io);
    s->acpi_mapped = true;

    memory_region_init_io(&s->pm_io, OBJECT(s), &pc87560_stub_ops,
                          s, "pc87560-pm", 256);

    //memory_region_init_io(&s->pic1_io, OBJECT(s), &pc87560_stub_ops,
    //                      s, "pc87560-pic1", 2);
    //memory_region_add_subregion(io, IC_PIC1, &s->pic1_io);

    //memory_region_init_io(&s->pic2_io, OBJECT(s), &pc87560_stub_ops,
    //                      s, "pc87560-pic2", 2);
    //memory_region_add_subregion(io, IC_PIC2, &s->pic2_io);

}

static void pc87560_superio_instance_init(Object *obj)
{
    PC87560SuperioState *s = PC87560_Superio(obj);

    object_initialize_child(obj, "serial[*]", &s->serial[0], TYPE_SERIAL);
    object_initialize_child(obj, "serial[*]", &s->serial[1], TYPE_SERIAL);
}

static const VMStateDescription vmstate_pc87560_superio = {
    .name               = "pc87560-superio",
    .version_id         = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, PC87560SuperioState),
        VMSTATE_STRUCT_ARRAY(serial, PC87560SuperioState, 2, 0,
                             vmstate_serial, SerialState),
        VMSTATE_STRUCT(fdc, PC87560SuperioState, 0, vmstate_fdc, FDCtrl),
        VMSTATE_END_OF_LIST()
    },
};

static const Property pc87560_superio_properties[] = {
    DEFINE_PROP_CHR("serial0",  PC87560SuperioState, serial[0].chr),
    DEFINE_PROP_CHR("serial1",  PC87560SuperioState, serial[1].chr),
    DEFINE_PROP_CHR("parallel", PC87560SuperioState, pp.chr),
};

static void pc87560_superio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass    *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *k  = PCI_DEVICE_CLASS(oc);

    k->realize       = pc87560_superio_realize;
    k->config_write  = pc87560_superio_config_write;
    k->vendor_id     = 0x100B;
    k->device_id     = 0x000E;
    k->class_id      = 0x0680;
    k->revision      = 0x01;
    k->subsystem_vendor_id = 0x103C; // specific for hp idk maybe it helps 
    k->subsystem_id     = 0x10A7; 

    dc->desc           = "NS PC87560 SuperI/O Function 1 (I/O Peripherals)";
    dc->vmsd           = &vmstate_pc87560_superio;
    dc->user_creatable = true;
    device_class_set_props(dc, pc87560_superio_properties);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo pc87560_superio_info = {
    .name          = TYPE_PC87560_Superio,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PC87560SuperioState),
    .instance_init = pc87560_superio_instance_init,
    .class_init    = pc87560_superio_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void pc87560_superio_register_types(void)
{
    type_register_static(&pc87560_superio_info);
}

type_init(pc87560_superio_register_types)
