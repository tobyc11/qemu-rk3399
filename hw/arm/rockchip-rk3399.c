#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/typedefs.h"
#include "qemu/units.h"
#include "hw/qdev-core.h"
#include "hw/arm/boot.h"
#include "hw/boards.h"
#include "hw/sysbus.h"
#include "hw/char/serial.h"
#include "hw/misc/unimp.h"
#include "hw/usb/hcd-ehci.h"
#include "hw/loader.h"
#include "hw/intc/arm_gicv3_common.h"
#include "sysemu/sysemu.h"
#include "hw/arm/rockchip-rk3399.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/cpu.h"
#include "target/arm/gtimer.h"
#include "qapi/qmp/qlist.h"
#include <stdint.h>

enum {
    RK3399_SYSMEM,
    RK3399_DEV_GICD,
    RK3399_DEV_GICR,
    RK3399_DEV_GIC_ITS,
    RK3399_DEV_PMU_CRU,
    RK3399_DEV_CRU,
    RK3399_DEV_PMU_PMUGRF,
    RK3399_DEV_UART0,
    RK3399_DEV_UART1,
    RK3399_DEV_UART2,
    RK3399_DEV_UART3,
};

/* Memory map */
const hwaddr rockchip_rk3399_memmap[] = {
    [RK3399_SYSMEM] = 0,
    [RK3399_DEV_GICD] = 0xfee00000,
    [RK3399_DEV_GICR] = 0xfef00000,
    [RK3399_DEV_GIC_ITS] = 0xfee20000,
    [RK3399_DEV_PMU_CRU] = 0xff750000,
    [RK3399_DEV_CRU] = 0xff760000,
    [RK3399_DEV_PMU_PMUGRF] = 0xff770000,
    [RK3399_DEV_UART0] = 0xff180000,
    [RK3399_DEV_UART1] = 0xff190000,
    [RK3399_DEV_UART2] = 0xff1a0000,
    [RK3399_DEV_UART3] = 0xff1b0000,
};

static void rockchip_rk3399_init(Object *obj)
{
    // RK3399State *s = ROCKCHIP_RK3399(obj);
    printf("rockchip_rk3399_init\n");
}

static void rockchip_rk3399_machine_init(MachineState *ms)
{
    RK3399State *s = ROCKCHIP_RK3399(ms);
    MemoryRegion *sysmem = get_system_memory();
    
    printf("rockchip_rk3399_machine_init\n");

    printf("ram: %p\n", ms->ram);
    memory_region_add_subregion(sysmem, RK3399_SYSMEM, ms->ram);

    printf("CPU type %s\n", ms->cpu_type);
    int n;
    int smp_cpus = ms->smp.cpus;
    for (n = 0; n < smp_cpus; n++) {
        Object *cpuobj = object_new(ms->cpu_type);

        object_property_set_bool(cpuobj, "has_el3", false, NULL);

        if (object_property_find(cpuobj, "has_el2")) {
            object_property_set_bool(cpuobj, "has_el2", false, NULL);
        }

        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
    }

    // Create GIC
    s->gic = qdev_new(gicv3_class_name());
    qdev_prop_set_uint32(s->gic, "revision", 3);
    qdev_prop_set_uint32(s->gic, "num-cpu", smp_cpus);
    /* Note that the num-irq property counts both internal and external
     * interrupts; there are always 32 of the former (mandated by GIC spec).
     */
#define NUM_IRQS 256
    qdev_prop_set_uint32(s->gic, "num-irq", NUM_IRQS + 32);

    QList *redist_region_count;
    redist_region_count = qlist_new();
    qlist_append_int(redist_region_count, smp_cpus);
    qdev_prop_set_array(s->gic, "redist-region-count", redist_region_count);

    object_property_set_link(OBJECT(s->gic), "sysmem", OBJECT(ms->ram), &error_fatal);
    qdev_prop_set_bit(s->gic, "has-lpi", true);

    SysBusDevice *gicbusdev;
    gicbusdev = SYS_BUS_DEVICE(s->gic);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, rockchip_rk3399_memmap[RK3399_DEV_GICD]);
    sysbus_mmio_map(gicbusdev, 1, rockchip_rk3399_memmap[RK3399_DEV_GICR]);

    create_unimplemented_device("its", rockchip_rk3399_memmap[RK3399_DEV_GIC_ITS], 0x20000);

    // Create CRU as dummy
    s->pmucru = qdev_new(TYPE_RK3399_PMUCRU);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->pmucru), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->pmucru), 0, rockchip_rk3399_memmap[RK3399_DEV_PMU_CRU]);

    s->cru = qdev_new(TYPE_RK3399_CRU);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->cru), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->cru), 0, rockchip_rk3399_memmap[RK3399_DEV_CRU]);

    create_unimplemented_device("uart0", rockchip_rk3399_memmap[RK3399_DEV_UART0], 0x100);
    create_unimplemented_device("uart1", rockchip_rk3399_memmap[RK3399_DEV_UART1], 0x100);
    create_unimplemented_device("uart2", rockchip_rk3399_memmap[RK3399_DEV_UART2], 0x100);
    create_unimplemented_device("uart3", rockchip_rk3399_memmap[RK3399_DEV_UART3], 0x100);

    // Boot
    s->bootinfo.ram_size = ms->ram_size;
    s->bootinfo.board_id = -1;
    s->bootinfo.loader_start = RK3399_SYSMEM;
    // s->bootinfo.get_dtb = machvirt_dtb;
    // s->bootinfo.skip_dtb_autoload = true;
    s->bootinfo.firmware_loaded = false;
    s->bootinfo.psci_conduit = QEMU_PSCI_CONDUIT_SMC;
    arm_load_kernel(ARM_CPU(first_cpu), ms, &s->bootinfo);
}

