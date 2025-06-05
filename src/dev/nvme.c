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
#include <nautilus/ringbuffer.h>

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
    uint64_t addr;
    uint64_t size;
    uint16_t id;
    uint16_t tail;
    uint16_t head;
    struct ring_buffer *ring_buff;
};

typedef struct nvme_queue nvme_sq;

typedef struct nvme_queue nvme_cq;


struct nvme_dev {

    struct nk_dev *dev; // necessary that it's first field (allegedly)
    // pci interrupt and interupt vector
    struct pci_dev *pci_dev;
    struct nk_block_dev *blkdev;
    enum {NONE=0, HD, CD} type;
    uint64_t block_size;
    uint64_t num_blocks;
    char* pci_addr;
    uint8_t* addr;
    int len;
    uint32_t dstrd;
    nvme_sq admin_sq;
    nvme_cq admin_cq;
    nvme_sq io_sq; // For now 1
    nvme_cq io_cq; // For now 1
    uint8_t buffer; // Suggets 2 MiB buffer, but probably unnecesary since Nautilus doesn't page
    uint64_t prp_list[NVME_NUM_PRP]; // physical region page list
    uint32_t* namespaces; // For now empty, can implement as linked list
    // ^^ probably want to use the pre-existing nautilus linked list implementation
    // for now let's have a struct with a single namespace
    struct nvme_namespace *ns;
    // stats?
    uint16_t q_id;
    // Where registers are mapped into the physical memory address space
    uint64_t  mem_start;
    uint64_t  mem_end;
    uint32_t mpsmin;
    uint8_t mdts; // max data transfer size (in units of the minimum memory page size and reported as a power of two )
};

// static variables
// list of discovered devices
static struct list_head dev_list;
static struct nvme_dev* nvme;

// forward declarations:
int nvme_create_io_sq_cmd(struct nvme_dev *nvme, uint16_t sq_id, uint16_t cq_id, nvme_sq *io_sq, struct nvme_completion *comp);
int nvme_create_io_cq_cmd(struct nvme_dev *nvme, uint16_t cq_id, nvme_cq *io_cq, struct nvme_completion *comp);
static int check_block_count(struct nvme_dev *nvme, uint64_t lba, uint64_t num_blocks, uint32_t nsid);
// helper functions
static inline volatile uint32_t read_arr_32le(volatile uint8_t * d, uint16_t o){
    uint32_t ret = 0;
    for (int i=0; i<4; i++){
        ret |= ((uint32_t)d[o+i]) << (8*i);
    }
    return ret;
}; 

static inline volatile uint64_t read_arr_64le(volatile uint8_t * d, uint16_t o){
    uint64_t ret = 0;
    for (int i=0; i<8; i++){
        ret |= ((uint64_t)d[o+i]) << (8*i);
    }
    return ret;
}; 

static int check_csts_fatal_status(struct nvme_dev *nvme){
    // check controller fatal status bit of controller status register
    volatile uint32_t csts = READ_MEM(nvme, NVME_CSTS_OFFSET);
    if ((csts >>1) & 0x1) {
        ERROR("NVMe fatal controller error!!\n");
        return 1;
    }
    return 0;
}

// Queues

