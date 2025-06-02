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

// Sources:
    // https://github.com/torvalds/linux/tree/master/drivers/nvme
    // https://github.com/freebsd/freebsd-src/tree/main/sys/dev/nvme
    // https://wiki.osdev.org/NVMe

#ifndef __NVME_H__
#define __NVME_H__

/* Work in progress NVMe driver*/
int  nk_nvme_init(struct naut_info *naut);
void nk_nvme_deinit();

/* NVMe Macros */
// PCI Config
#define NVME_VENDOR_ID 0x1b36
#define NVME_DEVICE_ID 0x0010
#define NVME_BAR0 0xfebb0000
#define NVME_PCI_CMD_OFFSET         0x4    // Device Control - RW
#define NVME_PCI_STATUS_OFFSET      0x6    // Device Status - RO
// PCI command register
#define NVME_PCI_CMD_IO_ACCESS_EN   1       // io access enable
#define NVME_PCI_CMD_MEM_ACCESS_EN  (1<<1)  // memory access enable
#define NVME_PCI_CMD_LANRW_EN       (1<<2)  // enable mastering lan r/w
#define NVME_PCI_CMD_INT_DISABLE    (1<<10) // legacy interrupt disable when set
// PCI status register
#define NVME_PCI_STATUS_INT         (1<<3)
// NVME controller registers
#define NVME_CAP_OFFSET 0x0 // Capabilities
#define NVME_VS_OFFSET 0x8 // Version
#define NVME_CC_OFFSET 0x14 // Controller Configuration
#define NVME_CSTS_OFFSET 0x1c // Controller Status
#define NVME_AQA_OFFSET 0x24 // Admin Queue Attributesf
#define NVME_ASQ_OFFSET 0x28 // Admin Submission Queue Base Address
#define NVME_ACQ_OFFSET 0x30 // Admin Completion Queue Base Address
// NVME config values
#define NVME_VERSION 0x00010400 // version 1.4
#define NVME_MPS 4 // 4 bits wide
#define NVME_PAGE_SIZE 1 << (12 + NVME_MPS) // should be between 0x1000 and 0x10000
#define NVME_SQES 6 //  I/O Submission Queue Entry Size (specified as 2^n)
#define NVME_CQES 4 //  I/O Completion Queue Entry Size (specified as 2^n)
// Admin queue sizes are 0's based and units are number of entries
// Min number of entries is 2 and max number of entries is 4096
#define NVME_ACQS 63 // Admin Completion Queue Size 
#define NVME_ASQS 63 // Admin Submission Queue Size

/* NVMe types */


/* Submission queue entries */
struct nvme_command {
    uint8_t opc;		/* opcode */
    uint8_t flags;		/* fused operation */
    uint16_t cid;		/* command identifier */
    uint32_t nsid;		/* namespace identifier */
    uint32_t rsvd2;		/* reserved */
    uint32_t rsvd3;		/* reserved */
    uint64_t metadata;	/* metadata pointer */
    /* data pointer:
       some NVMe implementations also support scatter-gather lists 
       but for now we're just using physical page regions (prp)*/
    uint64_t prp1;		/* prp entry 1 */
    uint64_t prp2;		/* prp entry 2 */
    uint32_t cdw10;		/* command-specific */
    uint32_t cdw11;		/* command-specific */
    uint32_t cdw12;		/* command-specific */
    uint32_t cdw13;		/* command-specific */
    uint32_t cdw14;		/* command-specific */
    uint32_t cdw15;		/* command-specific */
}; // size:  64 bytes = 2^6

/* command opcodes*/
enum nvme_opcode {
    NVME_OPC_FLUSH  		= 0x00,
    NVME_OPC_WRITE  		= 0x01,
    NVME_OPC_READ   		= 0x02,
    NVME_OPC_WRITE_UNCOR    = 0x04,
    NVME_OPC_COMPARE    	= 0x05,
    NVME_OPC_WRITE_ZEROES  	= 0x08,
    NVME_OPC_DSM    		= 0x09,
    NVME_OPC_VERIFY 		= 0x0c,
    NVME_OPC_RESV_REGISTER  = 0x0d,
    NVME_OPC_RESV_REPORT    = 0x0e,
    NVME_OPC_RESV_ACQUIRE   = 0x11,
    NVME_OPC_RESV_RELEASE   = 0x15,
    NVME_OPC_ZONE_MGMT_SEND = 0x79,
    NVME_OPC_ZONE_MGMT_RECV = 0x7a,
    NVME_OPC_ZONE_APPEND    = 0x7d,
    NVME_OPC_VENDOR_START   = 0x80,
};

/* admin opcodes */
enum nvme_admin_opcode {
	NVME_OPC_DELETE_IO_SQ			= 0x00,
	NVME_OPC_CREATE_IO_SQ			= 0x01,
	NVME_OPC_GET_LOG_PAGE			= 0x02,
	NVME_OPC_DELETE_IO_CQ			= 0x04,
	NVME_OPC_CREATE_IO_CQ			= 0x05,
	NVME_OPC_IDENTIFY			    = 0x06,
	NVME_OPC_ABORT				    = 0x08,
	NVME_OPC_SET_FEATURES			= 0x09,
	NVME_OPC_GET_FEATURES			= 0x0a,
};

// for identify admin command, which subsystem to get info about
enum nvme_identify_cns{
    NAMESPACE       = 0x00,
    CONTROLLER      = 0x01,
    NAMESPACE_LIST  = 0x02,
};

/* completion queue entry */
struct nvme_completion {
    uint32_t cdw0;		/* command-specific */
    uint32_t rsvd1;		/* reserved */
    uint16_t sq_head;	/* submission queue head pointer */
    uint16_t sq_id;		/* submission queue identifier */
    uint16_t cid;		/* command identifier */
    uint16_t phase_tag:1; /* is this a new entry */
    uint16_t status:15; 	/* did the command fail, and if so, why? */
} __attribute__((packed)); // size: 16 bytes = 2^4

/* namespace */
// can add or remove fields if need be
struct nvme_namespace {
    uint32_t nsid;
    uint64_t nsze; // total size of namespace in logical blocks
    uint64_t ncap; // total number of logical blocks that can be allocated at once
    uint8_t lbads; // lba data size (2^n)
};

#endif
