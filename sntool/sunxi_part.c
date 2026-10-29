// #########################################################################
// sunxi-part: shows and changes the NAND partition layout, as hakchi's scripts use it:
//
//   sunxi-part [--dry-run|-n] [--debug|-d] [grow|shrink|tune]
//
// The layout the kernel uses is the NAND driver's boot-info block after each u-boot copy (kernel
// drivers/block/nand/lib/src/phy/phy.h, struct _boot_info): a partition table at 0x200, a disk
// list at 0x1200 (the kernel creates nanda, nandb... from it), and a checksum. The four copies
// are in NAND blocks 0x08-0x27, seven blocks each; the driver repairs the first two from a good
// one when they are damaged, and hakchi rewrites only those two, so the last two keep the
// factory layout.
//
//   (no command)  prints the layout: "mbr::32768 nanda:boot:6144 nandb:rootfs:131072 ..."
//   grow          merges the factory's unused private and UDISK partitions into data
//   shrink        back to the factory layout, the data filesystem shrunk first
//   tune          checks the data filesystem and resizes it to its partition
//
// grow and shrink rewrite the boot-info block of the first two copies, reload the NAND driver,
// write the Linux partition table (sunxi-nand-part) and then tune. They change something only
// from hakchi's recovery shell (sp_isRecovery) and stop at the first step that fails;
// --dry-run shows the new layout and changes nothing. Behaviour and messages follow the
// sntool hakchi v1.0.4-126 shipped (5833d00, whose source was not published).
// #########################################################################

#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "sunxi_part.h"

// #########################################################################

#define SP_MAGIC 0xaa55a5a5u
#define SP_SUM_SEED 0xae15bc34u//the checksum field's value while summing (_cal_sum_physic_info)
#define SP_UBOOT_START 0x10u//boot-info fields: first block of the u-boot copies
#define SP_UBOOT_NEXT 0x14u//and the block after them
#define SP_MBR_COUNT 0x204u
#define SP_MBR_ARRAY 0x208u
#define SP_MBR_ENTRY 36u
#define SP_MBR_MAX 96u
#define SP_DISKS 0x1200u
#define SP_DISK_ENTRY 24u
#define SP_TOTAL (SP_DISKS+SP_MAX_PARTS*SP_DISK_ENTRY)

#define sp_error(...) (fprintf(stderr,"sunxi-part: " __VA_ARGS__))

static uint32_t rd32(const uint8_t*p)
{
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}

static void wr32(uint8_t*p,uint32_t v)
{
    p[0]=v&0xff;
    p[1]=(v>>8)&0xff;
    p[2]=(v>>16)&0xff;
    p[3]=(v>>24)&0xff;
}

static uint32_t checksum(const uint8_t*info)
{
    uint32_t sum=0;
    for(uint32_t i=0;i<SP_INFO_SIZE;i+=4)
        sum+=(i==8)?SP_SUM_SEED:rd32(info+i);
    return sum;
}

int sp_parse(const uint8_t*info,struct sp_layout*l)
{
    memset(l,0,sizeof(*l));
    if((rd32(info)!=SP_MAGIC)||(rd32(info+4)!=SP_INFO_SIZE)||(rd32(info+8)!=checksum(info)))
        return -1;
    l->count=rd32(info+SP_MBR_COUNT);
    l->total=rd32(info+SP_TOTAL);
    if((l->count<1)||(l->count>SP_MAX_PARTS)||(l->total==0))
        return -1;
    for(uint32_t i=0;i<l->count;++i)
    {
        const uint8_t*e=info+SP_MBR_ARRAY+i*SP_MBR_ENTRY;
        struct sp_part*p=&l->part[i];
        memcpy(p->name,e,16);
        p->name[16]=0;
        p->addr=rd32(e+16);
        p->len=rd32(e+20);
        p->user_type=rd32(e+24);
        p->keydata=rd32(e+28);
        p->ro=rd32(e+32);
        const uint32_t expect=i?(l->part[i-1].addr+l->part[i-1].len):0;
        if(p->addr!=expect)
            return -1;
        if((p->len==0)&&(i+1<l->count))
            return -1;
    }
    const struct sp_part*last=&l->part[l->count-1];
    if((last->addr>=l->total)||(last->len>l->total-last->addr))
        return -1;
    return 0;
}

