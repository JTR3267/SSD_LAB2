/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "ssd_fuse_header.h"
#define SSD_NAME       "ssd_file"
enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};


static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

typedef union pca_rule PCA_RULE;
union pca_rule
{
    unsigned int pca;
    struct
    {
        unsigned int page : 16;
        unsigned int block: 16;
    } fields;
};

PCA_RULE curr_pca;

unsigned int* L2P;

static int ssd_resize(size_t new_size)
{
    //set logic size to new_size
    if (new_size >= LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024  )
    {
        return -ENOMEM;
    }
    else
    {
        logic_size = new_size;
        return 0;
    }

}

static int ssd_expand(size_t new_size)
{
    //logical size must be less than logic limit

    if (new_size > logic_size)
    {
        return ssd_resize(new_size);
    }

    return 0;
}

void nand_write_log(char* logBuf, int size)
{
    if(size>512)
        return;
    FILE *fileRsult;
    size_t numwritten;
    fileRsult = fopen("log", "w");
    numwritten = fwrite(logBuf, sizeof(char), size, fileRsult);
    printf("write %zu bytes\n", numwritten);
    fclose(fileRsult);
    nand_write_size += 512;
}

void ftl_write_log()
{
    // TODO
    nand_write_log(/*your log*/, /*your log size*/);
}

void nand_read_log(char* logBuf, int size) // logBuf is output 
{
    if(size>512)
        return;
    FILE *fileRsult;
    size_t numwritten;
    fileRsult = fopen("log", "r");
    numwritten = fread(logBuf, sizeof(char), size, fileRsult);
    printf("read %zu bytes\n", numwritten);
    fclose(fileRsult);
}

static int nand_read(char* data_buf, char* spare_buf, int pca)
{
    char nand_name[100];
    FILE* fptr;
    PCA_RULE my_pca;
    my_pca.pca = pca;
    char* tmp_spare = calloc((PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), sizeof(char));

    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    //read
    if ( (fptr = fopen(nand_name, "r") ))
    {
        fseek(fptr, my_pca.fields.page * (PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), SEEK_SET );
        fread(tmp_spare, 1, (PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), fptr);
        fclose(fptr);

        memcpy(data_buf,tmp_spare,PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);
        memcpy(spare_buf,tmp_spare+PHYSICAL_DATA_SIZE_BYTES_PER_PAGE,PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE);

    }
    else
    {
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    free(tmp_spare);
    return PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
}

static int nand_write(const char* data_buf, const char* spare_buf, int pca)  //spare can use NULL
{
    char nand_name[100];
    FILE* fptr;
    PCA_RULE my_pca;
    my_pca.pca = pca;
    
    char* tmp_spare;
    tmp_spare = calloc((PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), sizeof(char));
    memcpy(tmp_spare, data_buf, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);
    if(spare_buf != NULL)
        memcpy(tmp_spare+PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, spare_buf, PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE);
    else
        memset(tmp_spare+PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, 0, PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE);

    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    //write
    if ( (fptr = fopen(nand_name, "r+")))
    {
        fseek( fptr, my_pca.fields.page * (PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), SEEK_SET );
        fwrite(tmp_spare, 1, (PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), fptr);
        fclose(fptr);
        physic_size ++;
        // nand_table[my_pca.fields.nand].valid_cnt++;
    }
    else
    {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }
    nand_write_size += PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
    free(tmp_spare);
    return PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
}

static int nand_erase(int nand)
{
    char nand_name[100];
	int found = 0;
    FILE* fptr;

    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, nand);

    //erase
    if ( (fptr = fopen(nand_name, "w")))
    {
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand (%s) erase nand = %d, return %d\n", nand_name, nand, -EINVAL);
        return -EINVAL;
    }


	if (found == 0)
	{
		printf("nand erase not found\n");
		return -EINVAL;
	}

    printf("nand erase %d pass\n", nand);
    return 1;
}

static unsigned int get_next_pca()
{
    /*  TODO: seq A, need to change to seq B */
	
    if (curr_pca.pca == INVALID_PCA)
    {
        //init
        curr_pca.pca = 0;
        return curr_pca.pca;
    }
    else if (curr_pca.pca == FULL_PCA)
    {
        //ssd is full, no pca can be allocated
        printf("No new PCA\n");
        return FULL_PCA;
    }

    if ( curr_pca.fields.block == PHYSICAL_NAND_NUM - 1)
    {
        curr_pca.fields.page += 1;
    }
    curr_pca.fields.block = (curr_pca.fields.block + 1 ) % PHYSICAL_NAND_NUM;


    if ( curr_pca.fields.page >= (NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE) )
    {
        printf("No new PCA\n");
        curr_pca.pca = FULL_PCA;
        return FULL_PCA;
    }
    else
    {
        printf("PCA = lba %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);
        return curr_pca.pca;
    }

}


static int ftl_read( char* buf, size_t lba)
{
    // TODO
}

static int ftl_write(const char* buf, size_t lba_rnage, size_t lba)
{
    // TODO
}



static int ssd_file_type(const char* path)
{
    if (strcmp(path, "/") == 0)
    {
        return SSD_ROOT;
    }
    if (strcmp(path, "/" SSD_NAME) == 0)
    {
        return SSD_FILE;
    }
    return SSD_NONE;
}

static int ssd_getattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi)
{
    (void) fi;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = stbuf->st_mtime = time(NULL);
    switch (ssd_file_type(path))
    {
        case SSD_ROOT:
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            break;
        case SSD_FILE:
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = logic_size;
            break;
        case SSD_NONE:
            return -ENOENT;
    }
    return 0;
}

