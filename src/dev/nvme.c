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
#include <nautilus/init.h>
#include <nautilus/blkdev.h>
#include <dev/pci.h>
#include <nautilus/mm.h>
#include <nautilus/interrupt.h>
#include <nautilus/cpu.h>
#include <nautilus/naut_string.h>
#include <dev/nvme.h>
#include <nautilus/shell.h>
#include <nautilus/dev.h>
#include <nautilus/endian.h>
#include <nautilus/semaphore.h>
#include <nautilus/spinlock.h>


#ifndef NAUT_CONFIG_DEBUG_NVME
#undef DEBUG_PRINT
#define DEBUG_PRINT(fmt, args...) 
#endif

#define ERROR(fmt, args...) ERROR_PRINT("nvme: " fmt, ##args)
#define DEBUG(fmt, args...) DEBUG_PRINT("nvme: " fmt, ##args)
#define INFO(fmt, args...) INFO_PRINT("nvme: " fmt, ##args)

#define READ_MEM(d, o)         (*((volatile uint32_t*)(((d)->mem_start)+(o))))
#define WRITE_MEM(d, o, v)     ((*((volatile uint32_t*)(((d)->mem_start)+(o))))=(v))

#define READ_MEM64(d, o)       (*((volatile uint64_t*)((d)->mem_start + (o))))
#define WRITE_MEM64(d, o, v)     ((*((volatile uint64_t*)(((d)->mem_start)+(o))))=(v))

/* Work in progress NVMe driver*/

// types
struct nvme_queue {
    struct nk_semaphore *slots_free_write;
    struct nk_semaphore *slots_free_read;
    spinlock_t lock;
    uint64_t addr;
    uint64_t size;
    uint16_t id;
    uint16_t tail;
    uint16_t head;
};

typedef struct nvme_sq {
    struct nvme_queue q;
    struct nvme_command* buffer;
} nvme_sq;

typedef struct nvme_cq {
    struct nvme_queue q;
    struct nvme_completion* buffer;
} nvme_cq;


struct nvme_dev {

    struct nk_dev *dev; // necessary that it's first field (allegedly)
    // pci interrupt and interupt vector
    struct pci_dev *pci_dev;
    struct nk_block_dev *blkdev;
    uint64_t block_size;
    uint64_t num_blocks;
    uint32_t dstrd;
    nvme_sq admin_sq;
    nvme_cq admin_cq;
    nvme_sq io_sq; // For now 1
    nvme_cq io_cq; // For now 1
    struct nvme_namespace *ns; // for now let's have a struct with a single namespace
    // Where registers are mapped into the physical memory address space
    uint64_t mem_start;
    uint64_t mem_end;
    uint32_t mpsmin;
    uint8_t mdts; // max data transfer size (in units of the minimum memory page size and reported as a power of two )
};


/* forward declarations */
int nvme_create_io_sq_cmd(struct nvme_dev *nvme, uint16_t sq_id, uint16_t cq_id, nvme_sq *io_sq, struct nvme_completion *comp);
int nvme_create_io_cq_cmd(struct nvme_dev *nvme, uint16_t cq_id, nvme_cq *io_cq, struct nvme_completion *comp);
static int check_block_count(struct nvme_dev *nvme, uint64_t lba, uint64_t num_blocks, uint32_t nsid);


/* helper functions */

/* concatenate first four bytes of an array in little endian order */
static inline volatile uint32_t read_arr_32le(volatile uint8_t * d, uint16_t o){
    uint32_t ret = 0;
    for (int i=0; i<4; i++){
        ret |= ((uint32_t)d[o+i]) << (8*i);
    }
    return ret;
}; 

/* concatenate first eight bytes of an array in little endian order */
static inline volatile uint64_t read_arr_64le(volatile uint8_t * d, uint16_t o){
    uint64_t ret = 0;
    for (int i=0; i<8; i++){
        ret |= ((uint64_t)d[o+i]) << (8*i);
    }
    return ret;
}; 

/* check controller fatal status bit of controller status register */
static inline int check_csts_fatal_status(struct nvme_dev *nvme){
    volatile uint32_t csts = READ_MEM(nvme, NVME_CSTS_OFFSET);
    if ((csts >>1) & 0x1) {
        ERROR("NVMe fatal controller error!!\n");
        return 1;
    }
    return 0;
}

/* Queue Management */

/* initialize admin completion queue by allocating space for buffer 
   and registering with controller */
int create_admin_completion_queue(struct nvme_dev *nvme) {
    nvme_cq *cq = &nvme->admin_cq;
    cq->q.size = NVME_ACQS;
    // initialize semaphores
    cq->q.slots_free_write = nk_semaphore_create(0, cq->q.size, 0, 0);
    nk_semaphore_init(cq->q.slots_free_write);
    cq->q.slots_free_read = nk_semaphore_create(0, 0, 0, 0);
    nk_semaphore_init(cq->q.slots_free_read);
    spinlock_init(&cq->q.lock);

    // allocate buffer, using NVME_PAGE_SIZE ensures alignment
    // technically queue could be larger then page_size, but ours isn't
    cq->buffer = (struct nvme_completion*)malloc(NVME_PAGE_SIZE);
    if (cq->buffer==NULL){
        ERROR("Couldn't malloc admin cq buffer\n");
        return -1;
    }
    DEBUG("created admin cq ring buffer\n");
    cq->q.addr = cq->buffer;
    // The Phase Tag values for all Completion Queue entries shall be initialized to 0
    memset(nvme->admin_cq.q.addr, 0, (cq->q.size) * sizeof(struct nvme_completion));
    // Bottom 12 bits of address MUST be 0!
    if (nvme->admin_cq.q.addr & 0xfff) {
        ERROR("Admin completion queue address is not 12-bit aligned!\n");
        // insufficient alignment will actually cause hardware issues
        // this is why we malloc a full NVME_PAGE_SIZE
        return -1;
    }

    // set remaining fields
    cq->q.id = 0; // admin queues must have id 0
    cq->q.tail = 0;
    cq->q.head = 0;
    DEBUG("admin_cq addr: 0x%lx\n", nvme->admin_cq.q.addr);

    // inform controller about queue
    // write address to Admin Completion Queue Base Address (AQA) register
	WRITE_MEM(nvme, NVME_ACQ_OFFSET, nvme->admin_cq.q.addr);
    // write admin completion queue size to Admin Queue Attributes (AQA) register
    volatile uint32_t aqa = READ_MEM(nvme, NVME_AQA_OFFSET);
    aqa |= (NVME_ACQS - 1) << 16;
    WRITE_MEM(nvme, NVME_AQA_OFFSET, aqa);
	return 0;
}

