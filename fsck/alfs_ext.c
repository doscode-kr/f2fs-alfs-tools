/*
 *	fs/f2fs/alfs_ext.h
 * 
 *	Copyright (c) 2013 MIT CSAIL
 * 
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 **/

//#include <linux/fs.h>

#include "f2fs.h"
#include "alfs_ext.h"
#define ALFSCODE

void print_hex_memory(void *mem) {
  int i;
  unsigned char *p = (unsigned char *)mem;
  for (i=0; i<4096; i++) {
  	if (0 == p[i])
		printf(" . ");
	else
    	printf("%02x ", p[i]);

    if (i%16==15)
      printf("\n");
  }
  printf("\n");
}

/*
 * Create mapping & summary tables 
 */
static int32_t create_metalog_mapping_table (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_AI (sbi);
	char* page = NULL;
	size_t psize = 0;
	uint32_t i = 0, j = 0;
	uint8_t is_dead_section = 1;
	int32_t ret = 0;
	int32_t NR_MAPPING_ROOMS = 1020;

	/* get the geometry information */
	ri->nr_mapping_phys_blks = NR_MAPPING_SECS * ri->blks_per_sec;
	ri->nr_mapping_logi_blks = ri->nr_metalog_logi_blks / NR_MAPPING_ROOMS;
	if (ri->nr_metalog_logi_blks % NR_MAPPING_ROOMS != 0) {
		ri->nr_mapping_logi_blks++;
	}

	DBG(0, "--------------------------------\n");
	DBG(0, " # of mapping entries: %u\n", ri->nr_metalog_logi_blks);
	DBG(0, " * mapping table b.addr: %u (blk)\n", ri->mapping_blkofs);
	DBG(0, " * mapping table length: %u (blk)\n", ri->nr_mapping_phys_blks);

	/* allocate the memory space for the summary table */
	if ((ri->map_blks = (struct alfs_map_blk*)malloc (
			sizeof (struct alfs_map_blk) * ri->nr_mapping_logi_blks)) == NULL) {
		DBG(0, "Errors occur while allocating memory space for the mapping table\n");
		goto out;
	}
	memset (ri->map_blks, 0x00, sizeof (struct alfs_map_blk) * ri->nr_mapping_logi_blks);

	/* get the free page from the memory pool */
	//page = (char*) malloc (4096);
	psize = 4096;
	//psize = sizeof (struct alfs_map_blk);
	page = (char*) malloc (psize);

	if (!(page)) {
		DBG(0, "Errors occur while allocating page\n");
		goto out;
	}

	/* read the mapping info from the disk */
	ri->mapping_gc_sblkofs = -1;
	ri->mapping_gc_eblkofs = -1;

	/* read the mapping info from the disk */
	for (i = 0; i < NR_MAPPING_SECS; i++) {
		is_dead_section = 1;

		for (j = 0; j < ri->blks_per_sec; j++) {
			//__le32* ptr_page_addr = NULL;
			struct alfs_map_blk* new_map_blk = NULL;

			/* read the mapping data from NAND devices */
			/*printk (KERN_INFO "MR: %lu\n", ri->mapping_blkofs + (i * ri->blks_per_sec) + j);*/
			/*
			DBG(0, "[%d,%d] dev_read at blk(%d), byte addr (%d) \n", i, j
						, ri->mapping_blkofs + (i * ri->blks_per_sec) + j
						, (ri->mapping_blkofs + (i * ri->blks_per_sec + j))*4096);
			*/
			if (dev_read(page, (ri->mapping_blkofs + (i * ri->blks_per_sec + j))*4096, psize) != 0) {

				DBG(0, "Errors occur while reading the mapping data from NAND devices\n");
				ret = -1;
				goto out;
			}

			/* get the virtual address from the page */
			//ptr_page_addr = (__le32*)page_address (page);
			//new_map_blk = (struct alfs_map_blk*)ptr_page_addr;
			new_map_blk = (struct alfs_map_blk*)page;

			/* check version # */
			if (new_map_blk->magic == cpu_to_le32 (0xEF)) {
				uint32_t index = le32_to_cpu (new_map_blk->index);
				// DBG(0, "index: %u (old ver: %u, new ver: %u)\n"
				// 	, index
				// 	, le32_to_cpu (ri->map_blks[index/NR_MAPPING_ROOMS].ver)
				// 	, le32_to_cpu (new_map_blk->ver));


				if (le32_to_cpu (ri->map_blks[index/NR_MAPPING_ROOMS].ver) <= le32_to_cpu (new_map_blk->ver)) {
					// DBG(0, "copy new map blk\n");
					memcpy (&ri->map_blks[index/NR_MAPPING_ROOMS], new_map_blk, psize);
					is_dead_section = 0; /* this section has a valid blk */
				}
			}
			else
			{
				//DBG(0, "magic does not matched ! [%d,%d] dev_read at blk(%d) \n", i, j , ri->mapping_blkofs + (i * ri->blks_per_sec) + j);
			}
		}

		/* is it dead? */
		if (is_dead_section == 1) {
			DBG(0, "dead section detected: %u\n", i);
			if (ri->mapping_gc_eblkofs == -1 && ri->mapping_gc_sblkofs == -1) {
				ri->mapping_gc_eblkofs = i * ri->blks_per_sec;
				ri->mapping_gc_sblkofs = i * ri->blks_per_sec + ri->blks_per_sec;
				ri->mapping_gc_sblkofs = ri->mapping_gc_sblkofs % ri->nr_mapping_phys_blks;
			}
		}
	}

	/* is there a free section for the mapping table? */
	if (ri->mapping_gc_sblkofs == -1 || ri->mapping_gc_eblkofs == -1) {
		DBG(0, "[ERROR] oops! there is no free space for the mapping table\n");
		ret = -1;
	} else {
		DBG(0, "-------------------------------\n");
		DBG(0, "ri->mapping_gc_slbkofs: %u (%u)\n", 
			ri->mapping_gc_sblkofs, ri->mapping_blkofs + ri->mapping_gc_sblkofs);
		DBG(0, "ri->mapping_gc_eblkofs: %u (%u)\n", 
			ri->mapping_gc_eblkofs, ri->mapping_blkofs + ri->mapping_gc_eblkofs);
		DBG(0, "-------------------------------\n");
	}

out:
	/* unlock & free the page */
	// unlock_page (page);
	// __free_pages (page, 0);
	;
	return ret;
}

