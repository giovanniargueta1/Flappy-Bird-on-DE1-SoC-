#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/string.h>  

#include "address_map_arm.h"  

#define SUCCESS 0
#define DEVICE_NAME "video"
#define BUF_LEN 80
#define PIPE_WIDTH 20         // Width of each pipe

// Buffer and register definitions
#define STATUS_REG 0xFF20302C
#define BUFFER_REG 0xFF203020
#define BACKBUFFER_REG  0xFF203024  // VGA Back Buffer register
#define CHAR_BUFFER_BASE 0xC9000000 // Character buffer base address
#define CHAR_CTRL_BASE 0xFF203030   // Character buffer controller base
#define CHAR_BUFFER_SIZE 8192       // 8192 bytes (2 pages)
#define PIXEL_BUFFER_1 0xC8000000   // First buffer
#define PIXEL_BUFFER_2 0xC0000000   // Second buffer
#define BUFFER_SIZE 0x0003FFFF      // Buffer size
#define STATUS_S_BIT 0x1        // S bit in Status register
#define BUFFER_SWAP_TRIGGER 1   // Value to write to trigger buffer swap

// VGA screen size constants for character buffer
#define CHAR_WIDTH 80
#define CHAR_HEIGHT 60

// Global variables for pixel buffer
void *LW_virtual;                // Used to access FPGA lightweight bridge
volatile int *pixel_ctrl_ptr;    // Virtual address of pixel buffer controller
volatile void *pixel_buffer;     // Used for virtual address of pixel buffer
volatile void *current_back_buffer; // Pointer to current back buffer memory
volatile char *char_buffer;      // Virtual address of character buffer
int resolution_x, resolution_y;  // VGA screen size

// Buffer control registers
static volatile int *buffer_register;     // Pointer to Buffer register
static volatile int *backbuffer_register; // Pointer to Backbuffer register

// Character device variables
static dev_t dev_no;
static struct class *cls;
static struct cdev video_cdev;

// Function prototypes
static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);
void get_screen_specs(volatile int *);
void clear_screen(void);
void plot_pixel(int, int, short int);
void draw_line(int, int, int, int, short int);
void draw_box(int, int, int, int, short int);  
void sync_vga(void);  
void swap_buffers(void);
void clear_text_buffer(void);
void draw_text(int x, int y, const char *text);
void draw_pipe_direct(int x, int top_height, int gap_size, short int color);

// File operation structure
static struct file_operations fops = {
    .read = device_read,
    .write = device_write,
    .open = device_open,
    .release = device_release
};

// Clear the character buffer by filling it with space characters
void clear_text_buffer(void) {
    int i, j;
    for (j = 0; j < CHAR_HEIGHT; j++) {
        for (i = 0; i < CHAR_WIDTH; i++) {
            *(char_buffer + j * CHAR_WIDTH + i) = ' '; // ASCII space
        }
    }
}

void draw_pipe_direct(int x, int top_height, int gap_size, short int color) {
    int y, i;
    int max_top;
    int bottom_y_start;
    
    // Bounds checking
    if (x < 0 || x + PIPE_WIDTH >= resolution_x || top_height < 0) {
        return;
    }

    max_top = (top_height < resolution_y) ? top_height : resolution_y;
    
    // Draw top section of pipe
    for (y = 0; y < max_top; y++) {
        for (i = 0; i < PIPE_WIDTH && (x + i) < resolution_x; i++) {
            plot_pixel(x + i, y, color);
        }
    }

    // Draw bottom section
    bottom_y_start = top_height + gap_size;
    if (bottom_y_start < resolution_y) {
        for (y = bottom_y_start; y < resolution_y; y++) {
            for (i = 0; i < PIPE_WIDTH && (x + i) < resolution_x; i++) {
                plot_pixel(x + i, y, color);
            }
        }
    }
}

// Draw ASCII text at specified coordinates (x, y)
void draw_text(int x, int y, const char *text) {
    int offset;
    if (x >= CHAR_WIDTH || y >= CHAR_HEIGHT) return; // Boundary check

    offset = y * CHAR_WIDTH + x;
    while (*text && offset < CHAR_WIDTH * CHAR_HEIGHT) {
        *(char_buffer + offset) = *text++;
        offset++;
    }
}

// Updated swap_buffers function
void swap_buffers(void) {
    volatile int *status_reg = (volatile int *)(LW_virtual + STATUS_REG - LW_BRIDGE_BASE);
    volatile void *temp;
    
    // Trigger the hardware buffer swap
    *buffer_register = BUFFER_SWAP_TRIGGER;
    
    // Wait for the swap to complete (S bit becomes 0)
    while ((*status_reg & STATUS_S_BIT) != 0);
    
    // Update our software pointers to match hardware swap
    temp = pixel_buffer;
    pixel_buffer = current_back_buffer;
    current_back_buffer = temp;
}