/* initialize admin submission queue by allocating space for buffer 
   and registering with controller */
int create_admin_submission_queue(struct nvme_dev *nvme) {
    nvme_sq* sq = &nvme->admin_sq;
    sq->q.size = NVME_ASQS;
    // initialize semaphores
    sq->q.slots_free_write = nk_semaphore_create(0, sq->q.size, 0, 0);
    nk_semaphore_init(sq->q.slots_free_write);
    sq->q.slots_free_read = nk_semaphore_create(0, 0, 0, 0);
    nk_semaphore_init(sq->q.slots_free_read);
    spinlock_init(&sq->q.lock);

    // allocate buffer, using NVME_PAGE_SIZE ensures alignment
    // technically queue could be larger then page_size, but ours isn't
    sq->buffer = (struct nvme_command*)malloc(NVME_PAGE_SIZE);
    if (sq->buffer == NULL) {
        ERROR("Couldn't malloc admin sq buffer\n");
        return -1;
    }
    DEBUG("created admin sq ring buffer\n");
    sq->q.addr = sq->buffer;
    DEBUG("admin_sq addr: 0x%lx\n", nvme->admin_sq.q.addr);
    // Bottom 12 bits of address MUST be 0!
    if (nvme->admin_sq.q.addr & 0xfff) {
        ERROR("Admin submission queue address is not 12-bit aligned!\n");
        // insufficient alignment will actually cause hardware issues
        // this is why we malloc a full NVME_PAGE_SIZE
        return -1;
    }

    // set remaining fields
    sq->q.id = 0; // admin queues must have id 0
    sq->q.tail = 0;
    sq->q.head = 0;

    // inform controller about queue
    // write address to Admin Submission Queue Base Address (AQA) register
	WRITE_MEM(nvme, NVME_ASQ_OFFSET, nvme->admin_sq.q.addr);
    // write admin submission queue size to Admin Queue Attributes (AQA) register
    volatile uint32_t aqa = READ_MEM(nvme, NVME_AQA_OFFSET);
    aqa |= NVME_ASQS - 1; // hw uses a zero-based value, so subtract 1
    WRITE_MEM(nvme, NVME_AQA_OFFSET, aqa);
	return 0;
}

/* initialize I/O queue by allocating space for buffer 
   and issuing Create I/O Completion Queue command to the controller */
int create_io_completion_queue(struct nvme_dev *nvme, nvme_cq *cq) {
	cq->q.size = NVME_IOCQS;
    // initialize semaphores
    cq->q.slots_free_write = nk_semaphore_create(0, cq->q.size, 0, 0);
    nk_semaphore_init(cq->q.slots_free_write);
    cq->q.slots_free_read = nk_semaphore_create(0, 0, 0, 0);
    nk_semaphore_init(cq->q.slots_free_read);
    spinlock_init(&cq->q.lock);

    // allocate buffer, using NVME_PAGE_SIZE ensures alignment
    // technically queue could be larger then page_size, but ours isn't
    cq->buffer = (struct nvme_completion*)malloc(NVME_PAGE_SIZE);
    if (cq->buffer==NULL){
        ERROR("Couldn't malloc admin cq buffer\n");
        return -1;
    }
    cq->q.addr = cq->buffer;

    // The address pointer is memory page aligned (based on the value in CC.MPS) unless otherwise specified
    DEBUG("io_cq addr: 0x%lx\n", cq->q.addr);
    if (cq->q.addr & 0xfff) {
        ERROR("IO completion queue address is not 12-bit aligned!\n");
        return -1;
    }
    // The Phase Tag values for all Completion Queue entries must be initialized to 0
    memset(cq->q.addr, 0, (cq->q.size) * sizeof(struct nvme_completion));

    // set remaining fields
    // io queues will arbitrarily have id=1 (can't be 0 because that is reserved for admin queues)
    uint16_t cq_id = 1;
    cq->q.id = cq_id;
    cq->q.tail = 0;
    cq->q.head = 0;

    // send Admin command to register the queue with the controller
    struct nvme_completion comp;
	return nvme_create_io_cq_cmd(nvme, cq_id, cq, &comp);
}

int create_io_submission_queue(struct nvme_dev *nvme, nvme_sq *sq) {
	sq->q.size = NVME_IOSQS;
    // initialize semaphores
    sq->q.slots_free_write = nk_semaphore_create(0, sq->q.size, 0, 0);
    nk_semaphore_init(sq->q.slots_free_write);
    sq->q.slots_free_read = nk_semaphore_create(0, 0, 0, 0);
    nk_semaphore_init(sq->q.slots_free_read);
    spinlock_init(&sq->q.lock);

    // allocate buffer, using NVME_PAGE_SIZE ensures alignment
    // technically queue could be larger then page_size, but ours isn't
    sq->buffer = (struct nvme_command*)malloc(NVME_PAGE_SIZE);
    if (sq->buffer==NULL){
        ERROR("Couldn't malloc io sq buffer\n");
        return -1;
    }
    sq->q.addr = sq->buffer;

    // The address pointer is memory page aligned (based on the value in CC.MPS) unless otherwise specified
    DEBUG("io_sq addr: 0x%lx\n", sq->q.addr);
    if (sq->q.addr & 0xfff) {
        ERROR("IO submission queue address is not 12-bit aligned!\n");
        return -1;
    }

    // set remaining fields
    // io queues will arbitrarily have id=1 (can't be 0 because that is reserved for admin queues)
    uint16_t sq_id = 1;
    uint16_t cq_id = 1; // this belongs to the corresponding completion queue
    sq->q.id = sq_id;
    sq->q.tail = 0;
    sq->q.head = 0;

    // send Admin command to register the queue with the controller
    struct nvme_completion comp;
	return nvme_create_io_sq_cmd(nvme, sq_id, cq_id, sq, &comp);
}


