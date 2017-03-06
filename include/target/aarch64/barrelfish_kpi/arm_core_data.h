/**
 * \file
 * \brief Data sent to a newly booted kernel
 */

/*
 * Copyright (c) 2012, 2017 ETH Zurich.
 * Copyright (c) 2015, 2016 Hewlett Packard Enterprise Development LP.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Universitaetstr. 6, CH-8092 Zurich. Attn: Systems Group.
 */

#ifndef _AARCH64_COREDATA_H
#define _AARCH64_COREDATA_H


struct armv8_coredata_elf {
    uint32_t    num;
    uint32_t    size;
    uint32_t    addr;
    uint32_t    shndx;
};

#define ARMV8_BOOTMAGIC_PSCI 0xb001b001
#define ARMV8_BOOTMAGIC_PARKING 0xb001b002

/**
 * \brief Data sent to a newly booted kernel
 *
 */
struct armv8_core_data {
    uint64_t boot_magic;
    lpaddr_t kernel_stack;
    lpaddr_t kernel_l0_pagetable;
    lpaddr_t kernel_global;

    lpaddr_t multiboot2; ///< The physical multiboot2 location
    uint64_t multiboot2_size;
    lpaddr_t efi_mmap;
    lpaddr_t module_start;  ///< The start of the cpu module
    lpaddr_t module_end;    ///< The end of the cpu module
    lpaddr_t urpc_frame_base;
    size_t urpc_frame_size;
    lpaddr_t monitor_binary;
    size_t monitor_binary_size;
    lpaddr_t memory_base_start;
    size_t memory_size;
    coreid_t src_core_id;
    uint64_t src_arch_id;
    coreid_t dst_core_id;
    char kernel_cmdline[128];

    lpaddr_t    initrd_start;
    lpaddr_t	initrd_size;


    uint64_t    start_kernel_ram; ///< The physical start of allocated kernel memory
    uint64_t    start_free_ram; ///< The physical start of free ram for the bsp allocator

    uint32_t    chan_id;

    genpaddr_t kcb; ///< The kernel control block

    struct armv8_coredata_elf elf;
}; //__attribute__ ((packed));

#define ARMV8_CORE_DATA_PAGES 1100


#endif