static int ssd_open(const char* path, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_NONE)
    {
        return 0;
    }
    return -ENOENT;
}

static int ssd_do_read(char* buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range, rst ;
    char* tmp_buf;

    // off limit
    if ((offset ) >= logic_size)
    {
        return 0;
    }
    if ( size > logic_size - offset)
    {
        //is valid data section
        size = logic_size - offset;
    }

    tmp_lba = offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
	tmp_lba_range = (offset + size - 1) / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - (tmp_lba) + 1;
    tmp_buf = calloc(tmp_lba_range * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));

    for (int i = 0; i < tmp_lba_range; i++) {
        // TODO
    }

    memcpy(buf, tmp_buf + offset % PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, size);

    free(tmp_buf);
    return size;
}

static int ssd_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_read(buf, size, offset);
}


static int ssd_do_write(const char* buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range, process_size;
    int idx, curr_size, remain_size;

    host_write_size += size;
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    tmp_lba = offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
    tmp_lba_range = (offset + size - 1) / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - (tmp_lba) + 1;

    process_size = 0;
    remain_size = size;
    curr_size = 0;
    for (idx = 0; idx < tmp_lba_range; idx++)
    {
        // TODO
    }

    return size;
}

static int ssd_write(const char* path, const char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
{

    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_write(buf, size, offset);
}

static int ftl_erase(int target_lba)
{
    // TODO
}

static int ssd_do_erase(int eraseStart, int eraseSize)
{
    //TODO
}

static int ssd_truncate(const char* path, off_t size, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }

    return ssd_resize(size);
}

static int ssd_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi, enum fuse_readdir_flags flags)
{
    (void) fi;
    (void) offset;
    (void) flags;
    if (ssd_file_type(path) != SSD_ROOT)
    {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, SSD_NAME, NULL, 0, 0);
    return 0;
}

static int ssd_ioctl(const char* path, unsigned int cmd, void* arg, struct fuse_file_info* fi, unsigned int flags, void* data)
{

    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    if (flags & FUSE_IOCTL_COMPAT)
    {
        return -ENOSYS;
    }
    switch (cmd)
    {
        case SSD_GET_LOGIC_SIZE:
            *(size_t*)data = logic_size;
            printf(" --> logic size: %ld\n", logic_size);
            return 0;
        case SSD_GET_PHYSIC_SIZE:
            *(size_t*)data = physic_size;
            printf(" --> physic size: %ld\n", physic_size);
            return 0;
        case SSD_GET_WA:
            *(double*)data = (double)nand_write_size / (double)host_write_size;
            return 0;
        case SSD_LOGIC_ERASE:
			{
            unsigned long long eraseFrame;
            eraseFrame = *(unsigned long long*) data;
            int eraseSize = eraseFrame & 0xFFFFFFFF;
            int eraseStart = (eraseFrame >> 32) & 0xFFFFFFFF;           
            printf(" --> erase start: %u, erase size: %u\n", eraseStart, eraseSize);
            ssd_do_erase(eraseStart, eraseSize);
			}
            return 0;
    }
    return -EINVAL;
}

static const struct fuse_operations ssd_oper =
{
    .getattr        = ssd_getattr,
    .readdir        = ssd_readdir,
    .truncate       = ssd_truncate,
    .open           = ssd_open,
    .read           = ssd_read,
    .write          = ssd_write,
    .ioctl          = ssd_ioctl,
};

int main(int argc, char* argv[])
{
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
	nand_write_size = 0;
	host_write_size = 0;
    curr_pca.pca = INVALID_PCA;
    L2P = malloc(LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE * sizeof(int));
    memset(L2P, INVALID_PCA, sizeof(int)*LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);

    // TODO：if log file exist, yes -> rebuild, no -> create new


    //create nand file
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        FILE* fptr;
        snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
        fptr = fopen(nand_name, "w");
        if (fptr == NULL)
        {
            printf("open fail");
        }
        fclose(fptr);
    }
    return fuse_main(argc, argv, &ssd_oper, NULL);
}