/* Command submisison helper functions */

/* Actually submit command to controller and poll for completion */
static int nvme_queue_submit_cmd(struct nvme_dev *nvme, nvme_sq *sq, struct nvme_cq *cq, struct nvme_command *cmd, struct nvme_completion *comp){
    // physical memory addresses in queues to access
    volatile struct nvme_command *sq_entry = sq->q.addr + (sq->q.tail * sizeof(struct nvme_command));
    volatile struct nvme_completion *cq_entry = cq->q.addr + (cq->q.head * sizeof(struct nvme_completion));  

    uint16_t sq_tail_doorbell = 0x1000 + ((2*sq->q.id) * (4 << nvme->dstrd));    
    // enqueue command in submission queue
    nk_semaphore_down(sq->q.slots_free_write);
    spin_lock(&sq->q.lock);
    sq->q.tail++;
    // wrap if tail reaches end of buffer
	if (sq->q.tail == (sq->q.size)){ 
        DEBUG("wrapping sq\n");
        sq->q.tail = 0;
    }
    // store previous phase tag bit value *before* writing command to queue
    volatile uint8_t prev_phase_tag = cq_entry->phase_tag;
    // write command to submission queue
    memcpy((void*)sq_entry, (void*)cmd, sizeof(struct nvme_command));
    // ring submission queue doorbell by writing address to register
        // this informs the controller that we submitted a command
    // tail should be the index of the next *free* slot, not the one we just wrote to
    WRITE_MEM(nvme, sq_tail_doorbell, sq->q.tail);
    spin_unlock(&sq->q.lock);
    nk_semaphore_up(sq->q.slots_free_read);

    nk_semaphore_down(cq->q.slots_free_write);
    spin_lock(&cq->q.lock);
    // poll
    // hardware will invert phase tag bit upon every write to cq
    // we can use this to tell if the controller has written a new completion entry
    while(cq_entry->phase_tag == prev_phase_tag){
        io_delay();
    };
    spin_unlock(&cq->q.lock);
    nk_semaphore_up(cq->q.slots_free_read);

    nk_semaphore_down(sq->q.slots_free_read);
    spin_lock(&sq->q.lock);
    // controller tells us where the new sq head is depending on how many entries it has consumed
    sq->q.head = cq_entry->sq_head;
    spin_unlock(&sq->q.lock);
    nk_semaphore_up(sq->q.slots_free_write);
 
    uint16_t cq_head_doorbell = 0x1000 + (((2*cq->q.id) + 1) * (4 << nvme->dstrd));
    // dequeue completion from completion queue
    nk_semaphore_down(cq->q.slots_free_read);
    spin_lock(&cq->q.lock);
    cq->q.head++;
    // wrap if tail reaches end of buffer
	if (cq->q.head == (cq->q.size)){
        DEBUG("wrapping cq\n");
        cq->q.head = 0;
    }
    spin_unlock(&cq->q.lock);
    nk_semaphore_up(cq->q.slots_free_write);

    // NOTE: *don't* overwrite consumed sq or cq entry or else undefined hardware behavior
        // we also need to preserve phase tag bit
    // copy completion entry to comp
    memcpy((void*)comp, (void*)cq_entry, sizeof(struct nvme_completion));
    // ring completion queue doorbell by writing address to register
        // this informs the controller that we processed a completion
    WRITE_MEM(nvme, cq_head_doorbell, cq->q.head);
    // DEBUG("sq->tail: %d, sq->head: %d, cq->head: %d\n", sq->q.tail, sq->q.head, cq->q.head);
    DEBUG("command completed with status: 0x%08x\n", comp->status);
    return comp->status;
}

/* wrapper for nvme_queue_submit_cmd that uses Admin queues */
static int nvme_submit_admin_cmd(struct nvme_dev *nvme, struct nvme_command *cmd, struct nvme_completion *comp)
{
    return nvme_queue_submit_cmd(nvme, &(nvme->admin_sq), &(nvme->admin_cq), cmd, comp);
}

/* wrapper for nvme_queue_submit_cmd that uses I/O queues */
static int nvme_submit_io_cmd(struct nvme_dev *nvme, struct nvme_command *cmd, struct nvme_completion *comp)
{
    return nvme_queue_submit_cmd(nvme, &(nvme->io_sq), &(nvme->io_cq), cmd, comp);
}

/* IO Commands */

/* handles Read and Write commands */
static int nvme_rw_cmd(struct nvme_dev *nvme, uint8_t opc, uint32_t nsid, 
                void *buff, uint64_t lba, uint32_t num_blocks, struct nvme_completion *comp)
{
    // if ((uintptr_t)buff & (NVME_PAGE_SIZE - 1)) {
    //     ERROR("Buffer address is not aligned to page size!\n");
    //     return -1;
    // }
    if (check_block_count(nvme, lba, num_blocks, nsid)){
        return -1;
    }

    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = opc;
	cmd.nsid = htole32(nsid);

    uint64_t num_extra_pages = num_blocks*(nvme->block_size) / NVME_PAGE_SIZE; // truncates
    // PRP (physical page region) entry = pointr to physical memory page
    // page size configured by CC.MPS
    cmd.prp1 = htole32((uintptr_t)buff); // prp1 is always the value of the first PRP
    // if data transfer fits in 1 page, leave prp2=0
    if (num_extra_pages == 1) {
        // if data transfer fits in 2 pages, prp2 is the address of the second page
        cmd.prp2 = htole32((uintptr_t)(buff + NVME_PAGE_SIZE));
    } else if (num_extra_pages > 1) {
        // allocate a PRP list if more than 2 pages are needed
        uint64_t * prp_list = (uint64_t *)malloc(NVME_PAGE_SIZE); // ensure alignment by mallocing a page
        if (prp_list == NULL) {
            ERROR("Couldn't malloc PRP list\n");
            return -1;
        }
        DEBUG("building PRP list, assuming contiguous buffer provided\n");
        // if more than 2 pages are needed, prp2 is pointer to PRP list
        cmd.prp2 = htole32((uintptr_t)prp_list);
        // build out PRP list from the provided contiguous buff
        for (uint64_t i=1; i<=num_extra_pages; i++){
            // address of the first page is specified in prp1, not the prp list
            prp_list[i-1] = buff + i*NVME_PAGE_SIZE;
        }
    }

    cmd.cdw10 = htole32(lba & 0xffffffffu);
	cmd.cdw11 = htole32(lba >> 32);
	cmd.cdw12 = htole32(num_blocks-1); // zero-based

    int status = nvme_submit_io_cmd(nvme, &cmd, comp);

    if (num_extra_pages > 1){
        free((void*)cmd.prp2);
    }
    return status;
}