uint32_t sp_size(const struct sp_layout*l,uint32_t i)
{
    return l->part[i].len?l->part[i].len:(l->total-l->part[i].addr);
}

void sp_render(uint8_t*info,const struct sp_layout*l)
{
    wr32(info+SP_MBR_COUNT,l->count);
    for(uint32_t i=0;i<SP_MBR_MAX;++i)
    {
        uint8_t*e=info+SP_MBR_ARRAY+i*SP_MBR_ENTRY;
        memset(e,0,SP_MBR_ENTRY);
        if(i>=l->count)
            continue;
        const struct sp_part*p=&l->part[i];
        memcpy(e,p->name,strnlen(p->name,16));
        wr32(e+16,p->addr);
        wr32(e+20,p->len);
        wr32(e+24,p->user_type);
        wr32(e+28,p->keydata);
        wr32(e+32,p->ro);
    }
    // The disk list as hakchi writes it: unused entries and the bytes after a name's NUL 0xff.
    for(uint32_t i=0;i<SP_MAX_PARTS;++i)
    {
        uint8_t*e=info+SP_DISKS+i*SP_DISK_ENTRY;
        memset(e,0xff,SP_DISK_ENTRY);
        if(i>=l->count)
            continue;
        const size_t n=strnlen(l->part[i].name,16);
        wr32(e,sp_size(l,i));
        wr32(e+4,0);
        memcpy(e+8,l->part[i].name,n);
        if(n<16)
            e[8+n]=0;
    }
    wr32(info+8,checksum(info));
}

int sp_format(char*out,size_t size,const struct sp_layout*l)
{
    int n=snprintf(out,size,"%s::%u",l->part[0].name,sp_size(l,0));
    for(uint32_t i=1;(i<l->count)&&(n>=0);++i)
    {
        const size_t at=((size_t)n<size)?(size_t)n:size;
        const int m=snprintf(out+at,size-at," nand%c:%s:%u",'a'+(int)i-1,l->part[i].name,sp_size(l,i));
        n=(m<0)?m:(n+m);
    }
    return n;
}

static int find(const struct sp_layout*l,const char*name)
{
    for(uint32_t i=0;i<l->count;++i)
        if(strcmp(l->part[i].name,name)==0)
            return (int)i;
    return -1;
}

// The factory layout: data with only private and UDISK after it.
static int isFactory(const struct sp_layout*l,int d)
{
    return (d>0)&&((uint32_t)d+3==l->count)&&(strcmp(l->part[d+1].name,"private")==0)&&
        (strcmp(l->part[d+2].name,"UDISK")==0);
}

// Grown: data last, to the end.
static int isGrown(const struct sp_layout*l,int d)
{
    return (d>0)&&((uint32_t)d+1==l->count)&&(l->part[d].addr+sp_size(l,d)==l->total);
}

int sp_grow(const struct sp_layout*l,struct sp_layout*out)
{
    const int d=find(l,"data");
    *out=*l;
    if(isGrown(l,d))
        return 1;
    if(!isFactory(l,d))
        return -1;
    out->count=d+1;
    out->part[d].len=l->total-l->part[d].addr;
    memset(&out->part[d+1],0,sizeof(out->part[0])*2);
    return 0;
}

int sp_shrink(const struct sp_layout*l,const struct sp_layout*factory,struct sp_layout*out)
{
    const int d=find(l,"data");
    *out=*l;
    if(isFactory(l,d))
        return 1;
    if(!isGrown(l,d)||(factory==NULL))
        return -1;
    const int fd=find(factory,"data");
    if((fd!=d)||!isFactory(factory,fd)||(factory->total!=l->total))
        return -1;
    for(int i=0;i<d;++i)
        if((strcmp(l->part[i].name,factory->part[i].name)!=0)||(l->part[i].len!=factory->part[i].len))
            return -1;
    out->count=factory->count;
    out->part[d].len=factory->part[d].len;
    out->part[d+1]=factory->part[d+1];
    out->part[d+2]=factory->part[d+2];
    // UDISK's length written out, where the factory has 0 (the rest), as hakchi does.
    out->part[d+2].len=l->total-out->part[d+2].addr;
    return 0;
}

