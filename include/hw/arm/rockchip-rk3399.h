#pragma once
#include "hw/qdev-core.h"
#include "qemu/typedefs.h"

#define TYPE_ROCKCHIP_RK3399 MACHINE_TYPE_NAME("rockchip-rk3399")

OBJECT_DECLARE_SIMPLE_TYPE(RK3399State, ROCKCHIP_RK3399)

struct RK3399State {
    MachineState parent_obj;
    DeviceState *pmucru;
    DeviceState *cru;
    DeviceState *gic;
    struct arm_boot_info bootinfo;
};

#define TYPE_RK3399_PMUCRU "rk3399-pmucru"

OBJECT_DECLARE_SIMPLE_TYPE(RK3399PMUCRUState, RK3399_PMUCRU)

struct RK3399PMUCRUState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    char *buffer;
};

#define TYPE_RK3399_CRU "rk3399-cru"

OBJECT_DECLARE_SIMPLE_TYPE(RK3399CRUState, RK3399_CRU)

struct RK3399CRUState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    char *buffer;
};