/* wrapper that calls nvme_rw_cmd with the Write opcode */
int nvme_write_cmd(struct nvme_dev *nvme, uint32_t nsid, void *buff,
    uint64_t lba, uint32_t num_blocks, struct nvme_completion *comp)
{
	return nvme_rw_cmd(nvme, NVME_OPC_WRITE, nsid, buff, lba, num_blocks, comp);
}

/* wrapper that calls nvme_rw_cmd with the Read opcode */
int nvme_read_cmd(struct nvme_dev *nvme, uint32_t nsid, void *buff, 
    uint64_t lba, uint32_t num_blocks, struct nvme_completion *comp)
{
	return nvme_rw_cmd(nvme, NVME_OPC_READ, nsid, buff, lba, num_blocks, comp);
}

/* Admin Commands */

/* Identify command */
int nvme_identify_controller_or_ns_list_cmd(struct nvme_dev *nvme, uint16_t subsys, void *buff, struct nvme_completion *comp){
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = NVME_OPC_IDENTIFY;
	cmd.prp1 = (uintptr_t)buff; // command output (a single page)
    cmd.cdw10 = htole32(subsys); // Controller or Namespace or Namespace list

    return nvme_submit_admin_cmd(nvme, &cmd, comp);
}

/* wrapper that sends the identify command with Controller as the CNS (Controller or Namespace) value */
int nvme_identify_controller_cmd(struct nvme_dev *nvme, void *buff, struct nvme_completion *comp){
    // a 4096 byte array of information is written to buff, see NVMe spec for definition
    return nvme_identify_controller_or_ns_list_cmd(nvme, CONTROLLER, buff, comp);
}

/* wrapper that sends the identify command with Controller as the CNS (Controller or Namespace) value */
int nvme_identify_ns_list_cmd(struct nvme_dev *nvme, void *buff, struct nvme_completion *comp){
    // A list of 1,024 namespace IDs is returned to the host containing active NSIDs in increasing order 
    return nvme_identify_controller_or_ns_list_cmd(nvme, NAMESPACE_LIST, buff, comp);
}

/* Identify command for a specific Namespace*/
int nvme_identify_ns_cmd(struct nvme_dev *nvme, void *buff, uint32_t nsid, struct nvme_completion *comp){
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = NVME_OPC_IDENTIFY;
	cmd.nsid = htole32(nsid);
	cmd.prp1 = (uintptr_t)buff; // command output (a single page)
    cmd.cdw10 = htole32(NAMESPACE);

    // a 4096 byte array of information is written to buff, see NVMe spec for definition
    return nvme_submit_admin_cmd(nvme, &cmd, comp);
}

/* Create I/O submission queue command */
int nvme_create_io_sq_cmd(struct nvme_dev *nvme, uint16_t sq_id, uint16_t cq_id, nvme_sq *io_sq, struct nvme_completion *comp){
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = NVME_OPC_CREATE_IO_SQ;
	cmd.prp1 = htole64(io_sq->q.addr);
	cmd.cdw10 = htole32(((io_sq->q.size - 1) << 16) | sq_id); // queue size is zero-based so subtract 1
	cmd.cdw11 = htole32((cq_id << 16) | 0x01);
    
    return nvme_submit_admin_cmd(nvme, &cmd, comp); 
}

/* Create I/O completion queue command */
int nvme_create_io_cq_cmd(struct nvme_dev *nvme, uint16_t cq_id, nvme_cq *io_cq, struct nvme_completion *comp){
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = NVME_OPC_CREATE_IO_CQ;
	cmd.prp1 = htole64(io_cq->q.addr);
	cmd.cdw10 = htole32(((io_cq->q.size - 1) << 16) | cq_id); // queue size is zero-based so subtract 1
    // if we were using interrupts, specify MSI vector + 1 in highword of DWORD11
	/* flags 0x1 = interrupts not enabled, physically contiguous */
	cmd.cdw11 = htole32(0x01);
    
    return nvme_submit_admin_cmd(nvme, &cmd, comp); 
}

/* Set Features command */
int nvme_set_features_cmd(struct nvme_dev *nvme, uint8_t feature,
    // we use it to set the number of I/O queues
    uint32_t cdw11,  uint32_t cdw12, uint32_t cdw13, uint32_t cdw14,
    uint32_t cdw15, struct nvme_completion *comp){
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = NVME_OPC_SET_FEATURES;
	cmd.cdw10 = htole32(feature);
	cmd.cdw11 = htole32(cdw11);
	cmd.cdw12 = htole32(cdw12);
	cmd.cdw13 = htole32(cdw13);
	cmd.cdw14 = htole32(cdw14);
	cmd.cdw15 = htole32(cdw15);

    return nvme_submit_admin_cmd(nvme, &cmd, comp); 
}

/* Inform controller about the number of I/O queues using the Set Features command*/
int nvme_set_num_io_queues(struct nvme_dev *nvme, uint16_t num_sq, uint16_t num_cq, struct nvme_completion *comp){
	uint32_t cdw11;

	cdw11 = ((num_cq - 1) << 16) | (num_sq - 1); // number of queues is zero-based
	return nvme_set_features_cmd(nvme, NVME_FEAT_NUMBER_OF_QUEUES, cdw11,
	    0, 0, 0, 0, comp);
}