int sp_equal(const struct sp_layout*a,const struct sp_layout*b)
{
    if((a->count!=b->count)||(a->total!=b->total))
        return 0;
    for(uint32_t i=0;i<a->count;++i)
    {
        const struct sp_part*p=&a->part[i];
        const struct sp_part*q=&b->part[i];
        if(strcmp(p->name,q->name)||(p->addr!=q->addr)||(p->len!=q->len)||
            (p->user_type!=q->user_type)||(p->keydata!=q->keydata)||(p->ro!=q->ro))
            return 0;
    }
    return 1;
}

// #########################################################################
// sntool's console checks

// hakchi's boot ramdisk (the recovery and memboot shell) is the only system with this file.
int sp_isRecovery(void)
{
    struct stat st;
    return (stat("/lib/modules-ramfs/kernel/drivers/block/nand/nand.ko",&st)==0)?0:1;
}

// The Mega Drive Mini: the chip ID word at 0x01c23818.
int sp_ismd(void)
{
    const uint32_t addr=0x01c23818u;
    int ret=1;
    const int fd=open("/dev/mem",O_RDONLY|O_SYNC|O_CLOEXEC);
    if(fd<0)
        return 1;
    const long page=sysconf(_SC_PAGESIZE);
    const off_t base=addr&~(uint32_t)(page-1);
    const volatile uint8_t*map=mmap(0,page,PROT_READ,MAP_SHARED,fd,base);
    if(map!=MAP_FAILED)
    {
        const uint32_t v=*(const volatile uint32_t*)(map+(addr-base));
        ret=(v==0x005a7213u)?0:1;
        munmap((void*)map,page);
    }
    close(fd);
    return ret;
}

// #########################################################################
// The NAND boot area, through hakchi's ioctls on /dev/nanda (kernel nfd/nand_blk.c)

#define SP_BLOCK 0x20000u
#define SP_PAGE 2048u
#define SP_FIRST_BLOCK 0x08u
#define SP_END_BLOCK 0x28u
#define SP_AREA_BLOCKS (SP_END_BLOCK-SP_FIRST_BLOCK)
#define SP_MAX_COPY_BLOCKS 8u
#define SP_MAX_COPIES SP_AREA_BLOCKS

enum
{
    sp_ioctl_test=_IO('v',121),
    sp_ioctl_read=_IO('v',124),
    sp_ioctl_write=_IO('v',125),
};

// The kernel's struct burn_param_t.
struct sp_burn_param
{
    void*buffer;
    uint32_t length;
    uint32_t offset;
    uint32_t flags;//raw:2, getoob:1
    void*oob_buffer;
    uint32_t oob_length;
    uint32_t badblocks;
};

struct sp_copy
{
    uint32_t block[SP_MAX_COPY_BLOCKS];
    uint32_t blocks;
    uint8_t*image;//the copy's blocks one after the other
    int valid;
    struct sp_layout layout;
};

struct sp_area
{
    uint8_t*raw;//blocks SP_FIRST_BLOCK.. as read
    int bad[SP_AREA_BLOCKS];
    struct sp_copy copy[SP_MAX_COPIES];
    uint32_t copies;
    uint32_t info_offs;//of the boot-info in a copy
};

static int phyBlock(int fd,unsigned long cmd,uint32_t block,uint8_t*buf,uint32_t*bad)
{
    struct sp_burn_param p;
    memset(&p,0,sizeof(p));
    p.buffer=buf;
    p.length=SP_BLOCK;
    p.offset=block*SP_BLOCK;
    p.flags=1;//raw: stop at a bad block, report it
    if(ioctl(fd,cmd,&p)<0)
        return -1;
    *bad=p.badblocks;
    return 0;
}

static int openNand(void)
{
    uint8_t test[SP_BLOCK];
    struct sp_burn_param p;
    const int fd=open("/dev/nanda",O_RDWR|O_NONBLOCK);
    if(fd<0)
    {
        sp_error("cannot open /dev/nanda: %s\n",strerror(errno));
        return -1;
    }
    memset(&p,0,sizeof(p));
    p.buffer=test;
    p.length=sizeof(test);
    if((ioctl(fd,sp_ioctl_test,&p)<0)||memcmp(test,"hakchi",7))
    {
        sp_error("the kernel does not give raw NAND access (hakchi_test)\n");
        close(fd);
        return -1;
    }
    return fd;
}