int create_admin_submission_queue(struct nvme_dev *nvme) {
    nvme_sq *sq = (nvme_sq*)malloc(sizeof(nvme_sq));
    if (sq==NULL){
        ERROR("Couldn't malloc admin sq\n");
        return -1;
    }
    sq->size = NVME_ASQS;
    sq->ring_buff = create_ring_buffer(sq->size + 1, sizeof(struct nvme_command));
    if (sq->ring_buff==NULL) { // malloc failed
        return -1;
    }
    DEBUG("created ring buffer\n");
    sq->addr = sq->ring_buff->buffer;
    sq->id = 0; // admin queues SHALL have id 0
    sq->tail = 0;
    sq->head = 0;
    nvme->admin_sq = *sq;
    DEBUG("admin_sq addr: 0x%lx\n", nvme->admin_sq.addr);
    // Bottom 12 bits of address MUST be 0!
    if (nvme->admin_sq.addr & 0xfff) {
        ERROR("Admin submission queue address is not 12-bit aligned!\n");
        return -1;
    }
	WRITE_MEM(nvme, NVME_ASQ_OFFSET, nvme->admin_sq.addr);
    // write admin submission queue size to Admin Queue Attributes (AQA) register
    volatile uint32_t aqa = READ_MEM(nvme, NVME_AQA_OFFSET);
    aqa |= NVME_ASQS;
    WRITE_MEM(nvme, NVME_AQA_OFFSET, aqa);
	return 0;
}

int create_admin_completion_queue(struct nvme_dev *nvme) {
    nvme_cq *cq = (nvme_cq*)malloc(sizeof(nvme_cq));
    if (cq==NULL){
        ERROR("Couldn't malloc admin cq\n");
        return -1;
    }
    cq->size = NVME_ACQS;
    cq->ring_buff = create_ring_buffer(cq->size + 1, sizeof(struct nvme_completion));
    if (cq->ring_buff==NULL) { // malloc failed
        return -1;
    }
    cq->addr = cq->ring_buff->buffer;
    cq->id = 0; // admin queues SHALL have id 0
    cq->tail = 0;
    cq->head = 0;
    nvme->admin_cq = *cq;
    DEBUG("admin_cq addr: 0x%lx\n", nvme->admin_cq.addr);
    // The Phase Tag values for all Completion Queue entries shall be initialized to 0
    memset(nvme->admin_cq.addr, 0, (cq->size+1) * sizeof(struct nvme_completion));
    // Bottom 12 bits of address MUST be 0!
    if (nvme->admin_cq.addr & 0xfff) {
        ERROR("Admin submission queue address is not 12-bit aligned!\n");
        return -1;
    }
	WRITE_MEM(nvme, NVME_ACQ_OFFSET,nvme->admin_cq.addr);
    // write admin completion queue size to Admin Queue Attributes (AQA) register
    volatile uint32_t aqa = READ_MEM(nvme, NVME_AQA_OFFSET);
    aqa |= NVME_ACQS << 16;
    WRITE_MEM(nvme, NVME_AQA_OFFSET, aqa);
	return 0;
}

int create_io_submission_queue(struct nvme_dev *nvme, nvme_sq *sq) {
	sq->size = 63;
    sq->ring_buff = create_ring_buffer(sq->size + 1, sizeof(struct nvme_command));
    if (sq->ring_buff==NULL) { // malloc failed
        return -1;
    }
    sq->addr = sq->ring_buff->buffer;
    DEBUG("io_sq addr: 0x%lx\n", sq->addr);
    if (sq->addr & 0xfff) {
        ERROR("IO submission queue address is not 12-bit aligned!\n");
        return -1;
    }
    // io queues will arbitrarily have id=1 (can't be 0 because that is reserved for admin queues)
    uint16_t sq_id = 1;
    uint16_t cq_id = 1;
    sq->id = sq_id;
    sq->tail = 0;
    sq->head = 0;
    struct nvme_completion comp;
	return nvme_create_io_sq_cmd(nvme, sq_id, cq_id, sq, &comp);
}