static int32_t create_metalog_summary_table (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_AI (sbi);
	uint32_t sum_length = 0;
	uint32_t i = 0, j = 0;
	uint8_t is_dead = 1;
	int32_t ret = 0;

	/* get the geometry information */
	sum_length = (sizeof (uint8_t) * ri->nr_metalog_phys_blks + F2FS_BLKSIZE - 1) / F2FS_BLKSIZE;

	DBG(0, "--------------------------------\n");
	DBG(0, " * summary table length: %u\n", sum_length);
	DBG(0, "--------------------------------\n");

	/* allocate the memory space for the summary table */
	if ((ri->summary_table = 
			(uint8_t*)malloc (sum_length * F2FS_BLKSIZE)) == NULL) {
		DBG(0, "Errors occur while allocating memory space for the mapping table\n");
		ret = -1;
		goto out;
	}

	/* set all the entries of the summary table invalid */
	memset (ri->summary_table, 2, sum_length * F2FS_BLKSIZE);

	/* set the entries which are vailid in the mapping valid */
	for (i = 0; i < ri->nr_mapping_logi_blks; i++) {
		for (j = 0; j < 1020; j++) {
			__le32 phyofs = ri->map_blks[i].mapping[j];
			if (le32_to_cpu (phyofs) != -1) {
				/*DBG(0, "summary: set phyofs %u to valid\n", le32_to_cpu (phyofs) - ri->metalog_blkofs);*/
				ri->summary_table[le32_to_cpu (phyofs) - ri->metalog_blkofs] = 1;
			}
		}
	}

	/* search for a section that contains only invalid blks */
	for (i = 0; i < ri->nr_metalog_phys_blks / ri->blks_per_sec; i++) {
		is_dead = 1;
		for (j = 0; j < ri->blks_per_sec; j++) {
			if (ri->summary_table[i*ri->blks_per_sec+j] != 2) {
				is_dead = 0;
				break;
			}
		}
		if (is_dead == 1) {
			ri->metalog_gc_eblkofs = i * ri->blks_per_sec;
			ri->metalog_gc_sblkofs = i * ri->blks_per_sec;
			ri->metalog_gc_sblkofs = (ri->metalog_gc_sblkofs + ri->blks_per_sec) % ri->nr_metalog_phys_blks;

			memset (&ri->summary_table[i*ri->blks_per_sec], 0x00, ri->blks_per_sec);
			break;
		}
	}

	/* metalog must have at least one dead section */
	if (is_dead == 0) {
		DBG(0, "[ERROR] oops! cannot find dead sections in metalog\n");
		ret = -1;
	} else {
		DBG(0, "-------------------------------\n");
		DBG(0, "ri->metalog_gc_sblkofs: %u (%u)\n",
			ri->metalog_gc_sblkofs, ri->metalog_blkofs + ri->metalog_gc_sblkofs);
		DBG(0, "ri->metalog_gc_eblkofs: %u (%u)\n", 
			ri->metalog_gc_eblkofs, ri->metalog_blkofs + ri->metalog_gc_eblkofs);
		DBG(0, "-------------------------------\n");
	}

out:
	return ret;
}


