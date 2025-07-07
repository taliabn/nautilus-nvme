/*
 * This file is part of the Nautilus AeroKernel developed
 * by the Constellation, Interweaving, Hobbes, and V3VEE
 * Projects with funding from the United States National
 * Science Foundation and the Department of Energy.
 *
 * The V3VEE Project is a joint project between Northwestern University
 * and the University of New Mexico.  The Hobbes Project is a collaboration
 * led by Sandia National Laboratories that includes several national
 * laboratories and universities. The Interweaving Project is a
 * joint project between Northwestern University and Illinois Institute
 * of Technology.   The Constellation Project is a joint project
 * between Northwestern University, Carnegie Mellon University,
 * and Illinois Institute of Technology.
 * You can find out more at:
 * http://www.v3vee.org
 * http://xstack.sandia.gov/hobbes
 * http://interweaving.org
 * http://constellation-project.net
 *
 * Copyright (c) 2023, Kevin Hayes <kjhayes@u.northwestern.edu>
 * Copyright (c) 2023, The V3VEE Project  <http://www.v3vee.org>
 *                     The Hobbes Project <http://xstack.sandia.gov/hobbes>
 * All rights reserved.
 *
 * Authors: Kevin Hayes <kjhayes@u.northwestern.edu>
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "LICENSE.txt".
 */

#include <dev/vga_early.h>
#include <nautilus/errno.h>
#include <nautilus/gpudev.h>
#include <nautilus/init.h>
#include <nautilus/printk.h>
#include <nautilus/vc.h>

static nk_gpu_dev_video_mode_t
boot_vga_text_mode_80_25 = {
    .type = NK_GPU_DEV_MODE_TYPE_TEXT,
    .width = 80,
    .height = 25,
    .channel_offset = {
        NK_GPU_DEV_CHANNEL_OFFSET_TEXT,
        NK_GPU_DEV_CHANNEL_OFFSET_ATTR,
        -1,
        -1,
    },
    .flags = 0,
    .mouse_cursor_width = 0,
    .mouse_cursor_height = 0,
    .mode_data = NULL,
};

static int
boot_vga_dev_get_available_modes(void *state, nk_gpu_dev_video_mode_t modes[], uint32_t *num) 
{
    if(*num < 1) {
        *num = 0;
    } else {
        *num = 1;
        modes[0] = boot_vga_text_mode_80_25;
    }
    return 0;
}
static int
boot_vga_dev_get_mode(void *state, nk_gpu_dev_video_mode_t *mode) {
    if(mode) {
        *mode = boot_vga_text_mode_80_25;
    }
    return 0;
}
static int
boot_vga_dev_set_mode(void *state, nk_gpu_dev_video_mode_t *mode) {
    if(memcmp(mode, &boot_vga_text_mode_80_25, sizeof(nk_gpu_dev_video_mode_t))) {
        // Not exactly the same
        return 1;
    }
    return 0;
}
static int
boot_vga_dev_flush(void *state) {
    // Don't need to do anything
    return 0;
}
static int
boot_vga_dev_text_set_char(void *state, nk_gpu_dev_coordinate_t *location, nk_gpu_dev_char_t *val) {
    vga_write_screen(location->x, location->y, vga_make_entry(val->symbol, val->attribute));
    return 0;
}

static int
boot_vga_dev_text_set_box_from_charmap(void *state, nk_gpu_dev_box_t *box, nk_gpu_dev_charmap_t *map) {
    size_t min_width = map->width < box->width ? map->width : box->width;
    size_t min_height = map->height < box->height ? map->height : box->height;

    // row major
    for(size_t y_i = 0; y_i < min_height; y_i++) {
      for(size_t x_i = 0; x_i < min_width; x_i++) {
          nk_gpu_dev_char_t *c = &NK_GPU_DEV_CHARMAP_CHAR(map,x_i,y_i);
          uint16_t val = vga_make_entry(c->symbol, c->attribute);
          vga_write_screen(box->x + x_i, box->y + y_i, val);
      }
    }

    return 0;
}

static int
boot_vga_dev_text_set_cursor(void *state, nk_gpu_dev_coordinate_t *location, uint32_t flags) {
    if((flags & NK_GPU_DEV_TEXT_CURSOR_ON) == 0) {
        // TODO: we can't hide the cursor yet
        return -EUNIMPL;
    }
    if((flags & NK_GPU_DEV_TEXT_CURSOR_BLINK) != 0) {
        // TODO: we can't handle blinking the cursor yet
        return -EUNIMPL;
    } 
    vga_set_cursor(location->x, location->y);
    return 0;
}

struct nk_gpu_dev_int boot_vga_dev_int = {
    .get_available_modes = boot_vga_dev_get_available_modes,
    .get_mode = boot_vga_dev_get_mode,
    .set_mode = boot_vga_dev_set_mode,
    .flush = boot_vga_dev_flush,
    .text_set_char = boot_vga_dev_text_set_char,
    .text_set_box_from_charmap = boot_vga_dev_text_set_box_from_charmap,
    .text_set_cursor = boot_vga_dev_text_set_cursor,
};

static int
boot_vga_dev_init(void) {
    //printk("boot_vga_dev_init!\n");

    vga_init_screen();

    struct nk_gpu_dev *dev = nk_gpu_dev_register("boot-vga",0,&boot_vga_dev_int,NULL);
    if(dev == NULL) {
        return -ENOMEM;
    }

    return 0;
}

static int
boot_vga_gpudev_console_launch(void) {
    int res = nk_vc_start_gpudev_console("boot-vga");
    if(res) {
        return res;
    }
    return nk_vc_make_gpudev_console_current("boot-vga");
}

nk_declare_driver_init(boot_vga_dev_init);
nk_declare_launch_init(boot_vga_gpudev_console_launch);