int create_io_completion_queue(struct nvme_dev *nvme, nvme_cq *cq) {
	cq->size = 63;
    cq->ring_buff = create_ring_buffer(cq->size + 1, sizeof(struct nvme_completion));
    if (cq->ring_buff==NULL) { // malloc failed
        return -1;
    }
    cq->addr = cq->ring_buff->buffer;
    // The address pointer is memory page aligned (based on the value in CC.MPS) unless otherwise specified
    DEBUG("io_cq addr: 0x%lx\n", cq->addr);
    if (cq->addr & 0xfff) {
        ERROR("IO completion queue address is not 12-bit aligned!\n");
        return -1;
    }
    // The Phase Tag values for all Completion Queue entries shall be initialized to 0
    memset(cq->addr, 0, (cq->size+1) * sizeof(struct nvme_completion));
    // io queues will arbitrarily have id=1 (can't be 0 because that is reserved for admin queues)
    uint16_t cq_id = 1;
    cq->id = cq_id;
    cq->tail = 0;
    cq->head = 0;
    struct nvme_completion comp;
	return nvme_create_io_cq_cmd(nvme, cq_id, cq, &comp);
}


/* Command submisison helper functions */

static int nvme_queue_submit_cmd(struct nvme_dev *nvme, struct nvme_queue *sq, struct nvme_queue *cq, struct nvme_command *cmd, struct nvme_completion *comp){
    // physical memory addresses in queues to access
    volatile struct nvme_command *sq_entry = sq->addr + (sq->tail * sizeof(struct nvme_command));
    volatile struct nvme_completion *cq_entry = cq->addr + (cq->head * sizeof(struct nvme_completion));  
    // DEBUG("sq_entry: 0x%lx\n", sq_entry);
    // DEBUG("cq_entry: 0x%lx\n", cq_entry);

    uint16_t sq_tail_doorbell = 0x1000 + ((2*sq->id) * (4 << nvme->dstrd));    
    // enqueue command in submission queue
	sq->tail++;
    if (sq->head == sq->tail + 1){
        ERROR("Submission queue is full!\n");
        return -1;
    }
	if (sq->tail == (sq->size + 1)){
        sq->tail = 0; // wrap
    }
    // DEBUG("sq->tail: %d\n", sq->tail);
    volatile uint8_t prev_phase_tag = cq_entry->phase_tag;
    memcpy((void*)sq_entry, (void*)cmd, sizeof(struct nvme_command));
    // DEBUG("wrote to sq_entry: 0x%lx\n", sq_entry);
    // ring submission queue doorbell by writing address to register
    // tail should be the index of the next *free* slot, not the one we just wrote to
    // DEBUG("cq_entry->phase_tag: %d\n", cq_entry->phase_tag);
    WRITE_MEM(nvme, sq_tail_doorbell, sq->tail);
    // DEBUG("waiting for command to be processed\n");
    // poll
    // hardware will invert phase tag bit upon every write to cq
    // we can use this to tell if the controller has written a new completion entry
    uint64_t timeout = 10000;
    uint64_t i = 0;
    while(cq_entry->phase_tag == prev_phase_tag){
        // if (i>timeout){
        //     ERROR("Timed out waiting for command to be processed!\n");
        //     if (check_csts_fatal_status(nvme)){
        //         ERROR("Fatal controller error!\n");
        //     }
        //     return -1;
        // }
        i++;
        io_delay();
    };
    // DEBUG("cq_entry->phase_tag: %d\n", cq_entry->phase_tag);
    // DEBUG("command processed\n");
    uint16_t cq_head_doorbell = 0x1000 + (((2*cq->id) + 1) * (4 << nvme->dstrd));
    // dequeue completion from completion queue
    cq->head++;
	if (cq->head == (cq->size + 1)){
        cq->head = 0; // wrap
    }
    // controller tells us where the new sq head is depending on how many entries it has consumed
    sq->head = cq_entry->sq_head;  
    // DEBUG("sq->head: %d\n", cq->head);
    // DEBUG("cq->head: %d\n", cq->head);
    // NOTE: *don't* overwrite consumed sq or cq entry or else undefined "hw" behavior
        // we also want to preserve phase tag bit
    // copy completion entry to comp
    memcpy((void*)comp, (void*)cq_entry, sizeof(struct nvme_completion));
    // DEBUG("read from cq_entry: 0x%lx\n", cq_entry);
    // ring completion queue doorbell by writing address to register
    WRITE_MEM(nvme, cq_head_doorbell, cq->head);
    // DEBUG("wrote to cq_head_doorbell\n");
    // DEBUG("sq->tail: %d, sq->head: %d, cq->head: %d\n", sq->tail, sq->head, cq->head);
    DEBUG("command completed with status: 0x%08x and took %d ticks\n", comp->status, i);
    return comp->status;
}