static void destroy_metalog_summary_table (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_AI (sbi);
	if (ri->summary_table) {
		//kfree (ri->summary_table);
		ri->summary_table = NULL;
	}
}

static void destroy_metalog_mapping_table (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_AI (sbi);
	if (ri->map_blks) {
		//kfree (ri->map_blks);
		ri->map_blks = NULL;
	}
}

static void destroy_ri (struct f2fs_sb_info* sbi)
{
	if (sbi->ai) {
		//kfree (sbi->ai);
		sbi->ai = NULL;
	}
}


/* 
 * create the structure for ALFS (ai) 
 */
int32_t alfs_create_ai (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = NULL;
	uint32_t nr_logi_metalog_segments = 0;
	uint32_t nr_phys_metalog_segments = 0;

	/* create alfs_info structure */
	if ((ri = (struct alfs_info*)malloc (
			sizeof (struct alfs_info))) == NULL) {
		DBG(0, "Errors occur while creating alfs_info\n");
		return -1;
	}
	sbi->ai = ri;

	/* initialize some variables */
	ri->mapping_blkofs = get_mapping_blkofs (sbi);
	ri->metalog_blkofs = get_metalog_blkofs (sbi);

	nr_logi_metalog_segments = get_nr_logi_meta_segments (sbi);
	nr_phys_metalog_segments = get_nr_phys_meta_segments (sbi, nr_logi_metalog_segments);

	ri->nr_metalog_logi_blks = SEGS2BLKS (sbi, nr_logi_metalog_segments);
	ri->nr_metalog_phys_blks = SEGS2BLKS (sbi, nr_phys_metalog_segments);

	ri->blks_per_sec = sbi->segs_per_sec * (1 << sbi->log_blocks_per_seg);

	/* create mutex for GC */
	/* display information about metalog */
	DBG(0, "--------------------------------\n");
	DBG(0, " * mapping_blkofs: %u\n", ri->mapping_blkofs);
	DBG(0, " * metalog_blkofs: %u\n", ri->metalog_blkofs);
	DBG(0, " * # of blks per sec: %u\n", ri->blks_per_sec);
	DBG(0, " * # of logical meta-log blks: %u\n", ri->nr_metalog_logi_blks);
	DBG(0, " * # of physical meta-log blks: %u\n", ri->nr_metalog_phys_blks);
	DBG(0, " * the range of logical meta address: %u - %u\n", 
		ri->metalog_blkofs, ri->metalog_blkofs + ri->nr_metalog_logi_blks);
	DBG(0, " * the range of physical meta address: %u - %u\n", 
		ri->metalog_blkofs, ri->metalog_blkofs + ri->nr_metalog_phys_blks);

	return 0;
}