/* Verify valid block range for namespace */
static int check_block_count(struct nvme_dev *nvme, uint64_t lba, uint64_t num_blocks, uint32_t nsid) {
    // currently, we only know about a single namespace
    if (nsid != nvme->ns->nsid) {
        ERROR("Unknown nvme namespace!\n");
        return -1;
    }
    // check if lba and num_blocks are within bounds for a given namespace
    uint64_t ns_block_size = 1 << nvme->ns->lbads;
    if ( num_blocks == 0 || (lba + num_blocks) > nvme->ns->nsze) {
        ERROR("Block number or num_blocks out of bounds! lba: %lu, num_blocks: %lu, max num_blocks: %lu\n", lba, num_blocks, nvme->ns->nsze);
        return -1;
    }
    uint64_t num_extra_pages = num_blocks * ns_block_size / NVME_PAGE_SIZE; // truncates, first page is in prp1
    uint64_t prp_list_size = num_extra_pages * sizeof(uint64_t); // each PRP list enrty is a 64-bit physical address
    if (prp_list_size > NVME_PAGE_SIZE) {
        // it's possible to set the last entry of the PRP list to a pointer to another PRP list, enabling more than 1 page of PRP entries
        // we don't implement that, but it could be added in the future
        ERROR("PRP list size exceeds page size!\n");
        return -1;
    }

    // check if the requested data transfer size exceeds the maximum data transfer size
    if (num_blocks * ns_block_size > (1<<nvme->mdts)*nvme->mpsmin) {
        ERROR("Maximum data transfer size exceeded! requested %lu, max allowed: %lu\n", num_blocks * ns_block_size, nvme->mdts);
        return -1;
    }

    // maybe need to check alignment?
    return 0;
}

/* Block Device Interface */

/* wrapper for read command that calls callback if provided 
   conforms to nk blkdev interface */
static int read_blocks(void *state, uint64_t blocknum, uint64_t count, uint8_t *dest, void (*callback)(nk_block_dev_status_t, void *), void *context)
{
    struct nvme_dev *s = (struct nvme_dev *)state;
    struct nvme_completion comp;
    int nvme_status = nvme_read_cmd(s, s->ns->nsid, dest, blocknum, count, &comp);
    nk_block_dev_status_t blk_dev_status;

    if (nvme_status){
        ERROR("NVMe read command failed! with code 0x%08x\n", nvme_status);
        blk_dev_status = NK_BLOCK_DEV_STATUS_ERROR;
    } else {
        blk_dev_status = NK_BLOCK_DEV_STATUS_SUCCESS;
        DEBUG("NVMe read command succeeded!\n");
    }

    if (callback) {
        callback(blk_dev_status, context);
    }

    return nvme_status;
}

/* wrapper for read command that calls callback if provided 
   conforms to nk blkdev interface */
static int write_blocks(void *state, uint64_t blocknum, uint64_t count, uint8_t *src, void (*callback)(nk_block_dev_status_t, void *), void *context)
{
    struct nvme_dev *s = (struct nvme_dev *)state;
    struct nvme_completion comp;
    int nvme_status = nvme_write_cmd(s, s->ns->nsid, src, blocknum, count, &comp);
    nk_block_dev_status_t blk_dev_status;

    if (nvme_status){
        ERROR("NVMe read command failed! with code 0x%08x\n", nvme_status);
        blk_dev_status = NK_BLOCK_DEV_STATUS_ERROR;
    } else {
        blk_dev_status = NK_BLOCK_DEV_STATUS_SUCCESS;
        DEBUG("NVMe write command succeeded!\n");
    }

    if (callback) {
        callback(blk_dev_status, context);
    }

    return nvme_status;
}

/* conforms to nk blkdev interface */
static int get_characteristics(void *state, struct nk_block_dev_characteristics *c)
{
    struct nvme_dev *s = (struct nvme_dev *)state;

    c->block_size = s->block_size;
    c->num_blocks = s->num_blocks;

    return 0;
}

/* nk block device interface */
static struct nk_block_dev_int inter = 
{
    .get_characteristics = get_characteristics,
    .read_blocks = read_blocks,
    .write_blocks = write_blocks,
};


/* Check capabilities register for version, I/O command support, supported page size, and record dstrd */
int nvme_init_check_cap(struct nvme_dev *state) {
    // Check the controller version is supported.
    volatile uint32_t version = READ_MEM(state, NVME_VS_OFFSET);
    if (version != NVME_VERSION){
        ERROR("Unsupported NVME version 0x%08x\n", version);
        return -1;
    }
    DEBUG("Supported NVME version 0x%08x found\n", version);

    volatile uint64_t cap = READ_MEM64(state, NVME_CAP_OFFSET);
    // Check the capabilities register for support of the I/O NVMe command set.
    if (!((cap >> 37) & 0x1)) {
        ERROR("NVMe controller does not support I/O command set\n");
        return -1;
    }
    DEBUG("NVMe controller supports I/O\n");

    // Check the capabilities register for support of the host's page size.
    // Memory page size max and min
    uint32_t mpsmin = 1 << (12 + ((cap>>48) & 0xf));
    uint32_t mpsmax = 1 << (12 + ((cap>>52) & 0xf));
    DEBUG("mpsmin=0x%08x; mpsmax=0x%08x\n", (cap>>48) & 0xf, (cap>>52) & 0xf);
    if (NVME_PAGE_SIZE < mpsmin || NVME_PAGE_SIZE > mpsmax){
        ERROR("Unsupported nvme page size. min=0x%08x; max=0x%08x; actual=0x%08x\n", mpsmin, mpsmax, NVME_PAGE_SIZE);
        return -1;
    }
    DEBUG("Supported nvme page size. min=0x%08x; max=0x%08x; actual=0x%08x=%lu\n", mpsmin, mpsmax, NVME_PAGE_SIZE, NVME_PAGE_SIZE);
    state->mpsmin = mpsmin; // need to save for use in max data transfer size calculation
    state->dstrd = (cap>>32) & 0xf;
    DEBUG("dstrd=0x%08x\n", state->dstrd);
    return 0;
}