static void freeArea(struct sp_area*a)
{
    for(uint32_t c=0;c<a->copies;++c)
        free(a->copy[c].image);
    free(a->raw);
    memset(a,0,sizeof(*a));
}

// Reads the u-boot copies and their boot-info blocks. The copies are found as the NAND driver
// does (uboot_info_init): the good blocks from SP_FIRST_BLOCK, as many per copy as u-boot and
// its boot-info need.
static int readArea(int fd,struct sp_area*a)
{
    memset(a,0,sizeof(*a));
    a->raw=malloc(SP_AREA_BLOCKS*SP_BLOCK);
    if(a->raw==NULL)
        return -1;
    int first=-1;
    for(uint32_t i=0;i<SP_AREA_BLOCKS;++i)
    {
        uint32_t bad=0;
        uint8_t*b=a->raw+i*SP_BLOCK;
        a->bad[i]=(phyBlock(fd,sp_ioctl_read,SP_FIRST_BLOCK+i,b,&bad)<0)||(bad>0);
        if((first<0)&&!a->bad[i]&&(memcmp(b+4,"uboot\0\0\0",8)==0))
            first=(int)i;
    }
    if(first<0)
    {
        sp_error("no u-boot in NAND blocks 0x%x-0x%x\n",SP_FIRST_BLOCK,SP_END_BLOCK-1);
        return -1;
    }
    const uint32_t ubootLen=rd32(a->raw+first*SP_BLOCK+0x14);
    a->info_offs=(ubootLen+SP_PAGE-1)/SP_PAGE*SP_PAGE;
    const uint32_t per=(a->info_offs+SP_INFO_SIZE+SP_BLOCK-1)/SP_BLOCK;
    if((ubootLen==0)||(per>SP_MAX_COPY_BLOCKS))
    {
        sp_error("unexpected u-boot length 0x%x\n",ubootLen);
        return -1;
    }
    struct sp_copy*c=&a->copy[0];
    for(uint32_t i=0;(i<SP_AREA_BLOCKS)&&(a->copies<SP_MAX_COPIES);++i)
    {
        if(a->bad[i])
            continue;
        c->block[c->blocks++]=SP_FIRST_BLOCK+i;
        if(c->blocks<per)
            continue;
        c->image=malloc(per*SP_BLOCK);
        if(c->image==NULL)
            return -1;
        for(uint32_t j=0;j<per;++j)
            memcpy(c->image+j*SP_BLOCK,a->raw+(c->block[j]-SP_FIRST_BLOCK)*SP_BLOCK,SP_BLOCK);
        const uint8_t*info=c->image+a->info_offs;
        c->valid=(memcmp(c->image+4,"uboot\0\0\0",8)==0)&&(sp_parse(info,&c->layout)==0)&&
            (rd32(info+SP_UBOOT_START)==SP_FIRST_BLOCK)&&(rd32(info+SP_UBOOT_NEXT)==SP_END_BLOCK);
        if(++a->copies<SP_MAX_COPIES)
            c=&a->copy[a->copies];
    }
    return 0;
}

// Writes info into the first half of the copies (the ones the driver repairs and hakchi
// writes), each block only if it changes, read back after writing.
static int writeInfo(int fd,struct sp_area*a,const uint8_t*info)
{
    uint8_t*check=malloc(SP_BLOCK);
    uint32_t written=0;
    int ret=0;
    if(check==NULL)
        return 1;
    for(uint32_t c=0;(c<a->copies/2)&&(ret==0);++c)
    {
        struct sp_copy*copy=&a->copy[c];
        memcpy(copy->image+a->info_offs,info,SP_INFO_SIZE);
        for(uint32_t j=0;(j<copy->blocks)&&(ret==0);++j)
        {
            const uint8_t*block=copy->image+j*SP_BLOCK;
            const uint32_t n=copy->block[j];
            uint32_t bad=0;
            if(memcmp(block,a->raw+(n-SP_FIRST_BLOCK)*SP_BLOCK,SP_BLOCK)==0)
                continue;
            if((phyBlock(fd,sp_ioctl_write,n,(uint8_t*)block,&bad)<0)||(bad>0))
            {
                sp_error("nand write failed at block %x\n",n);
                ret=1;
            }
            else if((phyBlock(fd,sp_ioctl_read,n,check,&bad)<0)||(bad>0)||memcmp(check,block,SP_BLOCK))
            {
                sp_error("nand block %x reads back different\n",n);
                ret=1;
            }
            else
                ++written;
        }
    }
    free(check);
    fprintf(stdout,"nand sectors written: %u\n",written);
    return ret;
}