int32_t alfs_build_ai (struct f2fs_sb_info *sbi)
{
	/* see if ri is initialized or not */
	if (sbi == NULL || sbi->ai == NULL) {
		DBG(0, "Error occur because some input parameters are NULL\n");
		return -1;
	}

	/* build meta-log mapping table */
	if (create_metalog_mapping_table (sbi) != 0) {
		DBG(0, "Errors occur while creating the metalog mapping table\n");
		goto error_metalog_mapping;
	}

	/* build meta-log summary table */
	if (create_metalog_summary_table (sbi) != 0) {
		DBG(0, "Errors occur while creating the metalog summary table\n");
		goto error_metalog_summary;
	}
 
	return 0;
 

error_metalog_summary:
	destroy_metalog_mapping_table (sbi);

error_metalog_mapping:

	return -1;
}

void alfs_destory_ai (struct f2fs_sb_info* sbi)
{
 
	destroy_metalog_summary_table (sbi);
	destroy_metalog_mapping_table (sbi);
	destroy_ri (sbi);
}


/*
 * mapping table management 
 */
int32_t get_mapping_free_blks (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_AI (sbi);
	uint32_t nr_free_blks;

	if (ri->mapping_gc_sblkofs < ri->mapping_gc_eblkofs) {
		nr_free_blks = ri->nr_mapping_phys_blks - ri->mapping_gc_eblkofs + ri->mapping_gc_sblkofs;
	} else if (ri->mapping_gc_sblkofs > ri->mapping_gc_eblkofs) {
		nr_free_blks = ri->mapping_gc_sblkofs - ri->mapping_gc_eblkofs;
	} else {
		DBG(0, "[ERROR] 'ri->mapping_gc_sblkofs (%u)' is equal to 'ri->mapping_gc_eblkofs (%u)'\n", 
			ri->mapping_gc_sblkofs, ri->mapping_gc_eblkofs);
		nr_free_blks = -1;
	}

	return nr_free_blks;
}

int8_t is_mapping_gc_needed (struct f2fs_sb_info* sbi, int32_t nr_free_blks)
{
	if (nr_free_blks <= (sbi->segs_per_sec * sbi->blocks_per_seg)) {
		return 0;
	}
	return -1;
}

int8_t alfs_do_mapping_gc (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ai = ALFS_AI (sbi);

	/*
	DBG(0, "before gc\n");
	DBG(0, "-------------------------------\n");
	DBG(0, "ai->mapping_gc_slbkofs: %u (%u)\n", 
		ai->mapping_gc_sblkofs, ai->mapping_blkofs + ai->mapping_gc_sblkofs);
	DBG(0, "ai->mapping_gc_eblkofs: %u (%u)\n", 
		ai->mapping_gc_eblkofs, ai->mapping_blkofs + ai->mapping_gc_eblkofs);
	DBG(0, "-------------------------------\n");
	*/

	/* perform gc */

	/* advance 'mapping_gc_sblkofs' */
	ai->mapping_gc_sblkofs = (ai->mapping_gc_sblkofs + ai->blks_per_sec) % 
		ai->nr_mapping_phys_blks;

	/*
	DBG(0, "after gc\n");
	DBG(0, "-------------------------------\n");
	DBG(0, "ai->mapping_gc_slbkofs: %u (%u)\n", 
		ai->mapping_gc_sblkofs, ai->mapping_blkofs + ai->mapping_gc_sblkofs);
	DBG(0, "ai->mapping_gc_eblkofs: %u (%u)\n", 
		ai->mapping_gc_eblkofs, ai->mapping_blkofs + ai->mapping_gc_eblkofs);
	DBG(0, "-------------------------------\n");
	*/

	return 0;
}

 

/*
 * metalog management 
 */
int32_t is_valid_meta_lblkaddr (struct f2fs_sb_info* sbi, 
	block_t lblkaddr)
{
	struct alfs_info* ri = ALFS_AI (sbi);

	if (sbi->ai == NULL)
		return -1;
	
	if (lblkaddr >= ri->metalog_blkofs &&
		lblkaddr < ri->metalog_blkofs + ri->nr_metalog_logi_blks)
		return 0;

	return -1;
}