// Get screen resolution
void get_screen_specs(volatile int *pixel_ctrl_ptr) {
    int resolution_reg = *(pixel_ctrl_ptr + 2);
    resolution_x = (resolution_reg >> 16) & 0xFFFF;
    resolution_y = resolution_reg & 0xFFFF;

    if (resolution_x < resolution_y) {
        int temp = resolution_x;
        resolution_x = resolution_y;
        resolution_y = temp;
    }

    printk(KERN_INFO "Screen resolution: %d x %d\n", resolution_x, resolution_y);
}

// Updated clear_screen function to clear back buffer
void clear_screen(void) {
    if (!current_back_buffer) {
        printk(KERN_ERR "Error: back buffer is NULL\n");
        return;
    }
    memset_io((void *)current_back_buffer, 0, BUFFER_SIZE);
}

// Updated plot_pixel function to draw to back buffer
void plot_pixel(int x, int y, short int color) {
    volatile short int *pixel_addr;

    if (x < 0 || x >= resolution_x || y < 0 || y >= resolution_y) {
        printk(KERN_ERR "Error: pixel coordinates out of bounds (%d, %d)\n", x, y);
        return;
    }

    pixel_addr = (volatile short int *)(current_back_buffer + (y * 0x400) + (x * 2));
    *pixel_addr = color;
}

// Draw a line between (x0, y0) and (x1, y1) using Bresenham's algorithm
void draw_line(int x0, int y0, int x1, int y1, short int color) {
    int is_steep, temp, deltax, deltay, error, y, y_step, x;

    is_steep = (abs(y1 - y0) > abs(x1 - x0));
    
    if (is_steep) {
        temp = x0; x0 = y0; y0 = temp;
        temp = x1; x1 = y1; y1 = temp;
    }

    if (x0 > x1) {
        temp = x0; x0 = x1; x1 = temp;
        temp = y0; y0 = y1; y1 = temp;
    }

    deltax = x1 - x0;
    deltay = abs(y1 - y0);
    error = -(deltax / 2);
    y = y0;
    y_step = (y0 < y1) ? 1 : -1;

    for (x = x0; x <= x1; x++) {
        if (is_steep) {
            plot_pixel(y, x, color);
        } else {
            plot_pixel(x, y, color);
        }
        error += deltay;
        if (error >= 0) {
            y += y_step;
            error -= deltax;
        }
    }
}

// Draw a filled box (rectangle)
void draw_box(int x1, int y1, int x2, int y2, short int color) {
    int x, y;
    for (y = y1; y <= y2; y++) {
        for (x = x1; x <= x2; x++) {
            plot_pixel(x, y, color);
        }
    }
}

void clear_both_buffers(void) {
    if (!pixel_buffer || !current_back_buffer) {
        printk(KERN_ERR "Error: buffer pointers are NULL\n");
        return;
    }
    // Clear both buffers using memset_io
    memset_io((void *)pixel_buffer, 0, BUFFER_SIZE);
    memset_io((void *)current_back_buffer, 0, BUFFER_SIZE);
}

// Synchronize with the VGA controller
void sync_vga(void) {
    volatile int *status_reg = (volatile int *)(LW_virtual + STATUS_REG - LW_BRIDGE_BASE);
    
    // Write 1 to Buffer register to initiate synchronization
    *buffer_register = BUFFER_SWAP_TRIGGER;
    
    // Wait for S bit to become 0, indicating swap completion
    while ((*status_reg & STATUS_S_BIT) != 0);
}

// Device functions
static int device_open(struct inode *inode, struct file *file) {
    return SUCCESS;
}

static int device_release(struct inode *inode, struct file *file) {
    return SUCCESS;
}

static ssize_t device_read(struct file *filp, char *buffer, size_t length, loff_t *offset) {
    char msg[BUF_LEN];
    int bytes_read = 0;

    snprintf(msg, BUF_LEN, "%d %d", resolution_x, resolution_y);
    bytes_read = strlen(msg) + 1;
    if (copy_to_user(buffer, msg, bytes_read)) {
        return -EFAULT;
    }

    return bytes_read;
}

