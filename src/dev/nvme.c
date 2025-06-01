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
    uint64_t prp_list[512];
    uint32_t* namespaces; // For now empty, can implement as linked list
    // ^^ probably want to use the pre-existing nautilus linked list implementation
    // for now let's have a struct with a single namespace
    struct nvme_namespace *ns;
    // stats?
    uint16_t q_id;
    // Where registers are mapped into the physical memory address space
    uint64_t  mem_start;
    uint64_t  mem_end;
    uint8_t mdts; // max data transfer size (in units of the minimum memory page size and reported as a power of two )
};

// static variables
// list of discovered devices
static struct list_head dev_list;

// forward declarations:
int nvme_create_io_sq_cmd(struct nvme_dev *nvme, uint16_t sq_id, uint16_t cq_id, nvme_sq *io_sq);
int nvme_create_io_cq_cmd(struct nvme_dev *nvme, uint16_t cq_id, nvme_cq *io_cq);
void nvme_write_reg(uint32_t offset, uint32_t value){}; // TODO: actually write this
static int nvme_pci_init(struct nvme_dev *state);

// helper functions
static inline volatile uint32_t read_arr_32le(const uint8_t * d, uint16_t o){
    uint32_t ret = 0;
    for (int i=0; i<4; i++){
        ret |= ((uint32_t)d[o+i]) << (8*i);
    }
    return ret;
}; 

static inline volatile uint64_t read_arr_64le(const uint8_t * d, uint16_t o){
    uint64_t ret = 0;
    for (int i=0; i<8; i++){
        ret |= ((uint64_t)d[o+i]) << (8*i);
    }
    return ret;
}; 

// Queues

// Currently very based on OSDev, should modify to be more Nautilus
int create_admin_submission_queue(struct nvme_dev *nvme) {
	nvme->admin_sq.addr = (uint64_t)malloc(PAGE_SIZE); // IDK if this is still valid without paging
	if (nvme->admin_sq.addr == 0) {
		return 1;
    }
	nvme->admin_sq.size = NVME_ASQS;
    nvme->admin_sq.id = 0; // admin queues will arbitrarily have id=0
    // Bottom 12 bits of address must be 0. Luckily, nautilus malloc guarantees this
	WRITE_MEM(nvme, NVME_ASQ_OFFSET, nvme->admin_sq.addr);
    // write admin submission queue size to Admin Queue Attributes (AQA) register
    uint32_t aqa = READ_MEM(nvme, NVME_AQA_OFFSET);
    aqa |= NVME_ASQS;
    WRITE_MEM(nvme, NVME_AQA_OFFSET, aqa);
	return 0;
}

int create_admin_completion_queue(struct nvme_dev *nvme) {
	nvme->admin_cq.addr = (uint64_t)malloc(PAGE_SIZE);
	if (nvme->admin_cq.addr == 0) {
		return 1;
    }
	nvme->admin_cq.size = NVME_ACQS;
    nvme->admin_cq.id = 0; // admin queues will arbitrarily have id=0
    // Bottom 12 bits of address must be 0. Luckily, nautilus malloc guarantees this
	WRITE_MEM(nvme, NVME_ACQ_OFFSET,nvme->admin_cq.addr);
    // write admin completion queue size to Admin Queue Attributes (AQA) register
    uint32_t aqa = READ_MEM(nvme, NVME_AQA_OFFSET);
    aqa |= NVME_ACQS << 16;
    WRITE_MEM(nvme, NVME_AQA_OFFSET, aqa);
	return 0;
}

int create_io_submission_queue(struct nvme_dev *nvme, nvme_sq *sq) {
	sq->addr = (uint64_t)malloc(PAGE_SIZE); // IDK if this is still valid without paging
	if (sq->addr == 0) {
		return 1;
    }
	sq->size = 63;
    // io queues will arbitrarily have id=1
    uint16_t sq_id = 1;
    uint16_t cq_id = 1;
    sq->id = sq_id;
	return nvme_create_io_sq_cmd(nvme, sq_id, cq_id, sq);
}

