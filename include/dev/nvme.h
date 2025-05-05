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

#ifndef __NVME_H__
#define __NVME_H__

/* Work in progress NVMe driver*/
int  nk_nvme_init(struct naut_info *naut);
void nk_nvme_deinit();

/* NVMe types*/

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
};

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

/* completion queue entry */
struct nvme_completion {
    uint32_t cdw0;		/* command-specific */
    uint32_t rsvd1;		/* reserved */
    uint16_t sq_head;	/* submission queue head pointer */
    uint16_t sq_id;		/* submission queue identifier */
    uint16_t cid;		/* command identifier */
    uint16_t status; 	/* did the command fail, and if so, why? */
};

#endif