int32_t is_valid_meta_pblkaddr (struct f2fs_sb_info* sbi, block_t pblkaddr)
{
	struct alfs_info* ri = ALFS_AI (sbi);

	if (sbi->ai == NULL)
		return -1;
	
	if (pblkaddr >= ri->metalog_blkofs &&
		pblkaddr < ri->metalog_blkofs + ri->nr_metalog_phys_blks)
		return 0;

	return -1;
}

int32_t get_metalog_free_blks (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_AI (sbi);
	uint32_t nr_free_blks;

	if (ri->metalog_gc_sblkofs < ri->metalog_gc_eblkofs) {
		nr_free_blks = ri->nr_metalog_phys_blks - ri->metalog_gc_eblkofs + ri->metalog_gc_sblkofs;
	} else if (ri->metalog_gc_sblkofs > ri->metalog_gc_eblkofs) {
		nr_free_blks = ri->metalog_gc_sblkofs - ri->metalog_gc_eblkofs;
	} else {
		DBG(0, "[ERROR] 'ri->metalog_gc_sblkofs (%u)' is equal to 'ri->metalog_gc_eblkofs (%u)'\n", 
			ri->metalog_gc_sblkofs, ri->metalog_gc_eblkofs);
		nr_free_blks = -1;
	}

	return nr_free_blks;
}


uint32_t alfs_get_mapped_pblkaddr (struct f2fs_sb_info* sbi, block_t lblkaddr)
{
	struct alfs_info* ri = ALFS_AI (sbi);
	block_t pblkaddr;
	block_t new_lblkaddr;

	/* see if ri is initialized or not */
	if (sbi->ai == NULL)
		return NULL_ADDR;

	/* get the physical blkaddr from the mapping table */
	new_lblkaddr = lblkaddr - ri->metalog_blkofs;
	pblkaddr = le32_to_cpu (ri->map_blks[new_lblkaddr/1020].mapping[new_lblkaddr%1020]);
	if (pblkaddr == -1)
		pblkaddr = 0;

	/* see if 'pblkaddr' is valid or not */
	if (is_valid_meta_pblkaddr (sbi, pblkaddr) == -1) {
		if (pblkaddr != NULL_ADDR) {
			DBG(0, "invalid pblkaddr: (%ld (=%ld-%ld) => %d)\n", 
				(int64_t)lblkaddr - (int64_t)ri->metalog_blkofs,
				(int64_t)lblkaddr, 
				(int64_t)ri->metalog_blkofs, 
				pblkaddr);
		}
		return NULL_ADDR;
	}

	/* see if the summary table is correct or not */
	if (ri->summary_table[pblkaddr - ri->metalog_blkofs] == 0 ||
		ri->summary_table[pblkaddr - ri->metalog_blkofs] == 2) {
		DBG(0, "the summary table is incorrect: pblkaddr=%u (%u)\n",
			pblkaddr, ri->summary_table[pblkaddr - ri->metalog_blkofs]);
	}

	return pblkaddr;
}

uint32_t alfs_get_new_pblkaddr (struct f2fs_sb_info* sbi, block_t lblkaddr, uint32_t length)
{
	struct alfs_info* ri = ALFS_AI (sbi);
	block_t pblkaddr = NULL_ADDR;

	/* see if ri is initialized or not */
	if (sbi->ai == NULL)
		return NULL_ADDR;

	/* have sufficent free blks - go ahead */
	if (ri->summary_table[ri->metalog_gc_eblkofs] == 0) {
		/* get the physical blkoff */
		pblkaddr = ri->metalog_blkofs + ri->metalog_gc_eblkofs;

		/* see if pblk is valid or not */
		if (is_valid_meta_pblkaddr (sbi, pblkaddr) == -1) {
			DBG(0, "pblkaddr is invalid (%u)\n", pblkaddr);
			return NULL_ADDR;
		}
	} else {
		DBG(0, "metalog_gc_eblkofs is NOT free: summary_table[%u] = %u\n",
			ri->metalog_gc_eblkofs, ri->summary_table[ri->metalog_gc_eblkofs]);
		return NULL_ADDR;
	}

	return pblkaddr;
}