int create_io_completion_queue(struct nvme_dev *nvme, nvme_cq *cq) {
	cq->addr = (uint64_t)malloc(PAGE_SIZE);
	if (cq->addr == 0) {
		return 1;
    }
	cq->size = 63;
    // io queues will arbitrarily have id=1
    uint16_t cq_id = 1;
    cq->id = cq_id;
	return nvme_create_io_cq_cmd(nvme, cq_id, cq);
}


/* Command submisison helper functions */

static int nvme_queue_submit_cmd(struct nvme_dev *nvme, struct nvme_queue *sq, struct nvme_queue *cq, struct nvme_command *cmd, struct nvme_completion comp){
    // enque cmd in ring buffer
    if (enqueue(sq, cmd)){
        ERROR("unable to enqueue command in ringbuff\n");
        return -1;
    };
    // ring sq doorbell
    uint16_t sq_tail_doorbell = 0x1000 + 2*sq->id * (4 << nvme->dstrd);
    WRITE_MEM(nvme, sq_tail_doorbell, sq.tail);
    DEBUG("command successfully submitted!\n");
    // somehow we have to move the submission ringbuffer's tail
    // poll: TODO
    ring_dequeue(cq, comp);
    uint16_t cq_head_doorbell = 0x1000 + 2*(sq->id + 1) * (4 << nvme->dstrd);

    return 0;
}

static int nvme_submit_admin_cmd(struct nvme_dev *nvme, struct nvme_command *cmd, struct nvme_completion *comp)
{
    return nvme_queue_submit_cmd(struct nvme_dev *nvme, &(nvme->admin_sq), &(nvme->admin_cq), cmd, comp);
}

static int nvme_submit_io_cmd(struct nvme_dev *nvme, struct nvme_command *cmd, struct nvme_completion *comp)
{
    return nvme_queue_submit_cmd(struct nvme_dev *nvme, &(nvme->io_sq), &(nvme->io_sq), cmd, comp);
}

/* IO Commands */

static int nvme_rw_cmd(struct nvme_dev *nvme, uint8_t opc, uint32_t nsid, 
                void *buff, uint64_t lba, uint32_t num_blocks, struct nvme_completion *comp)
{
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));

	cmd.opc = opc;
	cmd.nsid = htole32(nsid);
	cmd.prp1 = (uintptr_t)buff;
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
	cmd.cdw10 = htole32(((io_sq->size-1) << 16) | sq_id);
	cmd.cdw11 = htole32((cq_id << 16) | 0x01);
    
    return nvme_submit_admin_cmd(nvme, &cmd, comp); 
}