static int nvme_submit_admin_cmd(struct nvme_dev *nvme, struct nvme_command *cmd, struct nvme_completion *comp)
{
    return nvme_queue_submit_cmd(nvme, &(nvme->admin_sq), &(nvme->admin_cq), cmd, comp);
}

static int nvme_submit_io_cmd(struct nvme_dev *nvme, struct nvme_command *cmd, struct nvme_completion *comp)
{
    return nvme_queue_submit_cmd(nvme, &(nvme->io_sq), &(nvme->io_cq), cmd, comp);
}

/* IO Commands */

static int nvme_rw_cmd(struct nvme_dev *nvme, uint8_t opc, uint32_t nsid, 
                void *buff, uint64_t lba, uint32_t num_blocks, struct nvme_completion *comp)
{
    // if ((uintptr_t)buff & (NVME_PAGE_SIZE - 1)) {
    //     ERROR("Buffer address is not aligned to page size!\n");
    //     return -1;
    // }
    check_block_count(nvme, lba, num_blocks, nsid);
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = opc;
	cmd.nsid = htole32(nsid);
    uint64_t num_pages = num_blocks*(nvme->block_size) / NVME_PAGE_SIZE; // truncates
    // PRP (physical page region) entry = pointr to physical memory page
    // page size configured by CC.MPS
    cmd.prp1 = (uintptr_t)buff; // prp1 is always the value of the first PRP
    // if data transfer fits in 1 page, leave prp2=0
    if (num_pages == 1) {
        // if data transfer fits in 2 pages, prp2 is the address of the second page
        cmd.prp2 = (uintptr_t)(buff + NVME_PAGE_SIZE);
    } else if (num_pages > 1) {
        DEBUG("building PRP list, assuming contiguous buffer provided\n");
        // if more than 2 pages are needed, prp2 is pointer to PRP list
        cmd.prp2 = nvme->prp_list;
        // build out PRP list from the provided contiguous buff
        for (uint64_t i=1; i<num_pages; i++){
            // address of the first page is specified in prp1, not the prp list
            nvme->prp_list[i-1] = buff + i*NVME_PAGE_SIZE;
        }
    }
    cmd.cdw10 = htole32(lba & 0xffffffffu);
	cmd.cdw11 = htole32(lba >> 32);
	cmd.cdw12 = htole32(num_blocks-1);

    return nvme_submit_io_cmd(nvme, &cmd, comp);
}

int nvme_write_cmd(struct nvme_dev *nvme, uint32_t nsid, void *buff,
    uint64_t lba, uint32_t num_blocks, struct nvme_completion *comp)
{
	return nvme_rw_cmd(nvme, NVME_OPC_WRITE, nsid, buff, lba, num_blocks, comp);
}

int nvme_read_cmd(struct nvme_dev *nvme, uint32_t nsid, void *buff, 
    uint64_t lba, uint32_t num_blocks, struct nvme_completion *comp)
{
	return nvme_rw_cmd(nvme, NVME_OPC_READ, nsid, buff, lba, num_blocks, comp);
}

/* Admin Commands */

int nvme_identify_controller_or_ns_list_cmd(struct nvme_dev *nvme, uint16_t subsys, void *buff, struct nvme_completion *comp){
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = NVME_OPC_IDENTIFY;
	cmd.prp1 = (uintptr_t)buff; // command output (a single page)
    cmd.cdw10 = htole32(subsys);

    return nvme_submit_admin_cmd(nvme, &cmd, comp);
}