static void rockchip_rk3399_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = rockchip_rk3399_machine_init;
    mc->desc = "Rockchip RK3399 Development Board";
    mc->max_cpus = 4;
    mc->default_ram_id = "rk3399.highmem";
    
    printf("rockchip_rk3399_class_init\n");
}

static const TypeInfo rockchip_rk3399_type_info = {
    .name = TYPE_ROCKCHIP_RK3399,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(RK3399State),
    .instance_init = rockchip_rk3399_init,
    .class_init = rockchip_rk3399_class_init,
};

static void rockchip_rk3399_register_types(void)
{
    type_register_static(&rockchip_rk3399_type_info);
}

type_init(rockchip_rk3399_register_types)

// Below is implementation of RK3399_PMUCRU
static uint64_t rk3399_pmucru_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3399PMUCRUState *s = RK3399_PMUCRU(opaque);
    switch (size) {
    case 4:
        printf("rk3399_pmucru_read: offset=0x%" HWADDR_PRIx " value=0x%" PRIx32 "\n", offset, *(uint32_t *)(s->buffer + offset));
        return *(uint32_t *)(s->buffer + offset);
    default:
        printf("rk3399_pmucru_read: size=%d not supported\n", size);
        return 0;
    }
}

static void rk3399_pmucru_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    printf("rk3399_pmucru_write: offset=0x%" HWADDR_PRIx " value=0x%" PRIx64 "\n", offset, value);

    if (offset == 8) {
        value |= 1 << 31;
    }

    RK3399PMUCRUState *s = RK3399_PMUCRU(opaque);
    switch (size) {
    case 4:
        *(uint32_t *)(s->buffer + offset) = (uint32_t)value;
        return;
    }

    // Not supported
    printf("rk3399_pmucru_write: size=%d not supported\n", size);
}

static const MemoryRegionOps rk3399_pmucru_ops = {
    .read = rk3399_pmucru_read,
    .write = rk3399_pmucru_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void rk3399_pmucru_realize(DeviceState *dev, Error **errp)
{
    RK3399PMUCRUState *s = RK3399_PMUCRU(dev);
    s->buffer = malloc(0x1000);
    memset(s->buffer, 0, 0x1000);

    *(uint32_t *)(s->buffer + 0x8) = 0x0000031f;
    *(uint32_t *)(s->buffer + 0x90) = 0x2dc;

    memory_region_init_io(&s->iomem, OBJECT(dev), &rk3399_pmucru_ops, s, "rk3399.pmucru", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void rk3399_pmucru_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rk3399_pmucru_realize;
}

static const TypeInfo rk3399_pmucru_info = {
    .name = TYPE_RK3399_PMUCRU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3399PMUCRUState),
    .class_init = rk3399_pmucru_class_init,
};

static void rk3399_pmucru_register_types(void)
{
    type_register_static(&rk3399_pmucru_info);
}

type_init(rk3399_pmucru_register_types)

// Below is implementation of RK3399_CRU
static uint64_t rk3399_cru_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3399CRUState *s = RK3399_CRU(opaque);
    switch (size) {
    case 4:
        printf("rk3399_cru_read: offset=0x%" HWADDR_PRIx " value=0x%" PRIx32 "\n", offset, *(uint32_t *)(s->buffer + offset));
        return *(uint32_t *)(s->buffer + offset);
    default:
        printf("rk3399_cru_read: size=%d not supported\n", size);
        return 0;
    }
}

static void rk3399_cru_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    printf("rk3399_cru_write: offset=0x%" HWADDR_PRIx " value=0x%" PRIx64 "\n", offset, value);

    // Always set bit 31 to report PLL as locked. Linux driver spins on this bit
    if (offset < 0x100 && (offset % 0x20) == 0x8) {
        value |= 1 << 31;
    }

    RK3399CRUState *s = RK3399_CRU(opaque);
    switch (size) {
    case 4:
        *(uint32_t *)(s->buffer + offset) = (uint32_t)value;
        return;
    }

    // Not supported
    printf("rk3399_cru_write: size=%d not supported\n", size);
}

static const MemoryRegionOps rk3399_cru_ops = {
    .read = rk3399_cru_read,
    .write = rk3399_cru_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void rk3399_cru_realize(DeviceState *dev, Error **errp)
{
    RK3399CRUState *s = RK3399_CRU(dev);
    s->buffer = malloc(0x1000);
    memset(s->buffer, 0, 0x1000);

    *(uint32_t *)(s->buffer + 0x8) = 0x0000031f;
    *(uint32_t *)(s->buffer + 0x90) = 0x2dc;

    memory_region_init_io(&s->iomem, OBJECT(dev), &rk3399_cru_ops, s, "rk3399.cru", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void rk3399_cru_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rk3399_cru_realize;
}

static const TypeInfo rk3399_cru_info = {
    .name = TYPE_RK3399_CRU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3399CRUState),
    .class_init = rk3399_cru_class_init,
};

static void rk3399_cru_register_types(void)
{
    type_register_static(&rk3399_cru_info);
}

type_init(rk3399_cru_register_types)
