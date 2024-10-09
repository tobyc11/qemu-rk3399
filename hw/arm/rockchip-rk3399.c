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
#include "hw/intc/arm_gicv3_its_common.h"
#include "hw/arm/bsa.h"
#include "sysemu/sysemu.h"
#include "hw/arm/rockchip-rk3399.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/cpu.h"
#include "target/arm/gtimer.h"
#include "qapi/qmp/qlist.h"

#define NUM_IRQS 256

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
    RK3399_DEV_RKTIMER,
    RK3399_DEV_GRF,
    RK3399_DEV_CPU_DEBUG0,
    RK3399_DEV_CPU_DEBUG1,
    RK3399_DEV_CPU_DEBUG2,
    RK3399_DEV_CPU_DEBUG3,
    RK3399_DEV_CPU_DEBUG4,
    RK3399_DEV_CPU_DEBUG5,
    RK3399_DEV_PMU,
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
    [RK3399_DEV_RKTIMER] = 0xff850000,
    [RK3399_DEV_GRF] = 0xff770000,
    [RK3399_DEV_CPU_DEBUG0] = 0xfe430000,
    [RK3399_DEV_CPU_DEBUG1] = 0xfe432000,
    [RK3399_DEV_CPU_DEBUG2] = 0xfe434000,
    [RK3399_DEV_CPU_DEBUG3] = 0xfe436000,
    [RK3399_DEV_CPU_DEBUG4] = 0xfe610000,
    [RK3399_DEV_CPU_DEBUG5] = 0xfe710000,
    [RK3399_DEV_PMU] = 0xff310000,
};

static void rockchip_rk3399_init(Object *obj)
{
    // RK3399State *s = ROCKCHIP_RK3399(obj);
    printf("rockchip_rk3399_init\n");
}

static void create_its(RK3399State *sms)
{
    const char *itsclass = its_class_name();
    DeviceState *dev;

    dev = qdev_new(itsclass);

    object_property_set_link(OBJECT(dev), "parent-gicv3", OBJECT(sms->gic),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, rockchip_rk3399_memmap[RK3399_DEV_GIC_ITS]);
}