/* reset controller, create admin queus, set config register (cc), and start controller */
int nvme_init_controller_init (struct nvme_dev *state) {
        // Reset the controller 
        // triggered by a falling edge on the Enable bit of the controller configuration (CC) register
        volatile uint32_t cc = READ_MEM(state, NVME_CC_OFFSET);
        cc |= 1;
        WRITE_MEM(state, NVME_CC_OFFSET, cc);
        io_delay();
        cc &= 0xfffffffe; // set last bit to 0
        WRITE_MEM(state, NVME_CC_OFFSET, 0);
        // Wait for controller to indicate reset is complete (bit 0 of CSCT.RDY == 0)
        while (READ_MEM(state, NVME_CSTS_OFFSET) & 0x1){
            io_delay();
        }
        // check the status register
        if (check_csts_fatal_status(state)) {
            return -1;
        }

        // Create admin queues
        // can only modify admin queue registers when controller is disabled (CC.EN=0)
        // completion queues shall be created before submission queues
        if (create_admin_completion_queue(state) ||
            create_admin_submission_queue(state)) 
        {
            ERROR("Failure to create admin queues\n");
            return -1;
        }
        DEBUG("created admin queues\n");
        // Set the controller configuration
        cc = READ_MEM(state, NVME_CC_OFFSET);
        // We'll leave arbitration mechanism (AMS) as default (round robin)
            // But that's irrelevant while we only have a single I/O submission queue
        // command set selected (CSS) should support I/O by default
        // Set max page size (MPS)
        
        cc |= NVME_MPS << 7; // memory page size is (2 ^ (12 + MPS))

        // Start the controller by setting the enable bit
        cc |= 1;
        WRITE_MEM(state, NVME_CC_OFFSET, cc);
        cc = READ_MEM(state, NVME_CC_OFFSET);

        // check the status register again (not strictly necessary)
        if (check_csts_fatal_status(state)){
            return -1;
        }
        // Wait for controller to indicate it's ready to accept commands (bit 0 of CSCT.RDY == 1)
        while (!(READ_MEM(state, NVME_CSTS_OFFSET) & 0x1)){
            io_delay();
        }
        DEBUG("controller is ready\n");
        return 0;
}

/* initialize first I/O submission and completion queues*/
int nvme_init_create_and_configure_io_queues (struct nvme_dev *state) {
        struct nvme_completion comp;
        // Set number of I/O queues using Set Feature command
        // for now, we'll just have 1 sq and 1 cq
        uint16_t num_sq = 1;
        uint16_t num_cq = 1;
        if (nvme_set_num_io_queues(state, num_sq, num_cq, &comp)){
            ERROR("failed to set num IO queues!\n");
            return -1;
        }
        DEBUG("Set num I/O queues\n");

        // Set I/O Completion/Submission Queue Entry Size in controller configuration register
        // This must be done *before* actually creating I/O queues
        volatile uint32_t cc = READ_MEM(state, NVME_CC_OFFSET);
        cc |= (NVME_CQES << 20);
        cc |= (NVME_SQES << 16);
        WRITE_MEM(state, NVME_CC_OFFSET, cc);
        // check the status register again (not strictly necessary)
        if (check_csts_fatal_status(state)){
            return -1;
        }
        // Create the first IO completion queue, and the first IO submission queue.
        nvme_cq *cq = (nvme_cq*)malloc(sizeof(nvme_cq));
        if (cq == NULL){
            ERROR("Couldn't malloc io cq\n");
            return -1;
        }
        state->io_cq = *cq;
        nvme_sq *sq = (nvme_sq*)malloc(sizeof(nvme_sq));
        if (sq == NULL){
            ERROR("Couldn't malloc io sq\n");
            return -1;
        }
        state->io_sq = *sq;
        // completion queues shall be created before submission queues
        if (create_io_completion_queue(state, &(state->io_cq)) ||
            create_io_submission_queue(state, &(state->io_sq))) 
        {
            ERROR("Failure to create io queues\n");
            return -1;
        }
        // purely out of paranoia, let's check the status register again
        if (check_csts_fatal_status(state)){
            return -1;
        }
        DEBUG("created io queues\n");
        return 0;
}

/* check controller capabilities are compatable and record max data transfer size */
int nvme_init_identify_controller(struct nvme_dev *state) {
    // Send the identify command to the controller. 
    uint8_t id_data[4096]; // data structure returned by identify is 4096 bytes
    memset(id_data, 0, sizeof(id_data));
    struct nvme_completion comp;
    if (nvme_identify_controller_cmd(state, id_data, &comp)){
        ERROR("Identify controller command failed\n");
        return -1;
    };
    // Check it is an I/O controller
    uint8_t ctrlr_type = id_data[111];
    if (ctrlr_type != 0x1){
        ERROR("Unsupported NVME controller type %u returned by Identify\n", ctrlr_type);
        return -1;
    }
    // Record the maximum data transfer size
    uint8_t mdts = id_data[77];
    DEBUG("max data transfer size=0x%08x\n", (1<<mdts)*state->mpsmin); // 0 indicates no max
    state->mdts = mdts;

    // Check allowable I/O Completion/Submission Queue Entry Size
    uint8_t sqes = id_data[512];
    uint16_t min_sqes = 1 << (sqes & 0xf); // should be 6
    uint16_t max_sqes = 1 << (sqes >> 4);
    uint16_t actual_sqes = 1 << NVME_SQES;
    if (actual_sqes < min_sqes || actual_sqes > max_sqes){
        ERROR("Unsupported nvme submission queue entry size. min=0x%08x; max=0x%08x; actual=0x%08x\n",  min_sqes, max_sqes, actual_sqes);
        return -1;
    }
    uint8_t cqes = id_data[513];
    uint16_t min_cqes = 1 << (cqes & 0xf);
    uint16_t max_cqes = 1 << (cqes >> 4); // should be 4
    uint16_t actual_cqes = 1 << NVME_CQES;
    if (actual_cqes < min_cqes || actual_cqes > max_cqes){
        ERROR("Unsupported nvme completion queue entry size. min=0x%08x; max=0x%08x; actual=0x%08x\n", min_cqes, max_cqes, actual_cqes);
        return -1;
    }
    DEBUG("Supported submission and completion queues entry sizes\n");
    return 0;
}