int nvme_identify_controller_cmd(struct nvme_dev *nvme, void *buff, struct nvme_completion *comp){
    return nvme_identify_controller_or_ns_list_cmd(nvme, CONTROLLER, buff, comp);
}

// A list of 1,024 namespace IDs is returned to the host containing active NSIDs in increasing order 
int nvme_identify_ns_list_cmd(struct nvme_dev *nvme, void *buff, struct nvme_completion *comp){
    return nvme_identify_controller_or_ns_list_cmd(nvme, NAMESPACE_LIST, buff, comp);
}

int nvme_identify_ns_cmd(struct nvme_dev *nvme, void *buff, uint32_t nsid, struct nvme_completion *comp){
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = NVME_OPC_IDENTIFY;
	cmd.nsid = htole32(nsid);
	cmd.prp1 = (uintptr_t)buff; // command output (a single page)
    cmd.cdw10 = htole32(NAMESPACE);

    return nvme_submit_admin_cmd(nvme, &cmd, comp);
}

int nvme_create_io_sq_cmd(struct nvme_dev *nvme, uint16_t sq_id, uint16_t cq_id, nvme_sq *io_sq, struct nvme_completion *comp){
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = NVME_OPC_CREATE_IO_SQ;
	cmd.prp1 = htole64(io_sq->addr);
	cmd.cdw10 = htole32(((io_sq->size) << 16) | sq_id);
	cmd.cdw11 = htole32((cq_id << 16) | 0x01);
    
    return nvme_submit_admin_cmd(nvme, &cmd, comp); 
}

int nvme_create_io_cq_cmd(struct nvme_dev *nvme, uint16_t cq_id, nvme_cq *io_cq, struct nvme_completion *comp){
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = NVME_OPC_CREATE_IO_CQ;
	cmd.prp1 = htole64(io_cq->addr);
	cmd.cdw10 = htole32(((io_cq->size) << 16) | cq_id);
    // if we were using interrupts, specify MSI vector + 1 in highword of DWORD11
	/* flags 0x1 = interrupts not enabled, physically contiguous */
	cmd.cdw11 = htole32(0x01);
    
    return nvme_submit_admin_cmd(nvme, &cmd, comp); 
}