int8_t alfs_map_l2p (struct f2fs_sb_info* sbi, block_t lblkaddr, block_t pblkaddr, uint32_t length)
{
	struct alfs_info* ri = ALFS_AI (sbi);
	block_t cur_lblkaddr = lblkaddr;
	block_t cur_pblkaddr = pblkaddr;
	block_t new_lblkaddr;
	uint32_t loop = 0;

	/* see if ri is initialized or not */
	if (sbi->ai == NULL)
		return -1;

	/* see if pblkaddr is valid or not */
	if (pblkaddr == NULL_ADDR)
		return -1;

	for (loop = 0; loop < length; loop++) {
		block_t prev_pblkaddr = NULL_ADDR;

		/* see if cur_lblkaddr is valid or not */
		if (is_valid_meta_lblkaddr (sbi, cur_lblkaddr) == -1) {
			DBG(0, "is_valid_meta_lblkaddr is failed (cur_lblkaddr: %u)\n", cur_lblkaddr);
			return -1;
		}

		/* get the new pblkaddr */
		if (cur_pblkaddr == NULL_ADDR) {
			if ((cur_pblkaddr = alfs_get_new_pblkaddr (sbi, cur_lblkaddr, length)) == NULL_ADDR) {
				DBG(0, "cannot get the new free block (cur_lblkaddr: %u)\n", cur_lblkaddr);
				return -1;
			} 
		}

		/* get the old pblkaddr */
		new_lblkaddr = cur_lblkaddr - ri->metalog_blkofs;
		prev_pblkaddr = le32_to_cpu (ri->map_blks[new_lblkaddr/1020].mapping[new_lblkaddr%1020]);
		if (prev_pblkaddr == -1)
			prev_pblkaddr = 0;

		/* see if 'prev_pblkaddr' is valid or not */
		if (is_valid_meta_pblkaddr (sbi, prev_pblkaddr) == 0) {
			/* make the entry of the summary table invalid */
			ri->summary_table[prev_pblkaddr - ri->metalog_blkofs] = 2;	/* set to invalid */

		} else if (prev_pblkaddr != NULL_ADDR) {
			DBG(0, "invalid prev_pblkaddr = %ld\n", (int64_t)prev_pblkaddr);
		} else {
			/* it is porible that 'prev_pblkaddr' is invalid */
		}

		/* update the mapping & summary table */
		new_lblkaddr = cur_lblkaddr - ri->metalog_blkofs;
		ri->map_blks[new_lblkaddr/1020].mapping[new_lblkaddr%1020] = cpu_to_le32 (cur_pblkaddr);
		ri->map_blks[new_lblkaddr/1020].dirty = 1;

		ri->summary_table[cur_pblkaddr - ri->metalog_blkofs] = 1; /* set to valid */

		/* adjust end_blkofs in the meta-log */
		ri->metalog_gc_eblkofs = (ri->metalog_gc_eblkofs + 1) % (ri->nr_metalog_phys_blks);

		/* go to the next logical blkaddr */
		cur_lblkaddr++;
		cur_pblkaddr = NULL_ADDR;
	}

	return 0;
}


uint32_t alfs_get_mapping_info(struct f2fs_sb_info* sbi, uint32_t src){
	int32_t NR_MAPPING_ROOMS = 1020;
	struct alfs_info* ri = ALFS_AI (sbi);
	int32_t origin = src - ri->metalog_blkofs;
	int32_t loc = origin / NR_MAPPING_ROOMS;
	int32_t off = origin % NR_MAPPING_ROOMS;
	uint32_t dest = ri->map_blks[loc].mapping[off];
	if(dest != -1)
		DBG(0, " src(%u) => dest(%u)\n", src, dest);
	return dest;
}

int alfs_dev_read_block(struct f2fs_sb_info* sbi, void* page, block_t blk_ofs){
	block_t addr_to_read;
#ifdef ALFSCODE	
	addr_to_read = alfs_get_mapping_info(sbi, blk_ofs);
	if (addr_to_read == -1){
		//DBG(0, "address_to_read == -1, not mapped.\n");
		memset(page, 0, F2FS_BLKSIZE);	
		return F2FS_BLKSIZE;
	}
	else
		return dev_read_block(page, addr_to_read);
#else
	addr_to_read = blk_ofs;
	return dev_read_block(page, addr_to_read);
#endif	
}