// #########################################################################

static int debug=0;

// Runs a shell command; its output only with --debug. Its exit code, 255 if it did not run.
static int run(const char*cmd)
{
    char line[512];
    if(debug)
        fprintf(stderr,"+ %s\n",cmd);
    FILE*pf=popen(cmd,"re");
    if(pf==NULL)
        return 255;
    while(fgets(line,sizeof(line),pf))
        if(debug)
            fputs(line,stderr);
    const int status=pclose(pf);
    return WIFEXITED(status)?WEXITSTATUS(status):255;
}

static int dataMounted(void)
{
    char dev[256];
    int found=0;
    FILE*f=fopen("/proc/mounts","re");
    if(f==NULL)
        return 0;
    while(!found&&(fscanf(f,"%255s%*[^\n]",dev)==1))
        found=(strcmp(dev,"/dev/nandc")==0);
    fclose(f);
    return found;
}

static int umountBase(void)
{
    run("hakchi \"umount_base\"");
    if(dataMounted())
    {
        sp_error("/dev/nandc is mounted, not changing it\n");
        return 1;
    }
    return 0;
}

// fsck, 64bit off, resize to size ("612352s", or NULL: the whole partition), 3% reserved, fsck.
// fsck's exit code 1 is "errors corrected".
static int resizeData(const char*size)
{
    char cmd[128];
    if(umountBase())
        return 1;
    if(run("fsck.ext4 \"-fy\" \"/dev/nandc\" 2>/dev/null")>1)
        return 1;
    if(run("resize2fs \"-fs\" \"/dev/nandc\" 2>/dev/null"))
        return 1;
    if(size)
        snprintf(cmd,sizeof(cmd),"resize2fs \"-f\" \"/dev/nandc\" \"%s\" 2>/dev/null",size);
    else
        snprintf(cmd,sizeof(cmd),"resize2fs \"-f\" \"/dev/nandc\" 2>/dev/null");
    if(run(cmd))
        return 1;
    if(run("tune2fs \"-m\" \"3\" \"/dev/nandc\" 2>/dev/null"))
        return 1;
    if(run("fsck.ext4 \"-fy\" \"/dev/nandc\" 2>/dev/null")>1)
        return 1;
    return 0;
}

// The Linux partition table in /dev/nand, from the layout: the first partition's start, then
// name, length and user type of each; a last partition with user type 0 gets length 0 (the rest).
static int writeLinuxTable(const struct sp_layout*l)
{
    char cmd[1024];
    int n=snprintf(cmd,sizeof(cmd),"echo \"y\" | \"sunxi-nand-part\" \"-f\" \"a20\" \"/dev/nand\" \"%u\"",
        l->part[1].addr);
    for(uint32_t i=1;(i<l->count)&&(n>0)&&((size_t)n<sizeof(cmd));++i)
    {
        const struct sp_part*p=&l->part[i];
        const uint32_t len=((i+1==l->count)&&(p->user_type==0))?0:sp_size(l,i);
        n+=snprintf(cmd+n,sizeof(cmd)-n," \"%s %u %u\"",p->name,len,p->user_type);
    }
    if((n<0)||((size_t)n>=sizeof(cmd)))
        return 1;
    return run(cmd)?1:0;
}

static void printLayout(const struct sp_layout*l)
{
    char line[1024];
    sp_format(line,sizeof(line),l);
    fprintf(stdout,"%s\n",line);
}