int nvme_set_features_cmd(struct nvme_dev *nvme, uint8_t feature,
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

int nvme_set_num_io_queues(struct nvme_dev *nvme, uint16_t num_sq, uint16_t num_cq, struct nvme_completion *comp){
	uint32_t cdw11;

	cdw11 = ((num_cq - 1) << 16) | (num_sq - 1);
	return nvme_set_features_cmd(nvme, NVME_FEAT_NUMBER_OF_QUEUES, cdw11,
	    0, 0, 0, 0, comp);
}

static int check_block_count(struct nvme_dev *nvme, uint64_t lba, uint64_t num_blocks, uint32_t nsid) {
    // currently, we only know about a single namespace
    if (nsid != nvme->ns->nsid) {
        ERROR("Unknown nvme namespace!\n");
        return -1;
    }
    // check if lba and num_blocks are within bounds for a given namespace
    uint64_t ns_block_size = 1 << nvme->ns->lbads;
    if (lba >= nvme->ns->nsze || num_blocks == 0 || (lba + num_blocks) > nvme->ns->nsze || num_blocks > nvme->ns->nsze) {
        ERROR("Block number or num_blocks out of bounds! lba: %lu, num_blocks: %lu, max num_blocks: %lu\n", lba, num_blocks, nvme->ns->nsze);
        return -1;
    }
    uint64_t num_pages = num_blocks * ns_block_size / NVME_PAGE_SIZE; 
    if (num_pages > (NVME_NUM_PRP + 1) ) {
        ERROR("Number of pages spanned exceeds prp list length: num_pages: %lu, max num_pages: %lu\n", num_pages, NVME_NUM_PRP + 1);
        return -1;
    }
    // maybe need to check alignment?
    return 0;
}

static int read_blocks(void *state, uint64_t blocknum, uint64_t count, uint8_t *dest, void (*callback)(nk_block_dev_status_t, void *), void *context)
{
    // TODO: locks or something
    struct nvme_dev *s = (struct nvme_dev *)state;
    struct nvme_completion comp;
    int nvme_status = nvme_read_cmd(nvme, nvme->ns->nsid, dest, blocknum, count, &comp);
    nk_block_dev_status_t blk_dev_status;

    if (nvme_status){
        ERROR("NVMe read command failed! with code 0x%08x\n", nvme_status);
        blk_dev_status = NK_BLOCK_DEV_STATUS_ERROR;
    } else {
        blk_dev_status = NK_BLOCK_DEV_STATUS_SUCCESS;
        DEBUG("NVMe read command succeeded!\n");
    }

    if (callback) {
        DEBUG("calling callback\n");
        callback(blk_dev_status, context);
    }

    return nvme_status;
}

static int write_blocks(void *state, uint64_t blocknum, uint64_t count, uint8_t *src, void (*callback)(nk_block_dev_status_t, void *), void *context)
{
    // TODO: locks or something
    struct nvme_dev *s = (struct nvme_dev *)state;
    struct nvme_completion comp;
    int nvme_status = nvme_write_cmd(nvme, nvme->ns->nsid, src, blocknum, count, &comp);
    nk_block_dev_status_t blk_dev_status;

    if (nvme_status){
        ERROR("NVMe read command failed! with code 0x%08x\n", nvme_status);
        blk_dev_status = NK_BLOCK_DEV_STATUS_ERROR;
    } else {
        blk_dev_status = NK_BLOCK_DEV_STATUS_SUCCESS;
        DEBUG("NVMe write command succeeded!\n");
    }

    if (callback) {
        DEBUG("calling callback\n");
        callback(blk_dev_status, context);
    }

    return nvme_status;
}

static int get_characteristics(void *state, struct nk_block_dev_characteristics *c)
{
    // STATE_LOCK_CONF;
    struct nvme_dev *s = (struct nvme_dev *)state;
    
    // STATE_LOCK(s);
    c->block_size = s->block_size;
    c->num_blocks = s->num_blocks;
    // STATE_UNLOCK(s);
    return 0;
}

static struct nk_block_dev_int inter = 
{
    .get_characteristics = get_characteristics,
    .read_blocks = read_blocks,
    .write_blocks = write_blocks,
};


// Check PCI capabilities register for version, I/O command support, supported page size, and record dstrd
int nvme_init_check_pci_cap(struct nvme_dev *state) {
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

// reset controller, create admin queus, set config register (cc), and start controller
int nvme_init_controller_init (struct nvme_dev *state) {
        // Reset the controller 
        // triggered by a falling edge on the Enable bit of the controller configuration register
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
            return 1;
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
        if (cq==NULL){
            ERROR("Couldn't malloc io cq\n");
            return -1;
        }
        state->io_cq = *cq;
        nvme_sq *sq = (nvme_sq*)malloc(sizeof(nvme_sq));
        if (sq==NULL){
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

// check controller capabilities are compatable and record max data transfer size
int nvme_init_identify_controller(struct nvme_dev *state) {
    // Send the identify command to the controller. 
    uint8_t id_data[4096]; // data structure returned by identify is 4096 bytes
    memset(id_data, 0, sizeof(id_data));
    struct nvme_completion comp;
    if (nvme_identify_controller_cmd(state, id_data, &comp)){
        ERROR("Identify controller command failed\n");
        return -1;
    };
    // Check it is an IO controller
    uint8_t ctrlr_type = id_data[111];
    if (ctrlr_type != 0x1){
        ERROR("Unsupported NVME controller type %u returned by Identify\n", ctrlr_type);
        return -1;
    }
    // Record the maximum transfer size
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
    state->num_blocks = ns0->nsze; // TODO: are num_blocks and block_size mandatory for block device state?
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

int nk_nvme_init_register_blkdev(struct nvme_dev *state, uint16_t num) {

    char blkdev_name[32];
    sprintf(blkdev_name,"nvme-%d", num);
    state->blkdev = nk_block_dev_register(blkdev_name, 0, &inter, state);
    if (!state->blkdev) {
        ERROR("Failed to register %s\n",blkdev_name);
    }
    INFO("Added nvme device %s, type %s, blocksize=%lu, numblocks=%lu\n",
        blkdev_name,
        state->type==HD ? "HD" : state->type==CD ? "CD" : "UNKNOWN", 
        state->block_size,state->num_blocks );
    num++;
    INFO("%s potentially operational\n",state->dev->name);
    return 0;
}

// all the nvme initialization steps that are independent of nautilus or pci
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

    INIT_LIST_HEAD(&dev_list);    
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

                // this line was causing general protection faults. 
                // doesn't look immediately necessary, let's see if we can get away without it...
                // list_add(&dev_list, &(state->dev->dev_list_node));
                sprintf(state->dev->name, "nvme-%d", num);
            
                if (!foundmem) {
                    ERROR("init fn: ignoring device %s as it has no memory access method\n",state->dev->name);
                    continue;
                }

                if (nvme_init_check_pci_cap(state)) {
                    ERROR("init fn: NVMe PCI capabilities check failed for device %s\n", state->dev->name);
                    continue;
                }

                // This is a supported NVMe device
                state->pci_dev = pdev;

                // PCI/nautilus idependent
                nvme_init_nvme_setup(state);

                // now that we have block size and number of blocks, register block device (mostly copied from ata.c)
                if (nk_nvme_init_register_blkdev(state, num)){
                    ERROR("Failed to register NVMe block device\n");
                    return -1;
                }
            }
        }
    }
    // TODO: how to store state better than this?
    nvme = state;
    return 0;
}

void nk_nvme_deinit()
{
    INFO("deinit\n");
}

static int handle_nvmetest (char *buf, void *priv)
{
    nk_vc_printf("hello from nvme test!\n");
    DEBUG("nvme state info: mdts=0x%08x, block_size=%d, num_blocks=%d, nsid=%d\n", nvme->mdts, nvme->block_size, nvme->num_blocks, nvme->ns->nsid);
    DEBUG("\tnamespace info: nsze=%lu, ncap=%lu\n, lbads=%u, block_size=%lu", nvme->ns->nsze, nvme->ns->ncap, nvme->ns->lbads, 1 << nvme->ns->lbads);
    uint8_t id_data[4096];
    memset(id_data, 0, sizeof(id_data));

    struct nvme_completion comp;
    uint64_t lba = 0;
    uint32_t num_blocks = 1;
    int status;
    if (nvme_identify_controller_cmd(nvme, id_data, &comp)){
        ERROR("Identify controller command failed\n");
        return -1;
    };
    // sanity check minimum queue entry sizes
    // min sqes and min cqes should be 6 and 4 respectively
    if ((id_data[512]&0xf)!=0x6 || (id_data[513]&0xf)!=0x4){
        DEBUG("Failed Identify sanity check!\n");
        return -1;
    }
    uint32_t max_num_blocks = 128;
    uint32_t num_pages = max_num_blocks*(nvme->block_size) / NVME_PAGE_SIZE; 
    uint8_t volatile *data = malloc(num_pages*NVME_PAGE_SIZE);
    memset(data, 0, num_pages*NVME_PAGE_SIZE);

    // drive storage is persistant, so clear it first
    status = nvme_write_cmd(nvme, nvme->ns->nsid, data, lba, num_blocks, &comp);
    if (status){
        ERROR("Write command failed! with code 0x%08x\n", status);
        return -1;
    };
    // read back what should be zeros
    status = nvme_read_cmd(nvme, nvme->ns->nsid, data, lba, num_blocks, &comp);
    if (status){
        ERROR("read command failed! with code 0x%08x\n", status);
        return -1;
    };
    for (int i=0; i<nvme->block_size; i++){
        if (data[i] != 0){
            ERROR("data[%d] = 0x%02x, expected 0\n", i, data[i]);
            return -1;
        }
    }
    DEBUG("PASSED WRITE + READ ZERO TEST\n");

    // generate and write some data
    for (int i=0; i<nvme->block_size; i++){
        data[i] = i % 256;
    }
    status = nvme_write_cmd(nvme, nvme->ns->nsid, data, lba, num_blocks, &comp);
    if (status){
        ERROR("Write command failed! with code 0x%08x\n", status);
        return -1;
    };
    memset(data, 0, nvme->block_size*num_blocks);
    status = nvme_read_cmd(nvme, nvme->ns->nsid, data, lba, num_blocks, &comp);
    if (status){
        ERROR("read command failed! with code 0x%08x\n", status);
        return -1;
    };
    for (int i=0; i<nvme->block_size; i++){
        if (data[i] != (i % 256)){
            ERROR("data[%d] = 0x%02x, expected 0x%02x\n", i, data[i], (i % 256));
            return -1;
        }
    }
    DEBUG("PASSED WRITE + READ DATA TEST\n");

    // test writing to multiple blocks at once
    // TODO: add checks that not reading/writing beyond the end of the drive or >1 page (since rn only setting prp1)
    lba = 0; // write to the next block
    num_blocks = 128; // write multiple blocks
    // generate and write some data
    memset(data, 0, nvme->block_size*num_blocks);
    status = nvme_write_cmd(nvme, nvme->ns->nsid, data, lba, num_blocks, &comp);
    if (status){
        ERROR("Write command failed! with code 0x%08x\n", status);
        return -1;
    };
    memset(data, 0, nvme->block_size*num_blocks);
    status = nvme_read_cmd(nvme, nvme->ns->nsid, data, lba, num_blocks, &comp);
    if (status){
        ERROR("read command failed! with code 0x%08x\n", status);
        return -1;
    };
    for (int i=0; i<nvme->block_size*num_blocks; i++){
        if (data[i] != 0){
            ERROR("data[%d] = 0x%02x, expected 0x%02x\n", i, data[i], 0);
            return -1;
        }
    } 
    int j = 0;
    for (int i=0; i<nvme->block_size*num_blocks; i++){
        if (i%nvme->block_size == 0){
            j++;
        }
        data[i] = (i + j) % 256; // start from block index
    }
    status = nvme_write_cmd(nvme, nvme->ns->nsid, data, lba, num_blocks, &comp);
    if (status){
        ERROR("Write command failed! with code 0x%08x\n", status);
        return -1;
    };
    memset(data, 0, nvme->block_size*num_blocks);
    status = nvme_read_cmd(nvme, nvme->ns->nsid, data, lba, num_blocks, &comp);
    if (status){
        ERROR("read command failed! with code 0x%08x\n", status);
        return -1;
    };
    j = 0;
    for (int i=0; i<nvme->block_size*num_blocks; i++){
        if (i%nvme->block_size == 0){
            j++;
        }
        if (data[i] != (i + j) % 256){
            ERROR("data[%d] = 0x%02x, expected 0x%02x\n", i, data[i], (i + j) % 256);
            return -1;
        }
    }    
    DEBUG("PASSED WRITE + READ MULTIPLE BLOCKS TEST\n");
    return 0;
}

static struct shell_cmd_impl nvmetest_impl = {
    .cmd      = "nvmetest",
    .help_str = "nvmetest dev",
    .handler  = handle_nvmetest,
};
nk_register_shell_cmd(nvmetest_impl);
