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
#include <stdbool.h>
#include "ssd_fuse_header.h"
#define SSD_NAME       "ssd_file"
#define min(a, b) ((a) < (b) ? (a) : (b))
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

typedef struct erase_func_param
{
    size_t byte_size;
    off_t offset;
    size_t lba;
} ERASE_FUNC_PARAM;

typedef struct rebuild_L2P
{
    unsigned int pca;
    unsigned int second;
    bool contain;
    bool compare;
} REBUILD_L2P;

unsigned int* L2P;
bool* Block_full;
unsigned int threshold;

static int ssd_do_erase(int eraseStart, int eraseSize, bool writeLog);

static int ssd_resize(size_t new_size)
{
    //set logic size to new_size
    if (new_size >= LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND * (PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE))
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
    fileRsult = fopen("/home/jtr/SSD_LAB2/log", "w");
    if(fileRsult == NULL){
        perror("Error opening file");
        return;
    }
    numwritten = fwrite(logBuf, sizeof(char), size, fileRsult);
    printf("write %zu bytes\n", numwritten);
    fclose(fileRsult);
    nand_write_size += 512;
}

void ftl_write_log()
{
    // TODO
    sleep(1);
    char* log_buf = calloc(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));
    memset(log_buf, 0, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);

    int byte_index, bit_index;
    for(int lba = 0; lba < LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND; lba++){
        byte_index = lba / 8;
        bit_index = lba % 8;
        if(L2P[lba] != INVALID_PCA){
            log_buf[byte_index] |= (1 << bit_index);
        }
        else{
            log_buf[byte_index] &= ~(1 << bit_index);
        }
    }

    unsigned int seconds = (unsigned int) time(NULL);
    memcpy(log_buf + 100, &seconds, sizeof(unsigned int));

    nand_write_log(log_buf, 512);
    free(log_buf);
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
        physic_size -= 20;
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

    if ( curr_pca.fields.page == PAGE_NUMBER_PER_NAND - 1)
    {
        //Block_full[block] = false, block empty or using
        //Block_full[block] = true, block full
        Block_full[curr_pca.fields.block] = true;
        for(int block = 0; block < PHYSICAL_NAND_NUM; block++){
            if(!Block_full[block]){
                curr_pca.fields.block = block;
                break;
            }
        }
    }
    curr_pca.fields.page = (curr_pca.fields.page + 1 ) % PAGE_NUMBER_PER_NAND;
    printf("PCA = lba %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);
    return curr_pca.pca;
}

static int ftl_gc()
{
    sleep(1);
    printf("do gc\n");
    int valid_page[PHYSICAL_NAND_NUM] = {0};
    int unmap_table[PHYSICAL_NAND_NUM][PAGE_NUMBER_PER_NAND];
    for(int lba = 0; lba < LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND; lba++){
        if(L2P[lba] != INVALID_PCA){
            PCA_RULE pca;
            pca.pca = L2P[lba];
            unmap_table[pca.fields.block][valid_page[pca.fields.block]++] = lba;
        }
    }

    int minimum_valid_page = PAGE_NUMBER_PER_NAND;
    int target_block;
    for(int block = 0; block < PHYSICAL_NAND_NUM; block++){
        if(Block_full[block] && valid_page[block] < minimum_valid_page){
            target_block = block;
            minimum_valid_page = valid_page[block];
        }
    }
    
    char* tmp_buf = calloc(512, sizeof(char));
    char* spare_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));
    unsigned int seconds = (unsigned int) time(NULL);
    for(int i = 0; i < minimum_valid_page; i++){
        PCA_RULE pca;
        pca.pca = L2P[unmap_table[target_block][i]];
        nand_read(tmp_buf, spare_buf, pca.pca);
        pca.pca = get_next_pca();
        memcpy(spare_buf, &seconds, sizeof(unsigned int));
        nand_write(tmp_buf, spare_buf, pca.pca);
        L2P[unmap_table[target_block][i]] = pca.pca;
    }
    free(tmp_buf);
    free(spare_buf);
    
    Block_full[target_block] = false;
    sleep(1);
    return nand_erase(target_block);
}


