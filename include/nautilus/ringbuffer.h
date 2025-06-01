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

#include <nautilus/semaphore.h>
#include <nautilus/spinlock.h> // could use mutex instead, doesn't really matter
#include <nautilus/nautilus.h>
#include <naut_string.h> // for memcpy

struct ring_buffer {
    struct nk_semaphore *slots_free_write;
    struct nk_semaphore *slots_free_read;
    spinlock_t lock;
    int capacity;
    int write_index;
    int read_index;
    int struct_size;
    void* buffer; // I think this is correct?
};

struct ring_buffer* create_ring_buffer(int capacity, int struct_size);

void delete_ring_buffer(struct ring_buffer* q);

int ring_enqueue(struct ring_buffer *q, void* entry);

int ring_dequeue(struct ring_buffer *q, void* value);

// TODO: Add try_enqueue() and try_dequeue() to match Nautilus conventions
