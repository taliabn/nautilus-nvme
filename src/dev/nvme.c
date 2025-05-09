/* 
 * This file is part of the Nautilus AeroKernel developed
 * by the Hobbes and V3VEE Projects with funding from the 
 * United States National  Science Foundation and the Department of Energy.  
 *
 * The V3VEE Project is a joint project between Northwestern University
 * and the University of New Mexico.  The Hobbes Project is a collaboration
 * led by Sandia National Laboratories that includes several national 
 * laboratories and universities. You can find out more at:
 * http://www.v3vee.org  and
 * http://xstack.sandia.gov/hobbes
 *
 * Copyright (c) 2025, Kyle C. Hale <kh@u.northwestern.edu>
 * Copyright (c) 2025, The V3VEE Project  <http://www.v3vee.org> 
 *                     The Hobbes Project <http://xstack.sandia.gov/hobbes>
 * All rights reserved.
 *
 * Authors: Talia Ben-Naim <taliaben-naim2025@u.northwestern.edu>
 *          Jack Riconosciuto <jackriconosciuto2027@u.northwestern.edu>
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "LICENSE.txt".
 */

#include <nautilus/nautilus.h>
#include <dev/nvme.h>
#include <nautilus/shell.h>
#include <nautilus/dev.h>

#ifndef NAUT_CONFIG_DEBUG_NVME
#undef DEBUG_PRINT
#define DEBUG_PRINT(fmt, args...) 
#endif

#define ERROR(fmt, args...) ERROR_PRINT("nvme: " fmt, ##args)
#define DEBUG(fmt, args...) DEBUG_PRINT("nvme: " fmt, ##args)
#define INFO(fmt, args...) INFO_PRINT("nvme: " fmt, ##args)

/* Work in progress NVMe driver*/

struct nvme_queue {
    uint64_t addr;
    uint64_t size;
};

struct nvme_sq {
    struct nvme_queue sq; 
};

struct nvme_cq {
    struct nvme_queue cq;
};

struct nvme_dev { // Based off of Rust drivers

    struct nk_dev dev; // necessary that it's first field (allegedly)

    char* pci_addr;
    uint8_t* addr;
    int len;
    uint16_t dstrd;
    struct nvme_sq admin_sq;
    struct nvme_cq admin_cq;
    struct nvme_sq io_sq; // For now 1
    struct nvme_cq io_cq; // For now 1
    uint8_t buffer; // Suggets 2 MiB buffer, but probably unnecesary since Nautilus doesn't page
    uint64_t prp_list[512];
    uint32_t* namespaces; // For now empty, can implement as linked list
    // stats?
    uint16_t q_id;
};


int nk_nvme_init(struct naut_info *naut)
{
    INFO("init\n");
    return 0;
}

void nk_nvme_deinit()
{
    INFO("deinit\n");
}

static int handle_nvmetest (char * buf, void * priv)
{
    nk_vc_printf("hello from nvme test!\n");
    return 0;
}

static struct shell_cmd_impl nvmetest_impl = {
    .cmd      = "nvmetest",
    .help_str = "nvmetest dev",
    .handler  = handle_nvmetest,
};
nk_register_shell_cmd(nvmetest_impl);
