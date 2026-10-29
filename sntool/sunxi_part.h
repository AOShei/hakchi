// #########################################################################
// sunxi-part: the NAND partition layout of the NES, SNES and Famicom Classic Mini
// (the NAND driver's boot-info block after each u-boot copy), and sntool's console checks.
// #########################################################################

#ifndef SUNXI_PART_H
#define SUNXI_PART_H

#include <stdint.h>
#include <stddef.h>

#define SP_INFO_SIZE 0x8000u
#define SP_MAX_PARTS 24u

struct sp_part
{
    char name[17];
    uint32_t addr;
    uint32_t len;
    uint32_t user_type;
    uint32_t keydata;
    uint32_t ro;
};

struct sp_layout
{
    uint32_t count;
    struct sp_part part[SP_MAX_PARTS];
    uint32_t total;//sectors of the FTL partition holding them all
};

// The boot-info block (SP_INFO_SIZE bytes): 0 if its magic, length and checksum are right and
// its partitions follow each other, starting at 0.
int sp_parse(const uint8_t*info,struct sp_layout*l);
// Writes l into info: partition table, disk list and checksum, the rest of the block kept.
void sp_render(uint8_t*info,const struct sp_layout*l);
// Sectors of partition i as the kernel sizes its disk (a length of 0 is the rest).
uint32_t sp_size(const struct sp_layout*l,uint32_t i);
// "mbr::32768 nanda:boot:6144 ..." without the newline; the length it needs, as snprintf.
int sp_format(char*out,size_t size,const struct sp_layout*l);
// Into out: the layout with private and UDISK merged into data. 0 if done, 1 if l has no such
// partitions after data (already grown), -1 if l is not a layout this knows.
int sp_grow(const struct sp_layout*l,struct sp_layout*out);
// Into out: the grown layout back to the factory one (factory: a layout read from an untouched
// u-boot copy). 0 if done, 1 if l is not grown, -1 if l or factory is not a layout this knows.
int sp_shrink(const struct sp_layout*l,const struct sp_layout*factory,struct sp_layout*out);
int sp_equal(const struct sp_layout*a,const struct sp_layout*b);

// sntool commands: 0 for yes.
int sp_isRecovery(void);
int sp_ismd(void);
int sunxi_part(int argc,const char*argv[]);
// burn_boot1 "default": Nintendo's u-boot back in every copy (fd: /dev/nanda). 0 if done.
int sp_restoreStockUboot(int fd);

#endif