static int ftl_read(char* buf, char* spare_buf, size_t lba)
{
    // TODO
    if(L2P[lba] != INVALID_PCA){
        return nand_read(buf, spare_buf, L2P[lba]);
    }
    else{
        return 0;
    }
}

static int ftl_write(const char* buf, size_t size, off_t offset, size_t lba)
{
    // TODO
    if(physic_size > threshold) {
        ftl_gc();
    }

    int rst;
    PCA_RULE pca;
    pca.pca = get_next_pca();

    char* spare_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));
    memset(spare_buf, 0, PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE);
    
    unsigned int seconds = (unsigned int) time(NULL);
    unsigned int ui_lba = (unsigned int) lba;
    memcpy(spare_buf, &seconds, sizeof(unsigned int));
    memcpy(spare_buf + sizeof(unsigned int), &ui_lba, sizeof(unsigned int));

    if(size < PHYSICAL_DATA_SIZE_BYTES_PER_PAGE) {
        char* tmp_buf = calloc(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));
        char* tmp_spare = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));
        if(!ftl_read(tmp_buf, tmp_spare, lba)){
            memset(tmp_buf, 0, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);
        }
        memcpy(tmp_buf + offset % 512, buf, size);
        rst = nand_write(tmp_buf, spare_buf, pca.pca);
        free(tmp_buf);
        free(tmp_spare);
    }
    else{
        rst = nand_write(buf, spare_buf, pca.pca);
    }

    free(spare_buf);

    if (rst > 0)
    {
        L2P[lba] = pca.pca;
        return 512 ;
    }
    else
    {
        printf(" --> Write fail !!!");
        return -EINVAL;
    }
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
    int tmp_lba, tmp_lba_range, rst;
    char* tmp_buf;

    // off limit
    if ((offset ) >= LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE)
    {
        return 0;
    }
    if ( size > LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - offset)
    {
        //is valid data section
        size = LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - offset;
    }

    tmp_lba = offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
	tmp_lba_range = (offset + size - 1) / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - (tmp_lba) + 1;
    tmp_buf = calloc(tmp_lba_range * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));
    char* spare_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));

    for (int i = 0; i < tmp_lba_range; i++) {
        // TODO
        rst = ftl_read(tmp_buf + i * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, spare_buf, tmp_lba++);
        if ( rst == 0)
        {
            //data has not be written, return empty data
            memset(tmp_buf + i * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, 0, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);
        }
        else if (rst < 0 )
        {
            free(tmp_buf);
            free(spare_buf);
            return rst;
        }
    }

    memcpy(buf, tmp_buf + offset % PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, size);
    free(tmp_buf);
    free(spare_buf);

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
    int idx, curr_size, remain_size, rst;

    host_write_size += size;
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    tmp_lba = offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
    tmp_lba_range = (offset + size - 1) / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - (tmp_lba) + 1;

    ssd_do_erase(offset, size, false);

    process_size = 0;
    remain_size = size;

    for (idx = 0; idx < tmp_lba_range; idx++)
    {
        // TODO
        curr_size = PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
        if(offset % PHYSICAL_DATA_SIZE_BYTES_PER_PAGE != 0 && idx == 0){
            curr_size = min(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE * (offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + 1) - offset, remain_size);
        }
        else if(idx == tmp_lba_range - 1 && remain_size % PHYSICAL_DATA_SIZE_BYTES_PER_PAGE != 0){
            curr_size = remain_size;
        }

        rst = ftl_write(buf + process_size, curr_size, offset, tmp_lba + idx);
        if ( rst == 0 ){
            //write full return -enomem;
            return -ENOMEM;
        }
        else if (rst < 0){
            //error
            return rst;
        }
        remain_size -= curr_size;
        process_size += curr_size;
        offset += curr_size;
    }

    return size;
}

