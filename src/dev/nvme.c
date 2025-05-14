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
#include <nautilus/endian.h>

#ifndef NAUT_CONFIG_DEBUG_NVME
#undef DEBUG_PRINT
#define DEBUG_PRINT(fmt, args...) 
#endif

#define ERROR(fmt, args...) ERROR_PRINT("nvme: " fmt, ##args)
#define DEBUG(fmt, args...) DEBUG_PRINT("nvme: " fmt, ##args)
#define INFO(fmt, args...) INFO_PRINT("nvme: " fmt, ##args)

/* Work in progress NVMe driver*/

// types
struct nvme_queue {
    uint64_t addr;
    uint64_t size;
};

typedef struct nvme_queue nvme_sq;

typedef struct nvme_queue nvme_cq;

struct nvme_dev { // Based off of Rust drivers

    struct nk_dev dev; // necessary that it's first field (allegedly)

    char* pci_addr;
    uint8_t* addr;
    int len;
    uint16_t dstrd;
    nvme_sq admin_sq;
    nvme_cq admin_cq;
    nvme_sq io_sq; // For now 1
    nvme_cq io_cq; // For now 1
    uint8_t buffer; // Suggets 2 MiB buffer, but probably unnecesary since Nautilus doesn't page
    uint64_t prp_list[512];
    uint32_t* namespaces; // For now empty, can implement as linked list
    // stats?
    uint16_t q_id;
};

// forward declarations:
int nvme_create_io_sq_cmd(struct nvme_dev *nvme, uint16_t sq_id, uint16_t cq_id, nvme_sq *io_sq);
int nvme_create_io_cq_cmd(struct nvme_dev *nvme, uint16_t cq_id, nvme_cq *io_cq);
void nvme_write_reg(uint32_t offset, uint32_t value){}; // TODO: actually write this

// Currently very based on OSDev, should modify to be more Nautilus
int create_admin_submission_queue(nvme_sq *sq) {
	sq->addr = (uint64_t)malloc(PAGE_SIZE); // IDK if this is still valid without paging
	if (sq->addr == 0) {
		return 1;
    }
	sq->size = 63;
	// 0x28 is the Admin Submission queue register
	nvme_write_reg(0x28, sq->addr);
	return 0;
}

int create_admin_completion_queue(nvme_cq *cq) {
	cq->addr = (uint64_t)malloc(PAGE_SIZE);
	if (cq->addr == 0) {
		return 1;
    }
	cq->size = 63;
	// 0x30 is the Admin Completion queue register
	nvme_write_reg(0x30, cq->addr);
	return 0;
}

int create_io_submission_queue(struct nvme_dev *nvme, nvme_sq *sq) {
	sq->addr = (uint64_t)malloc(PAGE_SIZE); // IDK if this is still valid without paging
	if (sq->addr == 0) {
		return 1;
    }
	sq->size = 63;
    // since we only have one io queue pair, just set id=0
    uint16_t sq_id = 0;
    uint16_t cq_id = 0;
	return nvme_create_io_sq_cmd(nvme, sq_id, cq_id, sq);
}

int create_io_completion_queue(struct nvme_dev *nvme, nvme_cq *cq) {
	cq->addr = (uint64_t)malloc(PAGE_SIZE);
	if (cq->addr == 0) {
		return 1;
    }
	cq->size = 63;
    // since we only have one io queue pair, just set id=0
    uint16_t cq_id = 0;
	return nvme_create_io_cq_cmd(nvme, cq_id, cq);
}

int nk_nvme_init(struct naut_info *naut)
{
    INFO("init\n");
    struct nvme_dev device; // Should be Malloc-d and passed onto device tree?

    if (create_admin_submission_queue(&(device.admin_sq)) ||
        create_admin_completion_queue(&(device.admin_cq))) 
    {
        ERROR("Failure to create admin queues\n");
        return 1;
    }

    // Creation of the IO queues is done WITH 
    // NVMe commands on the admin queues
    if (create_io_submission_queue(&device, &(device.io_sq)) ||
        create_io_completion_queue(&device, &(device.io_cq))) 
    {
        ERROR("Failure to create admin queues\n");
        return 1;
    }
    
    return 0;
}

void nk_nvme_deinit()
{
    INFO("deinit\n");
}

/* Command submisison helper functions */

static int nvme_queue_submit_cmd(struct nvme_queue *queue, struct nvme_command *cmd){
    DEBUG("command successfully submitted!\n");
    // TODO replace with actual function...
    // should also probably do something with the completion
    return 0;
}