// Write function for handling commands
static ssize_t device_write(struct file *filp, const char *buffer, size_t length, loff_t *offset) {
    int x1, y1, x2, y2;
    int x, y;
    unsigned int color;
    char cmd[BUF_LEN];
    char *text_str;
    char position_part[BUF_LEN];
    int pipe_x, pipe_top, pipe_gap;
    if (sscanf(cmd, "pipe %d,%d,%d %x", &pipe_x, &pipe_top, &pipe_gap, &color) == 4) {
        draw_pipe_direct(pipe_x, pipe_top, pipe_gap, (short int)color);
        return length;
    }

    if (length > BUF_LEN)
        return -EINVAL;

    if (copy_from_user(cmd, buffer, length))
        return -EFAULT;

    cmd[length] = '\0';

    // Handle erase command for character buffer
    if (strncmp(cmd, "erase", 5) == 0) {
        clear_text_buffer();
        return length;
    }

    // Handle the "text x,y string" command
    if (strncmp(cmd, "text ", 5) == 0) {
        char *comma_pos = strchr(cmd + 5, ',');
        char *space_pos = strchr(cmd + 5, ' ');

        if (comma_pos && space_pos && comma_pos < space_pos) {
            int pos_len = space_pos - (cmd + 5);
            strncpy(position_part, cmd + 5, pos_len);
            position_part[pos_len] = '\0';
            text_str = space_pos + 1;

            size_t text_len = strlen(text_str);
            if (text_str[text_len - 1] == '\n' || text_str[text_len - 1] == '\r') {
                text_str[text_len - 1] = '\0';
            }

            if (sscanf(position_part, "%d,%d", &x, &y) == 2) {
                draw_text(x, y, text_str);
                return length;
            }
        }
        return -EINVAL;  
    }

    if (strncmp(cmd, "clear_both", 10) == 0) {
        clear_both_buffers();
        return length;
    }
    
    // Handle sync command
    if (strncmp(cmd, "sync", 4) == 0) {
        sync_vga();
        return length;
    }

    // Handle swap command
    if (strncmp(cmd, "swap", 4) == 0) {
        swap_buffers();
        return length;
    }

    // Handle clear command (now clears back buffer)
    if (strncmp(cmd, "clear", 5) == 0) {
        clear_screen();
        return length;
    }

    // Handle line command
    if (sscanf(cmd, "line %d,%d %d,%d %x", &x1, &y1, &x2, &y2, &color) == 5) {
        draw_line(x1, y1, x2, y2, (short int)color);
        return length;
    }

    // Handle box command
    if (sscanf(cmd, "box %d,%d %d,%d %x", &x1, &y1, &x2, &y2, &color) == 5) {
        draw_box(x1, y1, x2, y2, (short int)color);
        return length;
    }

    return -EINVAL;
}

// Initialize the module
static int __init start_video(void) {
    if (alloc_chrdev_region(&dev_no, 0, 1, DEVICE_NAME) < 0)
        return -1;

    cls = class_create(THIS_MODULE, DEVICE_NAME);
    if (cls == NULL) {
        unregister_chrdev_region(dev_no, 1);
        return -1;
    }

    if (device_create(cls, NULL, dev_no, NULL, DEVICE_NAME) == NULL) {
        class_destroy(cls);
        unregister_chrdev_region(dev_no, 1);
        return -1;
    }

    cdev_init(&video_cdev, &fops);
    if (cdev_add(&video_cdev, dev_no, 1) < 0) {
        device_destroy(cls, dev_no);
        class_destroy(cls);
        unregister_chrdev_region(dev_no, 1);
        return -1;
    }

    LW_virtual = ioremap_nocache(LW_BRIDGE_BASE, LW_BRIDGE_SPAN);
    if (LW_virtual == NULL) {
        printk(KERN_ERR "Error: ioremap_nocache returned NULL for LW_virtual\n");
        return -1;
    }

    // Set up buffer register pointers
    buffer_register = (volatile int *)(LW_virtual + BUFFER_REG - LW_BRIDGE_BASE);
    backbuffer_register = (volatile int *)(LW_virtual + BACKBUFFER_REG - LW_BRIDGE_BASE);

    pixel_ctrl_ptr = (volatile int *)(LW_virtual + PIXEL_BUF_CTRL_BASE);
    get_screen_specs(pixel_ctrl_ptr);

    // Map both pixel buffers
    pixel_buffer = ioremap_nocache(PIXEL_BUFFER_1, BUFFER_SIZE);
    current_back_buffer = ioremap_nocache(PIXEL_BUFFER_2, BUFFER_SIZE);

    if (!pixel_buffer || !current_back_buffer) {
        printk(KERN_ERR "Error: failed to map pixel buffers\n");
        if (pixel_buffer) iounmap((void *)pixel_buffer);
        if (current_back_buffer) iounmap((void *)current_back_buffer);
        iounmap(LW_virtual);
        return -1;
    }

    // Clear both buffers initially
    memset_io((void *)pixel_buffer, 0, BUFFER_SIZE);
    memset_io((void *)current_back_buffer, 0, BUFFER_SIZE);

    // Set up buffer addresses in the controller
    *buffer_register = PIXEL_BUFFER_1;
    *backbuffer_register = PIXEL_BUFFER_2;

    char_buffer = ioremap_nocache(CHAR_BUFFER_BASE, CHAR_BUFFER_SIZE);
    if (char_buffer == NULL) {
        printk(KERN_ERR "Error: ioremap_nocache returned NULL for char_buffer\n");
        iounmap((void *)pixel_buffer);
        iounmap((void *)current_back_buffer);
        iounmap(LW_virtual);
        return -1;
    }

    clear_text_buffer();
    return SUCCESS;
}

// Cleanup function
static void __exit stop_video(void) {
    iounmap(LW_virtual);
    iounmap((void *)pixel_buffer);
    iounmap((void *)current_back_buffer);
    iounmap(char_buffer);

    cdev_del(&video_cdev);
    device_destroy(cls, dev_no);
    class_destroy(cls);
    unregister_chrdev_region(dev_no, 1);
}

MODULE_LICENSE("GPL");
module_init(start_video);
module_exit(stop_video);