static int change(int fd,struct sp_area*a,const struct sp_layout*cur,const struct sp_layout*next,
    const char*what,int dry)
{
    char size[32];
    uint8_t*info;
    if(sp_isRecovery()!=0)
    {
        printLayout(cur);
        return 0;
    }
    if(dry)
    {
        printLayout(next);
        fprintf(stderr,"(not) writing %s partition table...\n",what);
        return 0;
    }
    const int d=find(next,"data");
    if(strcmp(what,"shrink")==0)
    {
        // The filesystem first, 1 MiB under the smaller partition.
        snprintf(size,sizeof(size),"%us",sp_size(next,d)-2048);
        if(resizeData(size))
        {
            sp_error("could not shrink the data filesystem, layout not changed\n");
            printLayout(cur);
            return 1;
        }
    }
    info=malloc(SP_INFO_SIZE);
    if(info==NULL)
        return 1;
    memcpy(info,a->copy[0].image+a->info_offs,SP_INFO_SIZE);
    sp_render(info,next);
    fprintf(stderr,"writing %s partition table...\n",what);
    int ret=umountBase();
    if(ret==0)
        ret=writeInfo(fd,a,info);
    free(info);
    if(ret)
        return 1;
    run("hakchi \"reload_nand\"");
    if(writeLinuxTable(next))
    {
        sp_error("sunxi-nand-part failed\n");
        return 1;
    }
    ret=resizeData(NULL);
    if(ret)
        sp_error("could not resize the data filesystem to its partition\n");
    printLayout(next);
    return ret;
}

int sunxi_part(int argc,const char*argv[])
{
    const char*cmd=0;
    int dry=0;
    for(int i=0;i<argc;++i)
    {
        if((strcmp(argv[i],"--dry-run")==0)||(strcmp(argv[i],"-n")==0))
            dry=1;
        else if((strcmp(argv[i],"--debug")==0)||(strcmp(argv[i],"-d")==0))
            debug=1;
        else if(!cmd&&(!strcmp(argv[i],"grow")||!strcmp(argv[i],"shrink")||!strcmp(argv[i],"tune")))
            cmd=argv[i];
        else
        {
            fprintf(stderr,"don't know what to do with '%s'\n",argv[i]);
            return 1;
        }
    }

    struct sp_area area;
    const int fd=openNand();
    if(fd<0)
        return 1;
    int ret=1;
    if(readArea(fd,&area)==0)
    {
        const struct sp_layout*cur=0;
        const struct sp_layout*factory=0;
        for(uint32_t c=0;c<area.copies;++c)
        {
            const struct sp_copy*copy=&area.copy[c];
            if(copy->valid&&!cur)
                cur=&copy->layout;
            // The factory layout: from an untouched copy, one hakchi does not write.
            if(copy->valid&&!factory&&(c>=area.copies/2)&&isFactory(&copy->layout,find(&copy->layout,"data")))
                factory=&copy->layout;
        }
        struct sp_layout next;
        if(!cur||!area.copy[0].valid)
            sp_error("no valid boot-info block in the first u-boot copy\n");
        else if(!cmd)
        {
            printLayout(cur);
            ret=0;
        }
        else if(sp_ismd()==0)
            sp_error("the Mega Drive Mini's layout is not supported\n");
        else if(strcmp(cmd,"tune")==0)
        {
            if(dry)
            {
                printLayout(cur);
                fprintf(stderr,"(not) resizing the data filesystem...\n");
                ret=0;
            }
            else if(sp_isRecovery()!=0)
                sp_error("tune: only from hakchi's recovery shell\n");
            else
            {
                ret=resizeData(NULL);
                printLayout(cur);
            }
        }
        else
        {
            const int grow=(strcmp(cmd,"grow")==0);
            const int r=grow?sp_grow(cur,&next):sp_shrink(cur,factory,&next);
            if(r>0)
            {
                printLayout(cur);
                ret=0;
            }
            else if(r<0)
                sp_error("%s: %s, not changing it\n",cmd,(!grow&&!factory)?
                    "no factory layout in the last u-boot copies":"not a layout this knows");
            else
                ret=change(fd,&area,cur,&next,cmd,dry);
        }
    }
    freeArea(&area);
    close(fd);
    return ret;
}

// #########################################################################