static int ssd_write(const char* path, const char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
    sleep(1);
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_write(buf, size, offset);
}

static bool ftl_erase(size_t size, off_t offset, size_t lba)
{
    // TODO
    bool rst = true;
    if(size < PHYSICAL_DATA_SIZE_BYTES_PER_PAGE){
        char* tmp_buf = calloc(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));
        char* spare_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));
        if(ftl_read(tmp_buf, spare_buf, lba)){
            L2P[lba] = INVALID_PCA;
            char* zero_buf = calloc(512, sizeof(char));
            memset(zero_buf, 0, 512);
            memset(tmp_buf + offset % 512, 0, size);
            if (memcmp(tmp_buf, zero_buf, 512) != 0) {
                ftl_write(tmp_buf, 512, offset, lba);
                rst = false;
            }
            free(zero_buf);
        }
        else{
            rst = false;
        }
        free(tmp_buf);
        free(spare_buf);
    }
    else{
        if (L2P[lba] == INVALID_PCA) rst = false;
        else L2P[lba] = INVALID_PCA;
    }
    return rst;
}

static int ssd_do_erase(int eraseStart, int eraseSize, bool writeLog)
{
    //TODO
    int tmp_lba, tmp_lba_range;
    int idx, curr_size, remain_size;

    if (eraseStart >= LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE)
    {
        return 0;
    }
    if (eraseSize > LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - eraseStart)
    {
        //is valid data section
        eraseSize = LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - eraseStart;
    }

    tmp_lba = eraseStart / 512;
    tmp_lba_range = (eraseStart + eraseSize - 1) / 512 - (tmp_lba) + 1;

    remain_size = eraseSize;

    ERASE_FUNC_PARAM write_delay_array[2];
    int write_delay_size = 0;
    bool do_erase = false;

    for (idx = 0; idx < tmp_lba_range; idx++)
    {
        curr_size = 512;
        if(eraseStart % 512 != 0 && idx == 0){
            curr_size = min(512 * (eraseStart / 512 + 1) - eraseStart, remain_size);
        }
        else if(idx == tmp_lba_range - 1 && remain_size % 512 != 0){
            curr_size = remain_size;
        }

        if(curr_size < 512){
            write_delay_array[write_delay_size].byte_size = curr_size;
            write_delay_array[write_delay_size].offset = eraseStart;
            write_delay_array[write_delay_size].lba = tmp_lba + idx;
            write_delay_size++;
        }
        else{
            do_erase |= ftl_erase(curr_size, eraseStart, tmp_lba + idx);
        }

        remain_size -= curr_size;
        eraseStart += curr_size;
    }

    for(int i = 0; i < write_delay_size && writeLog; i++){
        do_erase |= ftl_erase(write_delay_array[i].byte_size, write_delay_array[i].offset, write_delay_array[i].lba);
    }

    if(writeLog && do_erase){
        ftl_write_log();
    }

    return eraseSize;
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
            sleep(1);
            unsigned long long eraseFrame;
            eraseFrame = *(unsigned long long*) data;
            int eraseSize = eraseFrame & 0xFFFFFFFF;
            int eraseStart = (eraseFrame >> 32) & 0xFFFFFFFF;           
            printf(" --> erase start: %u, erase size: %u\n", eraseStart, eraseSize);
            ssd_do_erase(eraseStart, eraseSize, true);
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

static bool file_exist(char* file_path){
    return access(file_path, F_OK) == 0;
}

static void rebuild(char* log_file){
    REBUILD_L2P* time_L2P = malloc(LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND * sizeof(REBUILD_L2P));

    if(log_file != NULL){
        char* log_buf = calloc(512, sizeof(char));
        nand_read_log(log_buf, 512);

        unsigned int log_time;
        memcpy(&log_time, log_buf + 100, sizeof(unsigned int));

        int byte_index, bit_index;
        for (int lba = 0; lba < LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND; lba++) {
            byte_index = lba / 8;
            bit_index = lba % 8;
            time_L2P[lba].pca = INVALID_PCA;
            time_L2P[lba].contain = (log_buf[byte_index] >> bit_index) & 1;
            time_L2P[lba].second = log_time;
            time_L2P[lba].compare = false;
        }

        free(log_buf);
    }
    else{
        for (int lba = 0; lba < LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND; lba++) {
            time_L2P[lba].pca = INVALID_PCA;
            time_L2P[lba].contain = true;
            time_L2P[lba].second = 0;
            time_L2P[lba].compare = true;
        }
    }

    PCA_RULE pca;
    char* spare_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));
    char* data_buf = calloc(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));
    char* zero_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));
    memset(zero_buf, 0, PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE);

    for(int block = 0; block < PHYSICAL_NAND_NUM; block++){
        pca.fields.block = block;
        bool block_not_empty;
        for(int page = 0; page < PAGE_NUMBER_PER_NAND; page++){
            pca.fields.page = page;
            nand_read(data_buf, spare_buf, pca.pca);
            if(memcmp(spare_buf, zero_buf, PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE) != 0){
                physic_size++;
                unsigned int write_time, lba;
                memcpy(&write_time, spare_buf, sizeof(unsigned int));
                memcpy(&lba, spare_buf + sizeof(unsigned int), sizeof(unsigned int));
                if(!time_L2P[lba].contain && write_time > time_L2P[lba].second){
                    time_L2P[lba].pca = pca.pca;
                    time_L2P[lba].contain = true;
                    time_L2P[lba].second = write_time;
                    time_L2P[lba].compare = true;
                }
                else if(time_L2P[lba].contain && !time_L2P[lba].compare) {
                    time_L2P[lba].pca = pca.pca;
                    time_L2P[lba].second = write_time;
                    time_L2P[lba].compare = true;
                }
                else if(time_L2P[lba].contain && time_L2P[lba].compare && write_time > time_L2P[lba].second){
                    time_L2P[lba].pca = pca.pca;
                    time_L2P[lba].second = write_time;
                }

                if(page == 19){
                    //block full
                    block_not_empty = true;
                }
            }
            else if(page == 0){
                //block empty
                block_not_empty = false;
                break;
            }
            else{
                //find current pca
                curr_pca.fields.block = block;
                curr_pca.fields.page = page - 1;
                block_not_empty = false;
                break;
            }
        }
        Block_full[block] = block_not_empty;
    }

    free(spare_buf);
    free(data_buf);
    free(zero_buf);

    for(int lba = 0; lba < LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND; lba++){
        L2P[lba] = time_L2P[lba].pca;
    }
    free(time_L2P);

    if (curr_pca.pca == INVALID_PCA){
        for(int block = 0; block < PHYSICAL_NAND_NUM; block++){
            if(Block_full[block]){
                curr_pca.fields.block = block;
                curr_pca.fields.page = 19;
                break;
            }
        }
    }
}

int main(int argc, char* argv[])
{
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
	nand_write_size = 0;
	host_write_size = 0;
    threshold = 983;
    curr_pca.pca = INVALID_PCA;
    L2P = malloc(LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND * sizeof(int));
    memset(L2P, INVALID_PCA, sizeof(int)*LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND);

    Block_full = malloc(PHYSICAL_NAND_NUM * sizeof(bool));
    memset(Block_full, 0, PHYSICAL_NAND_NUM * sizeof(bool));

    // TODO：if log file exist, yes -> rebuild, no -> create new
    if (!file_exist("nand_0")) {
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
    }
    else if(file_exist("log")){
        rebuild("log");
    }
    else{
        rebuild(NULL);
    }
    
    return fuse_main(argc, argv, &ssd_oper, NULL);
}