static void create_gic(RK3399State *sms, MemoryRegion *mem)
{
    unsigned int smp_cpus = MACHINE(sms)->smp.cpus;
    SysBusDevice *gicbusdev;
    const char *gictype;
    uint32_t redist0_capacity, redist0_count;
    QList *redist_region_count;
    int i;

    gictype = gicv3_class_name();

    sms->gic = qdev_new(gictype);
    qdev_prop_set_uint32(sms->gic, "revision", 3);
    qdev_prop_set_uint32(sms->gic, "num-cpu", smp_cpus);
    /*
     * Note that the num-irq property counts both internal and external
     * interrupts; there are always 32 of the former (mandated by GIC spec).
     */
    qdev_prop_set_uint32(sms->gic, "num-irq", NUM_IRQS + 32);
    qdev_prop_set_bit(sms->gic, "has-security-extensions", true);

    redist0_capacity = 6;
    redist0_count = MIN(smp_cpus, redist0_capacity);

    redist_region_count = qlist_new();
    qlist_append_int(redist_region_count, redist0_count);
    qdev_prop_set_array(sms->gic, "redist-region-count", redist_region_count);

    object_property_set_link(OBJECT(sms->gic), "sysmem", OBJECT(mem), &error_fatal);
    qdev_prop_set_bit(sms->gic, "has-lpi", true);

    gicbusdev = SYS_BUS_DEVICE(sms->gic);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, rockchip_rk3399_memmap[RK3399_DEV_GICD]);
    sysbus_mmio_map(gicbusdev, 1, rockchip_rk3399_memmap[RK3399_DEV_GICR]);

    /*
     * Wire the outputs from each CPU's generic timer and the GICv3
     * maintenance interrupt signal to the appropriate GIC PPI inputs,
     * and the GIC's IRQ/FIQ/VIRQ/VFIQ interrupt outputs to the CPU's inputs.
     */
    for (i = 0; i < smp_cpus; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));
        int intidbase = NUM_IRQS + i * GIC_INTERNAL;
        int irq;
        /*
         * Mapping from the output timer irq lines from the CPU to the
         * GIC PPI inputs used for this board.
         */
        const int timer_irq[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,
            [GTIMER_HYPVIRT] = ARCH_TIMER_NS_EL2_VIRT_IRQ,
        };

        for (irq = 0; irq < ARRAY_SIZE(timer_irq); irq++) {
            qdev_connect_gpio_out(cpudev, irq,
                                  qdev_get_gpio_in(sms->gic,
                                                   intidbase + timer_irq[irq]));
        }

        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0,
                                    qdev_get_gpio_in(sms->gic,
                                                     intidbase
                                                     + ARCH_GIC_MAINT_IRQ));

        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(sms->gic,
                                                     intidbase
                                                     + VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(gicbusdev, i, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + smp_cpus, qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, i + 2 * smp_cpus, qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, i + 3 * smp_cpus, qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }
    create_its(sms);
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
    create_gic(s, sysmem);

    // Create CRU as dummy
    s->pmucru = qdev_new(TYPE_RK3399_PMUCRU);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->pmucru), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->pmucru), 0, rockchip_rk3399_memmap[RK3399_DEV_PMU_CRU]);

    s->cru = qdev_new(TYPE_RK3399_CRU);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->cru), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->cru), 0, rockchip_rk3399_memmap[RK3399_DEV_CRU]);

    // serial_mm_init(get_system_memory(), rockchip_rk3399_memmap[RK3399_DEV_UART0], 2,
    //                qdev_get_gpio_in(s->gic, 99),
    //                115200, serial_hd(2), DEVICE_NATIVE_ENDIAN);
    // serial_mm_init(get_system_memory(), rockchip_rk3399_memmap[RK3399_DEV_UART1], 2,
    //                qdev_get_gpio_in(s->gic, 98),
    //                115200, serial_hd(1), DEVICE_NATIVE_ENDIAN);
    serial_mm_init(get_system_memory(), rockchip_rk3399_memmap[RK3399_DEV_UART2], 2,
                   qdev_get_gpio_in(s->gic, 100),
                   115200, serial_hd(0), DEVICE_NATIVE_ENDIAN);
    // serial_mm_init(get_system_memory(), rockchip_rk3399_memmap[RK3399_DEV_UART3], 2,
    //                qdev_get_gpio_in(s->gic, 101),
    //                115200, serial_hd(3), DEVICE_NATIVE_ENDIAN);

    create_unimplemented_device("rktimer", rockchip_rk3399_memmap[RK3399_DEV_RKTIMER], 0x20);
    create_unimplemented_device("grf", rockchip_rk3399_memmap[RK3399_DEV_GRF], 0x10000);

    create_unimplemented_device("debug", rockchip_rk3399_memmap[RK3399_DEV_CPU_DEBUG0], 0x1000);
    create_unimplemented_device("debug", rockchip_rk3399_memmap[RK3399_DEV_CPU_DEBUG1], 0x1000);
    create_unimplemented_device("debug", rockchip_rk3399_memmap[RK3399_DEV_CPU_DEBUG2], 0x1000);
    create_unimplemented_device("debug", rockchip_rk3399_memmap[RK3399_DEV_CPU_DEBUG3], 0x1000);
    create_unimplemented_device("debug", rockchip_rk3399_memmap[RK3399_DEV_CPU_DEBUG4], 0x1000);
    create_unimplemented_device("debug", rockchip_rk3399_memmap[RK3399_DEV_CPU_DEBUG5], 0x1000);

    create_unimplemented_device("pmu", rockchip_rk3399_memmap[RK3399_DEV_PMU], 0x1000);

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
