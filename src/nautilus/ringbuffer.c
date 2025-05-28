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

#include <nautilus/ringbuffer.h>

struct ring_buffer* create_ring_buffer(int capacity) {
    struct ring_buffer *q = malloc(sizeof(struct ring_buffer) + ((capacity - 1) * sizeof(struct nvme_command)));
    q->slots = nk_semaphore_create(0, capacity, 0, 0);
    spinlock_init(&q->lock);
    q->capacity = capacity;
    q->write_index = 0;
    q->read_index = 0;
    return q;
}

delete_ring_buffer(struct ring_buffer* q) {
    nk_semaphore_release(q->slots);
    free(q);
}

int ring_enqueue(struct ring_buffer *q, struct nvme_command entry) {
    nk_semaphore_down(q->slots);
    spin_lock(&q->lock);
    if ((q->write_index + 1) % q->capacity == q->read_index) {
        // handle full buffer in caller
        // should be impossible because semaphore?
        return -1;
    }
    q->buffer[q->write_index] = entry;
    q->write_index = (q->write_index + 1) % q->capacity;
    spin_unlock(&q->lock);
    nk_semaphore_up(q->slots);
    return 0;
}

int ring_dequeue(struct ring_buffer *q, struct nvme_command *value) {
    spin_lock(&q->lock);
    if (q->write_index == q->read_index) {
        // handle empty buffer in caller
        // should be impossible because semaphore?
        return -1;
    }
    *value = q->buffer[q->read_index];
    q->read_index = (q->read_index + 1) % q->capacity;
    spin_unlock(&q->lock);
    nk_semaphore_up(q->slots);
    return 0;
}