// identify namespaces, get block size and number of blocks
int nvme_init_identify_ns(struct nvme_dev *state) {
    uint8_t id_data[4096]; // data structure returned by identify is 4096 bytes
    memset(id_data, 0, sizeof(id_data));
    struct nvme_completion comp;
    if (nvme_identify_ns_list_cmd(state, id_data, &comp)){
        ERROR("Identify namespace command failed\n");
        return -1;
    }
    // we could loop over this list of nsid's until we find a zero id (which indicates no more namespaces)
    // however, qemu must be configured to add additional namespaces
    // qemu defaults to a single namespace with nsid=1
    // for now, let's just store info about the first one, but for good measure still send the identify namespace list command and get the nsid that way
    uint32_t nsid0 = (id_data[2] << 16) | (id_data[1] << 8) | id_data[0];
    DEBUG("Identify Namespace List returned nsid0=0x%06x\n", nsid0);
    if (!nsid0){
        ERROR("Namespace ID list is empty\n");
        return -1;
    }
    // identify the first namespace
    memset(id_data, 0, sizeof(id_data));
    if (nvme_identify_ns_cmd(state, id_data, nsid0, &comp)){
        ERROR("Identify namespace command failed\n");
        return -1;
    }    
    
    // Record namespace block size and capacity
    struct nvme_namespace *ns0 = (struct nvme_namespace *)malloc(sizeof(struct nvme_namespace));
    if (!ns0) {
        ERROR("Cannot allocate NVMe namespace\n");
        return -1;
    }
    memset(ns0, 0, sizeof(*ns0));
    state->ns = ns0; // namespace
    ns0->nsid = nsid0; // this should be 1 (for qemu's first ns)
    ns0->nsze = read_arr_64le(id_data, 0); // num_blocks
    state->num_blocks = ns0->nsze;
    ns0->ncap = read_arr_64le(id_data, 8);
    DEBUG("Namespace nsid=%u, nsze=%lu, ncap=%lu\n", ns0->nsid, ns0->nsze, ns0->ncap);
    // Get logical block address (LBA) format and size
    // LBA format. It's possible to have multiple, let's only care about the first one for now
    // formatted LBA size: Index of the supported LBA format, not the actual size
    uint8_t flbas = id_data[26] & 0xf; 
    // LBA format
    uint32_t lbaf = read_arr_32le(id_data, 128 + flbas);
    // qemu defaults to 0 metadata bytes per LBA and no extended LBA. let's keep it that way for now
    ns0->lbads = (lbaf>>16) & 0xf;
    // actual lba data size = (2^lbads);
    state->block_size = 1 << (ns0->lbads);
    DEBUG("flbas=%u, lbaf=0x%08x, lbads=%u, block_size=%lu\n",
        flbas, lbaf, ns0->lbads, state->block_size);
    return 0;
}

/* all the nvme initialization steps that are independent of nautilus or pci */
int nvme_init_nvme_setup(struct nvme_dev *state) {
    // NVME specific setup
    if (nvme_init_controller_init(state)) {
        ERROR("Failed to initialize NVMe controller\n");
        return -1;
    }

    // send identify controller command and parse returned info
    if (nvme_init_identify_controller(state)){
        ERROR("Failed to identify controller\n");
        return -1;
    }
    
    // Identify active namespace IDs, and then identify individual namespaces. 
    if (nvme_init_identify_ns(state)){
        ERROR("Failed to identify namespaces\n");
        return -1;
    }

    // allocate io queues, register with controller, and set sizes
    if (nvme_init_create_and_configure_io_queues(state)) {
        ERROR("Failed to create and configure I/O queues\n");
        return -1;
    };

    // If we want interrupts, enable them and register a handler (skipping for now)
    // Also do stuff with MSI

    return 0;
}

/* Nautilus block device registration */
int nk_nvme_init_register_blkdev(struct nvme_dev *state, uint16_t num) {

    char blkdev_name[32];
    sprintf(blkdev_name, "nvme-%d", num);
    state->blkdev = nk_block_dev_register(blkdev_name, 0, &inter, state);
    if (!state->blkdev) {
        ERROR("Failed to register %s\n",blkdev_name);
    }
    INFO("Added nvme device %s, blocksize=%lu, numblocks=%lu\n",
        blkdev_name, state->block_size,state->num_blocks );
    return 0;
}