static int nvme_submit_admin_cmd(struct nvme_dev *nvme, struct nvme_command *cmd)
{
    return nvme_queue_submit_cmd(&(nvme->admin_sq), cmd);
}

static int nvme_submit_io_cmd(struct nvme_dev *nvme, struct nvme_command *cmd)
{
    return nvme_queue_submit_cmd(&(nvme->io_sq), cmd);
}

/* IO Commands */

static int nvme_rw_cmd(struct nvme_dev *nvme, uint8_t opc, uint32_t nsid, 
                void *buff, uint64_t lba, uint32_t num_blocks)
{
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = opc;
	cmd.nsid = htole32(nsid);
	cmd.prp1 = (uintptr_t)buff;
    cmd.cdw10 = htole32(lba & 0xffffffffu);
	cmd.cdw11 = htole32(lba >> 32);
	cmd.cdw12 = htole32(num_blocks-1);

    return nvme_submit_io_cmd(nvme, &cmd);
}

int nvme_write_cmd(struct nvme_dev *nvme, uint32_t nsid, void *buff,
    uint64_t lba, uint32_t num_blocks)
{
	return nvme_rw_cmd(nvme, NVME_OPC_WRITE, nsid, buff, lba, num_blocks);
}

int nvme_read_cmd(struct nvme_dev *nvme, uint32_t nsid, void *buff, 
    uint64_t lba, uint32_t num_blocks)
{
	return nvme_rw_cmd(nvme, NVME_OPC_READ, nsid, buff, lba, num_blocks);
}

/* Admin Commands */

int nvme_identify_controller_cmd(struct nvme_dev *nvme, void *buff){
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = NVME_OPC_IDENTIFY;
	cmd.prp1 = (uintptr_t)buff; // command output (a single page)
    cmd.cdw10 = htole32(CONTROLLER);

    return nvme_submit_admin_cmd(nvme, &cmd);
}

int nvme_identify_ns_cmd(struct nvme_dev *nvme, void *buff, uint32_t nsid){
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = NVME_OPC_IDENTIFY;
	cmd.nsid = htole32(nsid);
	cmd.prp1 = (uintptr_t)buff; // command output (a single page)
    cmd.cdw10 = htole32(NAMESPACE);

    return nvme_submit_admin_cmd(nvme, &cmd);
}

int nvme_create_io_sq_cmd(struct nvme_dev *nvme, uint16_t sq_id, uint16_t cq_id, nvme_sq *io_sq){
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = NVME_OPC_CREATE_IO_SQ;
	cmd.prp1 = htole64(io_sq->addr);
	cmd.cdw10 = htole32(((io_sq->size-1) << 16) | sq_id);
	cmd.cdw11 = htole32((cq_id << 16) | 0x01);
    
    return nvme_submit_admin_cmd(nvme, &cmd); 
}

int nvme_create_io_cq_cmd(struct nvme_dev *nvme, uint16_t cq_id, nvme_cq *io_cq){
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = NVME_OPC_CREATE_IO_CQ;
	cmd.prp1 = htole64(io_cq->addr);
	cmd.cdw10 = htole32(((io_cq->size-1) << 16) | cq_id);
    // TODO: are we enabling interrupts? for now let's say no. 
    // otherwise, specify MSI vector + 1 in highword of DWORD11
	/* flags 0x1 = interrupts not enabled, physically contiguous */
	cmd.cdw11 = htole32(0x01);
    
    return nvme_submit_admin_cmd(nvme, &cmd); 
}

// this is a fairly meaningless test for now
static int handle_nvmetest (char *buf, void *priv)
{
    nk_vc_printf("hello from nvme test!\n");
    struct nvme_dev nvme;
    uint8_t data[2048];
    // qemu defaults nsid to zero
    uint32_t nsid = 0;
    nvme_identify_ns_cmd(&nvme, data, nsid);
    uint64_t lba = 0;
    uint32_t num_blocks = 1;
    nvme_read_cmd(&nvme, nsid, data, lba, num_blocks);
    nvme_write_cmd(&nvme, nsid, data, lba, num_blocks);
    return 0;
}

static struct shell_cmd_impl nvmetest_impl = {
    .cmd      = "nvmetest",
    .help_str = "nvmetest dev",
    .handler  = handle_nvmetest,
};
nk_register_shell_cmd(nvmetest_impl);