int nvme_create_io_cq_cmd(struct nvme_dev *nvme, uint16_t cq_id, nvme_cq *io_cq, ){
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

static int read_blocks(void *state, uint64_t blocknum, uint64_t count, uint8_t *dest, void (*callback)(nk_block_dev_status_t, void *), void *context){
    // TODO: WRITEME!
    return 0;
}

static int write_blocks(void *state, uint64_t blocknum, uint64_t count, uint8_t *src, void (*callback)(nk_block_dev_status_t, void *), void *context)
{
    // TODO: WRITEME!
    return 0;
}

static int get_characteristics(void *state, struct nk_block_dev_characteristics *c)
{
    // STATE_LOCK_CONF;
    // struct ata_blkdev_state *s = (struct ata_blkdev_state *)state;
    
    // STATE_LOCK(s);
    // c->block_size = s->block_size;
    // c->num_blocks = s->num_blocks;
    // STATE_UNLOCK(s);
    return 0;
}

static struct nk_block_dev_int inter = 
{
    .get_characteristics = get_characteristics,
    .read_blocks = read_blocks,
    .write_blocks = write_blocks,
};

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

    struct nvme_dev *device = (struct nvme_dev *)malloc(sizeof(struct nvme_dev));
    memset(device, 0, sizeof(struct nvme_dev));
    if (!device) {
        ERROR("Cannot allocate NVMe device state\n");
        return -1;
    }

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

            struct nvme_dev *state = (struct nvme_dev *)malloc(sizeof(struct nvme_dev));
            if (!state) {
                ERROR("Cannot allocate NVMe device state\n");
                return -1;
            }
            memset(state, 0, sizeof(*state));
        
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

        // Check the controller version is supported.
        uint32_t version = READ_MEM(state, NVME_VS_OFFSET);
        if (version != NVME_VERSION){
            ERROR("Unsupported NVME version 0x%08x\n", version);
            return -1;
        }
        DEBUG("Supported NVME version 0x%08x found\n", version);

        uint64_t cap = READ_MEM64(state, NVME_CAP_OFFSET);
        // Check the capabilities register for support of the I/O NVMe command set.
        if (!((cap >> 37) & 0x1)) {
            ERROR("NVMe controller does not support I/O command set\n");
            return -1;
        }

        // Check the capabilities register for support of the host's page size.
        // Memory page size max and min
        uint32_t mpsmin = 1 << (12 + ((cap>>48) & 0xf));
        uint32_t mpsmax = 1 << (12 + ((cap>>52) & 0xf));
        if (NVME_PAGE_SIZE < mpsmin || NVME_PAGE_SIZE > mpsmax){
            ERROR("Unsupported nvme page size. min=0x%08x; max=0x%08x; actual=0x%08x\n", mpsmin, mpsmax, NVME_PAGE_SIZE);
            return -1;
        }
        state->dstrd = 1 << (2 + ((cap>>32) & 0xf));

        // This is a supported NVMe device
        state->pci_dev = pdev;

        // NVME specific setup

        // Reset the controller 
        // triggered by a falling edge on the Enable bit of the controller configuration register
        WRITE_MEM(state, NVME_CC_OFFSET, 1);
        io_delay();
        WRITE_MEM(state, NVME_CC_OFFSET, 0);

        // Wait for controller to indicate reset is complete (bit 0 of CSCT.RDY == 0)
        while (READ_MEM(state, NVME_CSTS_OFFSET) & 0x1){
            io_delay();
        }

        // Create admin queues
        if (create_admin_submission_queue(state) ||
            create_admin_completion_queue(state)) 
        {
            ERROR("Failure to create admin queues\n");
            return 1;
        }

        // Set the controller configuration
        uint32_t cc = READ_MEM(state, NVME_CC_OFFSET);
        // We'll leave arbitration mechanism (AMS) as default (round robin)
        // command set selected (CSS) should support I/O by default
        // Set max page size (MPS)
        cc |= NVME_MPS << 7; // memory page size is (2 ^ (12 + MPS))

        // Start the controller by setting the enable bit
        cc |= 1;
        WRITE_MEM(state, NVME_CC_OFFSET, cc);
        // Wait for controller to indicate it's ready to accept commands (bit 0 of CSCT.RDY == 1)
        while (!(READ_MEM(state, NVME_CSTS_OFFSET) & 0x1)){
            io_delay();
        }

        // If we want interrupts, enable them and register a handler (skipping for now)

        // Send the identify command to the controller. 
        uint8_t id_data[4096]; // data structure returned by identify is 4096 bytes
        if (nvme_identify_controller_cmd(state, id_data)){
            ERROR("Identify controller command failed\n");
            return -1;
        };
        // Check it is an IO controller
        uint8_t ctrlr_type = id_data[111];
        if (ctrlr_type != 0x1){
            ERROR("Unsupported NVME controller type %u returned by Identify", ctrlr_type);
            // return -1;
        }
        // Record the maximum transfer size
        uint8_t mdts = id_data[77];
        DEBUG("max data transfer size=0x%08x\n", (1<<mdts)*mpsmin); // 0 indicates no max
        state->mdts = mdts;

        // Check allowable I/O Completion/Submission Queue Entry Size
        uint8_t sqes = id_data[512];
        uint16_t min_sqes = 1 << (sqes & 0xf); // should be 6
        uint16_t max_sqes = 1 << (sqes >> 4);
        DEBUG("submission queue entry size min=0x%08x; max=0x%08x", min_sqes, max_sqes, NVME_SQES);
        if (NVME_SQES < min_sqes || NVME_SQES > max_sqes){
            ERROR("Unsupported nvme submission queue entry size. min=0x%08x; max=0x%08x; actual=0x%08x\n",  min_sqes, max_sqes, NVME_SQES);
            // return -1;
        }
        uint8_t cqes = id_data[513];
        uint16_t min_cqes = 1 << (cqes & 0xf);
        uint16_t max_cqes = 1 << (cqes >> 4); // should be 4
        DEBUG("completion queue entry size min=0x%08x; max=0x%08x; actual=0x%08x\n", min_cqes, max_cqes, NVME_CQES);
        if (NVME_CQES < min_cqes || NVME_CQES > max_cqes){
            ERROR("Unsupported nvme completion queue entry size. min=0x%08x; max=0x%08x; actual=0x%08x\n", min_cqes, max_cqes, NVME_CQES);
            // return -1;
        }
        // Set I/O Completion/Submission Queue Entry Size in controller configuration register
        // This must be done *before* actually creating I/O queues
        cc = READ_MEM(state, NVME_CC_OFFSET);
        cc |= (NVME_CQES << 20);
        cc |= (NVME_SQES << 16);
        WRITE_MEM(state, NVME_CC_OFFSET, state);
        // Create the first IO completion queue, and the first IO submission queue.
        if (create_io_completion_queue(state, &(state->io_sq)) ||
            create_io_submission_queue(state, &(state->io_cq))) 
        {
            ERROR("Failure to create admin queues\n");
            // return -1;
        }
        // Do stuff with MSI here if we enable interrupts
        // Identify active namespace IDs, and then identify individual namespaces. 
        memset(id_data, 0, sizeof(id_data));
        if (nvme_identify_ns_list_cmd(state, id_data)){
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
            // return -1;
        }

        memset(id_data, 0, sizeof(id_data));
        if (nvme_identify_ns_cmd(state, id_data, nsid0)){
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
        state->ns = ns0;
        ns0->nsid = nsid0; // this should be one (for qemu's first ns)
        ns0->nsze = read_arr_64le(id_data, 0); // num_blocks
        state->num_blocks = ns0->nsze; // TODO: are num_blocks and block_size mandatory for block device state?
        ns0->ncap = read_arr_64le(id_data, 8);
        DEBUG("Namespace nsid=%u, nsze=%lu, ncap=%lu\n", ns0->nsid, ns0->nsze, ns0->ncap);
        // TODO: state->block_size is 64 bits. Ours might not fit? check
        // LBA format. It's possible to have multiple, let's only care about the first one for now
        uint8_t lba_format_idx = id_data[26] & 0xf;
        uint32_t lba_format = read_arr_32le(id_data, 128 + lba_format_idx);
        // qemu defaults to 0 metadata bytes per LBA and no extended LBA. let's keep it that way for now
        ns0->lbads = (lba_format>>16) & 0xf;
        // actual lba data size = (2^lbads);
        state->block_size = 1 << (ns0->lbads);
        DEBUG("LBA Format Index=%u, lba_format=0x%08x, lbads=%u, block_size=%lu\n",
            lba_format_idx, lba_format, ns0->lbads, state->block_size);
        // now that we have block size and number of blocks, register block device (mostly copied from ata.c)
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
        }
        }
    }
    return 0;
}

void nk_nvme_deinit()
{
    INFO("deinit\n");
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