// PCI initialization mostly copied from e1000e_pci_init
int nk_nvme_init(struct naut_info *naut)
{
    struct pci_info *pci = nk_get_nautilus_info()->sys.pci;
    struct list_head *curbus, *curdev;
    uint16_t num = 0;
    
    INFO("init\n");
    
    if (!pci) {
        ERROR("No PCI info\n");
        return -1;
    }

    struct nvme_dev *state = (struct nvme_dev *)malloc(sizeof(struct nvme_dev));
    if (!state) {
        ERROR("Cannot allocate NVMe device state\n");
        return -1;
    }
    memset(state, 0, sizeof(*state));

    DEBUG("Finding NVMe devices\n");

    list_for_each(curbus,&(pci->bus_list)) {
        struct pci_bus *bus = list_entry(curbus,struct pci_bus,bus_node);
    
        DEBUG("Searching PCI bus %u for NVMe devices\n", bus->num);
    
        list_for_each(curdev, &(bus->dev_list)) {
            struct pci_dev *pdev = list_entry(curdev,struct pci_dev,dev_node);
            struct pci_cfg_space *cfg = &pdev->cfg;
        
            DEBUG("Device %u is a 0x%x:0x%x\n", pdev->num, cfg->vendor_id, cfg->device_id);
            // intel vendor id and e1000e device id
            if (cfg->vendor_id==NVME_VENDOR_ID && cfg->device_id==NVME_DEVICE_ID) {
                int foundio=0, foundmem=0;
            
                DEBUG("Found NVMe Device\n");

                // We will *not* support interrupts for now
        
                // find out the bar for NVMe
                // only care about bar0
                uint32_t bar = pci_cfg_readl(pci,bus->num, pdev->num, 0, 0x10);
                uint32_t size;
                DEBUG("bar 0: 0x%0x\n", bar);

                // get the last bit and if it is zero, it is the memory
                // " -------------------------"	one, it is the io
                if (!(bar & 0x1)) {
                    uint8_t mem_bar_type = (bar & 0x6) >> 1;
                    if (mem_bar_type != 2) { 
                        // 64 bit address that we do not handle it
                        ERROR("Cannot handle memory bar type 0x%x\n", mem_bar_type);
                        return -1;
                    }
                }
        
                // determine size
                // write all 1s, get back the size mask
                pci_cfg_writel(pci, bus->num, pdev->num, 0, 0x10, 0xffffffff);
                // size mask comes back + info bits
                // write all ones and read back. if we get 00 (negative size), size = 4.
                size = pci_cfg_readl(pci, bus->num, pdev->num, 0, 0x10);
        
                // mask all but size mask
                if (bar & 0x1) { // I/O
                    size &= 0xfffffffc;
                } else { // memory
                    size &= 0xfffffff0;
                }
                // two complement, get back the positive size
                size = ~size;
                size++;
        
                // now we have to put back the original bar
                pci_cfg_writel(pci, bus->num, pdev->num, 0, 0x10, bar);
        
                if (size == 0) { // size = 0 -> non-existent bar, skip to next one
                    continue;
                }
        
                uint32_t start = 0;
                // we have a 64-bit address but bar1 is all zeros
                state->mem_start = bar & 0xfffffff0;
                state->mem_end = state->mem_start + size;
                foundmem = 1;
        
                INFO("Adding nvme device: bus=%u dev=%u func=%u: mem_start=%p mem_end=%p\n",
                    bus->num, pdev->num, 0,
                    state->mem_start, state->mem_end);

                uint16_t pci_cmd = NVME_PCI_CMD_MEM_ACCESS_EN | NVME_PCI_CMD_IO_ACCESS_EN | NVME_PCI_CMD_LANRW_EN | NVME_PCI_CMD_INT_DISABLE;
                DEBUG("init fn: new pci cmd: 0x%04x\n", pci_cmd);
                pci_cfg_writew(pci, bus->num, pdev->num, 0, NVME_PCI_CMD_OFFSET, pci_cmd);
                DEBUG("init fn: pci_cmd 0x%04x expects 0x%04x\n",
                    pci_cfg_readw(pci, bus->num,pdev->num, 0, NVME_PCI_CMD_OFFSET),
                    pci_cmd);
                DEBUG("init fn: pci status 0x%04x\n",
                    pci_cfg_readw(pci, bus->num,pdev->num, 0, NVME_PCI_STATUS_OFFSET));

                sprintf(state->dev->name, "nvme-%d", num);
            
                if (!foundmem) {
                    ERROR("init fn: ignoring device %s as it has no memory access method\n",state->dev->name);
                    continue;
                }

                if (nvme_init_check_cap(state)) {
                    ERROR("init fn: NVMe PCI capabilities check failed for device %s\n", state->dev->name);
                    continue;
                }

                // This is a supported NVMe device
                state->pci_dev = pdev;

                // PCI/nautilus idependent
                if (nvme_init_nvme_setup(state)) {
                    ERROR("NVME init failed\n");
                    return -1;
                }

                // now that we have block size and number of blocks, register block device (mostly copied from ata.c)
                if (nk_nvme_init_register_blkdev(state, num)){
                    ERROR("Failed to register NVMe block device\n");
                    return -1;
                }
                INFO("Device nvme-%d potentially operational\n", num);
                num++;
            }
        }
    }
    return 0;
}

void nk_nvme_deinit()
{
    INFO("deinit\n");
}

static int handle_nvmetest (char *buf, void *priv)
{
    // arguments: starting block, total number of blocks
    uint64_t start;
    uint32_t count;
    uint32_t reps;
    struct nk_block_dev *d;
    struct nk_block_dev_characteristics c;

    if (sscanf(buf, "nvmetest %lu %u %u", &start, &count, &reps) != 3) {
        nk_vc_printf("Usage: nvmetest start_block count repetitions\n");
        return -1;
    }

    if (!(d=nk_block_dev_find("nvme-0"))) {
        nk_vc_printf("Can't find nvme-0\n");
        return -1;
    }

    if (nk_block_dev_get_characteristics(d, &c)) {
        nk_vc_printf("Can't get characteristics of nvme-0\n");
        return -1;
    }

    nk_vc_printf("nvme block_size=%lu, num_blocks=%lu\n", c.block_size, c.num_blocks);

    // allocate buffer for read/write data
    uint32_t num_pages = 1 + (count*(c.block_size) / NVME_PAGE_SIZE);
    uint8_t *data = malloc(num_pages*NVME_PAGE_SIZE);
    memset(data, 0, num_pages*NVME_PAGE_SIZE);

    for (int r=0; r<reps; r++) {
        DEBUG("nvmetest rep %d\n", r); // repeating is useful for testing queue wraparound
        // generate and write some data
        int j = 0;
        for (int i=0; i< c.block_size*count; i++){
            if (i % c.block_size == 0){
                j++;
            }
            data[i] = (i + j + r) % 256; // offset by block address
        }
        int status = nk_block_dev_write(d, start, count, data, NK_DEV_REQ_BLOCKING, NULL, NULL);
        if (status){
            ERROR("Write command failed! with code 0x%08x\n", status);
            free(data);
            return -1;
        };

        // read back the data to make sure it matches
        memset(data, 0, num_pages*NVME_PAGE_SIZE);
        status = nk_block_dev_read(d, start, count, data, NK_DEV_REQ_BLOCKING, NULL, NULL);
        if (status){
            ERROR("Read command failed! with code 0x%08x\n", status);
            free(data);
            return -1;
        };
        j = 0;
        for (int i=0; i<c.block_size*count; i++){
            if (i%c.block_size == 0){
                j++;
            }
            if (data[i] != (i + j + r) % 256){
                ERROR("data[%d] = 0x%02x, expected 0x%02x\n", i, data[i], (i + j + r) % 256);
                free(data);
                return -1;
            }
        }
    }

    free(data);
    nk_vc_printf("PASSED NVME TEST\n");
    return 0;
}

static struct shell_cmd_impl nvmetest_impl = {
    .cmd      = "nvmetest",
    .help_str = "nvmetest dev",
    .handler  = handle_nvmetest,
};
nk_register_shell_cmd(nvmetest_impl);
