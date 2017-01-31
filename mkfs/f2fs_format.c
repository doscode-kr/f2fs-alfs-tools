/**
 * f2fs_format.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * Dual licensed under the GPL or LGPL version 2 licenses.
 */
#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <time.h>
#include <uuid/uuid.h>

#include "f2fs_fs.h"
#include "f2fs_format_utils.h"

/* chamdoo - 2013.09.18 */
#define ENABLE_DBG_LOG
#define ALFS_SNAPSHOT
#define ALFS_META_LOGGING
/*#define ALFS_LARGE_SEGMENT*/

extern struct f2fs_configuration config;
struct f2fs_super_block raw_sb;
struct f2fs_super_block *sb = &raw_sb;
struct f2fs_checkpoint *cp;

/* Return first segment number of each area */
#define prev_zone(cur)		(config.cur_seg[cur] - config.segs_per_zone)
#define next_zone(cur)		(config.cur_seg[cur] + config.segs_per_zone)
#define last_zone(cur)		((cur - 1) * config.segs_per_zone)
#define last_section(cur)	(cur + (config.secs_per_zone - 1) * config.segs_per_sec)

const char *media_ext_lists[] = {
	"jpg",
	"gif",
	"png",
	"avi",
	"divx",
	"mp4",
	"mp3",
	"3gp",
	"wmv",
	"wma",
	"mpeg",
	"mkv",
	"mov",
	"asx",
	"asf",
	"wmx",
	"svi",
	"wvx",
	"wm",
	"mpg",
	"mpe",
	"rm",
	"ogg",
	"jpeg",
	"video",
	"apk",	/* for android system */
	NULL
};

#if defined(ALFS_SNAPSHOT) && defined(ALFS_META_LOGGING)
#define NR_SUPERBLK_SECS	1	/* # of sections for the super block */
#define NR_MAPPING_SECS		3 	/* # of sections for mapping entries */
#define NR_METALOG_TIMES	2	/* # of sections for meta-log */
#define NR_MAPPING_ROOMS 	1020 /* # of rooms for mapping on 4K page (block)*/
struct alfs_map_blk {
	__le32 magic;
	__le32 ver;
	__le32 index;
	__le32 dirty;
	__le32 mapping[F2FS_BLKSIZE/sizeof(__le32)-4];
};

struct alfs_map_blk* _ptr_map_blks = NULL;
uint64_t _meta_logging_index = 0;
//unsigned long long start = alfs_get_starting_blk_ofs();		// 2048, for 1TB case  with ALFS

void alfs_init_meta_log_blk_ofs (uint64_t value){
	_meta_logging_index = value;
	DBG(1, "init _meta_log_blk_ofs (starting offset ) = (%lu)\n", _meta_logging_index);
}
static inline void alfs_add_meta_logging_index(){
	_meta_logging_index++;
}
static inline uint64_t alfs_get_starting_blk_ofs(){
	uint64_t start_ofs = (config.segs_per_sec * config.blks_per_seg) * (NR_SUPERBLK_SECS + NR_MAPPING_SECS);
	return start_ofs;
}

static inline uint64_t alfs_get_meta_logging_index(){
	if (_meta_logging_index == 0 )
	{
		_meta_logging_index = alfs_get_starting_blk_ofs();
	}
	return _meta_logging_index;
}


inline void alfs_free_mapping_info_table(){
	free (_ptr_map_blks);
}

// Init mapping info table at the global variable.
void alfs_init_mapping_info_table(){
	uint32_t nr_segment_count_meta = 0;
	uint32_t nr_meta_blks = 0;
	uint32_t nr_mapping_blks = 0;
	uint32_t loop = 0;

	DBG(1, "start alfs_init_mapping_info \n");


	/* (1) obtain the number of mapping entries.
	 * for aroud 1TB cap., it is around 1017. (depends on the cap.)
	 * Mapping entry = # seg * # blks.
	 * A 4K page block can save 1020 mapping informations. for 1TB, aroud 511 mapping blks are required.
	 * */
	nr_segment_count_meta =
					get_sb(segment_count_ckpt) +
					get_sb(segment_count_sit) +
					get_sb(segment_count_nat) +
					get_sb(segment_count_ssa);
	nr_meta_blks = nr_segment_count_meta * config.blks_per_seg;

	nr_mapping_blks = nr_meta_blks / NR_MAPPING_ROOMS;

	if (nr_meta_blks % NR_MAPPING_ROOMS != 0) {
		nr_mapping_blks++;
	}
	DBG(1, "\n----\n");
	DBG(1, "nr_segment_count_meta: %u\n", nr_segment_count_meta);
	DBG(1, "nr_meta_blks: %u \n", nr_meta_blks);
	DBG(1, "nr_mapping_blks: %u\n", nr_mapping_blks);

	/* (2) create mapping table
	 * Allocate memories and init them by memset.
	 * REMEMBER THAT, A 4K page block can save 1020 mapping informations.
	 */
	_ptr_map_blks = (struct alfs_map_blk*)malloc (sizeof (struct alfs_map_blk) * nr_mapping_blks);
	if (_ptr_map_blks == NULL) {
		DBG(0, "\tError: errors occur allocating memory space for the map-blk table\n");
		return ;
	}
	for (loop = 0; loop < nr_mapping_blks; loop++) {
		_ptr_map_blks[loop].magic = cpu_to_le32 (0xEF);
		_ptr_map_blks[loop].ver = cpu_to_le32(0);
		_ptr_map_blks[loop].index = cpu_to_le32 (loop * NR_MAPPING_ROOMS);
		_ptr_map_blks[loop].dirty = cpu_to_le32(0);
		memset (_ptr_map_blks[loop].mapping, (__le32)-1, sizeof (__le32) * NR_MAPPING_ROOMS);
	}
}
/*
 * Save mapping informations to the global variable `_ptr_map_blks`
 * This function is called every right before `dev_write` related in meta data logging.
 */
void alfs_set_mapping_info(uint64_t src){
	uint64_t dest = alfs_get_meta_logging_index();
	uint64_t map_location = src - alfs_get_starting_blk_ofs();
	uint32_t loc = map_location/NR_MAPPING_ROOMS;
	uint32_t off = map_location%NR_MAPPING_ROOMS;
	DBG(1, " origin(%lu) saved at map [%u,%u] as (%lu)\n", src, loc, off, dest);

	_ptr_map_blks[loc].mapping[off] = dest;
}

/*
 * utility function to check content in byte level.
 */
static void print_hex_memory(void *mem) {
	int i;
	unsigned char *p = (unsigned char *)mem;
	for (i=1;i<=4096;i++) {
		if (0 == p[i])
		printf(" . ");
	else
		printf("%02x ", p[i]);

	if (i%16==0)
	printf("\n");
	}
	printf("\n");
}
#endif

static int dev_write_meta_block(void* buf, uint64_t blk_addr, uint32_t blk_size){
#if defined(ALFS_SNAPSHOT) && defined(ALFS_META_LOGGING)
	if (dev_write(buf, alfs_get_meta_logging_index() * blk_size, blk_size)) {
		return -1;
	}
	alfs_set_mapping_info(blk_addr);
	alfs_add_meta_logging_index();
#else
	if (dev_write(buf, blk_addr * blk_size, blk_size)) {
		return -1;
	}
#endif
	return 0;
}

/* chamdoo (2013.09.19) */
/* dos (2016.11.04) */
static inline u_int64_t cal_zone_align_start_offset (u_int32_t blk_size_bytes, u_int32_t segment_size_bytes)
{
	u_int32_t zone_size_bytes;
	u_int64_t zone_align_start_offset;

	zone_size_bytes =
		blk_size_bytes * config.secs_per_zone *
		config.segs_per_sec * config.blks_per_seg;
#ifdef ALFS_SNAPSHOT
	zone_align_start_offset =
		(config.start_sector * DEFAULT_SECTOR_SIZE +
		2 * F2FS_BLKSIZE + zone_size_bytes - 1) /
		zone_size_bytes * zone_size_bytes -
		config.start_sector * DEFAULT_SECTOR_SIZE;

	DBG(0, "Info: before zone_align_start_offset: (%" PRIu64 ")\n", zone_align_start_offset);
	DBG(1, "segment0_blkaddr.org: %u\n",
		cpu_to_le32(zone_align_start_offset / blk_size_bytes));
	zone_align_start_offset =
		segment_size_bytes * cpu_to_le32(config.segs_per_sec) *
		(NR_SUPERBLK_SECS + NR_MAPPING_SECS);	/* snapshot region */

	DBG(0, "Info: after zone_align_start_offset: (%" PRIu64 ")\n", zone_align_start_offset);
#else

	zone_align_start_offset =
		(config.start_sector * config.sector_size +
		2 * F2FS_BLKSIZE + zone_size_bytes - 1) /
		zone_size_bytes * zone_size_bytes -
		config.start_sector * config.sector_size;
#endif

	DBG(1, "-------------------zone_align_start_offset------------------------------\n");
	DBG(1, "config.start_sector(%u)\n", config.start_sector);
	DBG(1, "config.sector_size(%u)\n", config.sector_size);
	DBG(1, "zone_size_bytes(%u)\n", zone_size_bytes);
	DBG(1, "------------------------------------------------------------------------\n");

	return zone_align_start_offset;

}

static inline u_int32_t  get_max_nat_segments(u_int32_t log_blks_per_seg)
{
	u_int32_t sit_bitmap_size;
	u_int32_t max_nat_bitmap_size, max_nat_segments;
	u_int32_t tmp;
/*
	 * The number of node segments should not be exceeded a "Threshold".
	 * This number resizes NAT bitmap area in a CP page.
	 * So the threshold is determined not to overflow one CP page
	 */
	sit_bitmap_size = ((get_sb(segment_count_sit) / 2) << log_blks_per_seg) / 8;
	sit_bitmap_size = (sit_bitmap_size > MAX_SIT_BITMAP_SIZE)? MAX_SIT_BITMAP_SIZE : sit_bitmap_size;

	/*
	 * It should be reserved minimum 1 segment for nat.
	 * When sit is too large, we should expand cp area. It requires more pages for cp.
	 */
	tmp = CHECKSUM_OFFSET - sizeof(struct f2fs_checkpoint) + 1;
	DBG(0, "Info: MAX_SIT_BITMAP_SIZE (%lu) \n", MAX_SIT_BITMAP_SIZE);
	DBG(0, "Info: sizeof(struct f2fs_checkpoint) (%lu) \n", sizeof(struct f2fs_checkpoint));
	DBG(0, "Info: sit_bitmap_size(%u), compared to (%u) \n", sit_bitmap_size, (tmp - 64));

	if (sit_bitmap_size > (tmp - 64)) {
		DBG(0, "Info: WE SHOULD EXPAND CP AREA \n");
		max_nat_bitmap_size = tmp;
		set_sb(cp_payload, F2FS_BLK_ALIGN(sit_bitmap_size));
	} else {
		max_nat_bitmap_size = tmp - sit_bitmap_size;
		set_sb(cp_payload, 0);
	}

	max_nat_segments = (max_nat_bitmap_size * 8) >> log_blks_per_seg;
	DBG(0, "Info: get_sb(segment_count_nat)  (%u) \n", get_sb(segment_count_nat) );
	DBG(0, "Info: max_nat_segments (%u) \n", max_nat_segments);

	return max_nat_segments;
}

static bool is_extension_exist(const char *name)
{
	int i;

	for (i = 0; i < F2FS_MAX_EXTENSION; i++) {
		char *ext = (char *)sb->extension_list[i];
		if (!strcmp(ext, name))
			return 1;
	}

	return 0;
}

static void configure_extension_list(void)
{
	const char **extlist = media_ext_lists;
	char *ext_str = config.extension_list;
	char *ue;
	int name_len;
	int i = 0;

	set_sb(extension_count, 0);
	memset(sb->extension_list, 0, sizeof(sb->extension_list));

	while (*extlist) {
		name_len = strlen(*extlist);
		memcpy(sb->extension_list[i++], *extlist, name_len);
		extlist++;
	}
	set_sb(extension_count, i);

	if (!ext_str)
		return;

	/* add user ext list */
	ue = strtok(ext_str, ", ");
	while (ue != NULL) {
		name_len = strlen(ue);
		if (name_len >= 8) {
			MSG(0, "\tWarn: Extension name (%s) is too long\n", ue);
			goto next;
		}
		if (!is_extension_exist(ue))
			memcpy(sb->extension_list[i++], ue, name_len);
next:
		ue = strtok(NULL, ", ");
		if (i >= F2FS_MAX_EXTENSION)
			break;
	}

	set_sb(extension_count, i);

	free(config.extension_list);
}

static int f2fs_prepare_super_block(void)
{
	u_int32_t blk_size_bytes;
	u_int32_t log_sectorsize, log_sectors_per_block;
	u_int32_t log_blocksize, log_blks_per_seg;
	u_int32_t segment_size_bytes;
	u_int32_t sit_segments;
	u_int32_t blocks_for_sit, blocks_for_nat, blocks_for_ssa;
	u_int32_t total_valid_blks_available;
	u_int64_t zone_align_start_offset, diff;
	u_int64_t total_meta_zones, total_meta_segments;
	u_int32_t max_nat_segments;
	u_int32_t total_zones;

/* chamdoo - 2013.09.18 */
#ifdef ALFS_SNAPSHOT
	u_int32_t nr_meta_logging_zones = 0;
	u_int32_t nr_meta_logging_segments = 0;
	u_int32_t nr_meta_logging_blks = 0;
#endif

	set_sb(magic, F2FS_SUPER_MAGIC);
	set_sb(major_ver, F2FS_MAJOR_VERSION);
	set_sb(minor_ver, F2FS_MINOR_VERSION);

	log_sectorsize = log_base_2(config.sector_size);
	log_sectors_per_block = log_base_2(config.sectors_per_blk);
	log_blocksize = log_sectorsize + log_sectors_per_block;
	log_blks_per_seg = log_base_2(config.blks_per_seg);

	set_sb(log_sectorsize, log_sectorsize);
	set_sb(log_sectors_per_block, log_sectors_per_block);

	set_sb(log_blocksize, log_blocksize);
	DBG(1, "config.blks_per_seg = %d\n", config.blks_per_seg);
	DBG(1, "log_blks_per_seg = %d\n", log_blks_per_seg);
	set_sb(log_blocks_per_seg, log_blks_per_seg);

	set_sb(segs_per_sec, config.segs_per_sec);
	set_sb(secs_per_zone, config.secs_per_zone);

	set_sb(checksum_offset, 0);
	set_sb(block_count, config.total_sectors >> log_sectors_per_block);

	blk_size_bytes = 1 << log_blocksize;
	segment_size_bytes = blk_size_bytes * config.blks_per_seg;

	DBG(1, "config.start_sector: %u\n"
		"DEFAULT_SECTOR_SIZE: %u\n"
		"F2FS_BLKSIZE: %u\n"
		"segment_size_bytes: %u\n",
		config.start_sector,
		DEFAULT_SECTOR_SIZE,
		F2FS_BLKSIZE,
		segment_size_bytes);

	zone_align_start_offset = cal_zone_align_start_offset (blk_size_bytes, segment_size_bytes);

	DBG(1, "segment0_blkaddr.org: %u\n",
		cpu_to_le32(zone_align_start_offset / blk_size_bytes));

	if (config.start_sector % config.sectors_per_blk) {
		MSG(1, "\tWARN: Align start sector number to the page unit\n");
		MSG(1, "\ti.e., start sector: %d, ofs:%d (sects/page: %d)\n",
				config.start_sector,
				config.start_sector % config.sectors_per_blk,
				config.sectors_per_blk);
	}

	set_sb(segment_count, (config.total_sectors * config.sector_size - zone_align_start_offset) / segment_size_bytes
							 / config.segs_per_zone * config.segs_per_zone);

	set_sb(segment0_blkaddr, zone_align_start_offset / blk_size_bytes);

	DBG(1, "-----------------set sb segment0_blkaddr--------------------------------\n");
	MSG(0, "Info: zone aligned segment0 blkaddr: %u\n", get_sb(segment0_blkaddr));
	DBG(1, "----------------------log_blocksize-------------------------------------\n");
	DBG(1, "log_blocksize(%u)\n", 1<<log_blocksize);

	/* cp blk addr = segment 0's blk addr */
	sb->cp_blkaddr = sb->segment0_blkaddr;
	set_sb(segment_count_ckpt, F2FS_NUMBER_OF_CHECKPOINT_PACK);

	/* sit blk addr = cp blk addr + cp size */
	set_sb(sit_blkaddr, get_sb(segment0_blkaddr) + (get_sb(segment_count_ckpt) * config.blks_per_seg));
	blocks_for_sit = ALIGN(get_sb(segment_count), SIT_ENTRY_PER_BLOCK);
	sit_segments = SEG_ALIGN(blocks_for_sit);
	set_sb(segment_count_sit, sit_segments * 2);

	/* nat blk addr = sit blk addr + sit size */
	set_sb(nat_blkaddr, get_sb(sit_blkaddr) + (get_sb(segment_count_sit) * config.blks_per_seg) );

	total_valid_blks_available = (get_sb(segment_count) - (get_sb(segment_count_ckpt) + get_sb(segment_count_sit)))
								 * config.blks_per_seg;
	blocks_for_nat = ALIGN(total_valid_blks_available, NAT_ENTRY_PER_BLOCK);
	set_sb(segment_count_nat, SEG_ALIGN(blocks_for_nat));

	max_nat_segments = get_max_nat_segments(log_blks_per_seg);
	if (get_sb(segment_count_nat) > max_nat_segments)
		set_sb(segment_count_nat, max_nat_segments);

	set_sb(segment_count_nat, get_sb(segment_count_nat) * 2);

	/* ssa blk addr = nat blk addr + nat size */
	set_sb(ssa_blkaddr, get_sb(nat_blkaddr) + (get_sb(segment_count_nat) * config.blks_per_seg) );

	total_valid_blks_available = (get_sb(segment_count) -
						(get_sb(segment_count_ckpt) + get_sb(segment_count_sit) + get_sb(segment_count_nat)) )
						* config.blks_per_seg;
	blocks_for_ssa = total_valid_blks_available / config.blks_per_seg + 1;
	set_sb(segment_count_ssa, SEG_ALIGN(blocks_for_ssa));
	DBG(1, "total_valid_blks_available(%u)\n",total_valid_blks_available);
	DBG(1, "blocks_for_ssa(%u)\n",blocks_for_ssa);
	DBG(1, "segment_count_ssa(%u)\n",get_sb(segment_count_ssa));



	total_meta_segments = get_sb(segment_count_ckpt) +
						get_sb(segment_count_sit) +
						get_sb(segment_count_nat) +
						get_sb(segment_count_ssa);

	diff = total_meta_segments % (config.segs_per_zone);
	if (diff){
		set_sb(segment_count_ssa, get_sb(segment_count_ssa) + (config.segs_per_zone - diff));
		total_meta_segments += (config.segs_per_zone - diff);
	}

	total_meta_zones = ZONE_ALIGN(total_meta_segments * config.blks_per_seg);

/*
	chamdoo (2013.09.19), dos (2016.10.18)
 */
#ifdef ALFS_SNAPSHOT
	/* meta-log region */
	if (NR_METALOG_TIMES % 2 != 0) {
		DBG(0, "ERROR: NR_METALOG_TIMES must be even numbers = %u\n", NR_METALOG_TIMES);
		exit (-1);
	}

	nr_meta_logging_zones = total_meta_zones * (NR_METALOG_TIMES - 1);
	nr_meta_logging_segments = total_meta_segments * (NR_METALOG_TIMES - 1);
	nr_meta_logging_blks = (nr_meta_logging_segments * config.blks_per_seg);

	 DBG (1, "nr_meta_logging_zones: %u\n", nr_meta_logging_zones);
	 DBG (1, "nr_meta_logging_segments: %u, nr_meta_logging_blks: %u (%u*%u = %u)\n",
		nr_meta_logging_segments,
		nr_meta_logging_blks,
		nr_meta_logging_segments,
		config.blks_per_seg,
		nr_meta_logging_segments*config.blks_per_seg*4096
		);
#endif

/*
 chamdoo (2013.09.19), dos (2016.10.18)
 */
#ifdef ALFS_SNAPSHOT
	set_sb(main_blkaddr, get_sb(segment0_blkaddr)
						+ (total_meta_zones * config.segs_per_zone * config.blks_per_seg)
						+ nr_meta_logging_blks /* added for meta-logging */
						);

	total_zones = get_sb(segment_count) / (config.segs_per_zone) - (total_meta_zones + nr_meta_logging_zones);
#else
	set_sb(main_blkaddr, get_sb(segment0_blkaddr)
						+ total_meta_zones * config.segs_per_zone * config.blks_per_seg
						);

	total_zones = get_sb(segment_count) / (config.segs_per_zone) - total_meta_zones;
#endif
/*
	End of adding alfs codes.
*/
	set_sb(section_count, total_zones * config.secs_per_zone);
	set_sb(segment_count_main, get_sb(section_count) * config.segs_per_sec);


	DBG (1, "BEFORE config.overprovision: %f\n", config.overprovision);
	DBG (1, "BEFORE config.reserved_segments: %u\n", config.reserved_segments);
	/* Let's determine the best reserved and overprovisioned space */
	if (config.overprovision == 0)
		config.overprovision = get_best_overprovision(sb);

	config.reserved_segments = (2 * (100 / config.overprovision + 1) + 6)
								* config.segs_per_sec;

	DBG (1, "AFTER config.overprovision: %f\n", config.overprovision);
	DBG (1, "AFTER config.reserved_segments: %u\n", config.reserved_segments);
	/*
		dos (2016.11.21)
		I don't understand what this code means.
	 */
#ifdef ALFS_SNAPSHOT
	/*config.reserved_segments  = config.segs_per_sec * (2 + 6); *//* for postmark & tpc-c */
	config.reserved_segments  = config.segs_per_sec * 2; /* for postmark & tpc-c */
#else
	config.reserved_segments  = config.segs_per_sec * 10;
#endif

	if ((get_sb(segment_count_main) - 2) <
					config.reserved_segments) {
		MSG(1, "\tError: Device size is not sufficient for F2FS volume, "
			"more segment needed =%u",
			config.reserved_segments -
			(get_sb(segment_count_main) - 2));
		return -1;
	}

	uuid_generate(sb->uuid);

	utf8_to_utf16(sb->volume_name, (const char *)config.vol_label,
				MAX_VOLUME_NAME, strlen(config.vol_label));
	set_sb(node_ino, 1);
	set_sb(meta_ino, 2);
	set_sb(root_ino, 3);

	DBG(1, "total_meta_zones: %" PRIu64 "\n", total_meta_zones);
	DBG(1, "total_zones: %u\n", total_zones);
	DBG(1, "config.segs_per_sec: %u = 1?\n", config.segs_per_sec);
	DBG(1, "config.secs_per_zone: %u = 1?\n", config.secs_per_zone);

	if (total_zones <= 6) {
		MSG(1, "\tError: %d zones: Need more zones "
			"by shrinking zone size\n", total_zones);
		return -1;
	}

	DBG(1, "config.heap: %u\n", config.heap);

	if (config.heap) {
		config.cur_seg[CURSEG_HOT_NODE] = last_section(last_zone(total_zones));
		config.cur_seg[CURSEG_WARM_NODE] = prev_zone(CURSEG_HOT_NODE);
		config.cur_seg[CURSEG_COLD_NODE] = prev_zone(CURSEG_WARM_NODE);
		config.cur_seg[CURSEG_HOT_DATA] = prev_zone(CURSEG_COLD_NODE);
		config.cur_seg[CURSEG_COLD_DATA] = 0;
		config.cur_seg[CURSEG_WARM_DATA] = next_zone(CURSEG_COLD_DATA);
	} else {
		config.cur_seg[CURSEG_HOT_NODE] = 0;
		config.cur_seg[CURSEG_WARM_NODE] = next_zone(CURSEG_HOT_NODE);
		config.cur_seg[CURSEG_COLD_NODE] = next_zone(CURSEG_WARM_NODE);
		config.cur_seg[CURSEG_HOT_DATA] = next_zone(CURSEG_COLD_NODE);
		config.cur_seg[CURSEG_COLD_DATA] = next_zone(CURSEG_HOT_DATA);
		config.cur_seg[CURSEG_WARM_DATA] = next_zone(CURSEG_COLD_DATA);
	}

	DBG(1, "config.cur_seg[CURSEG_HOT_NODE](%u)\n", last_section(last_zone(total_zones)));
	DBG(1, "-------------------------------------------------\n");
	DBG(1, "get_sb(segment0_blkaddr)(%u)\n", get_sb(segment0_blkaddr));
	DBG(1, "get_sb(cp_blkaddr)(%u)\n", get_sb(cp_blkaddr));
	DBG(1, "get_sb(sit_blkaddr)(%u)\n", get_sb(sit_blkaddr));
	DBG(1, "get_sb(nat_blkaddr)(%u)\n", get_sb(nat_blkaddr));
	DBG(1, "get_sb(ssa_blkaddr)(%u)\n", get_sb(ssa_blkaddr));
	DBG(1, "get_sb(main_blkaddr)(%u)\n", get_sb(main_blkaddr));
	DBG(1, "get_sb(segment_count_ckpt)(%u)\n", get_sb(segment_count_ckpt));
	DBG(1, "get_sb(segment_count_sit)(%u)\n", get_sb(segment_count_sit));
	DBG(1, "get_sb(segment_count_nat)(%u)\n", get_sb(segment_count_nat));
	DBG(1, "get_sb(segment_count_ssa)(%u)\n", get_sb(segment_count_ssa));
	DBG(1, "get_sb(segment_count_main)(%u)\n", get_sb(segment_count_main));
	DBG(1, "get_sb(segment_count)(%u)\n", get_sb(segment_count));
	DBG(1, "get_sb(log_blocks_per_seg)(%u)\n", get_sb(log_blocks_per_seg));
#ifdef ALFS_SNAPSHOT
	DBG(1, "nr_meta_logging_segments(%u)\n", nr_meta_logging_segments);
#endif
	DBG(1, "-------------------------------------------------\n");

	configure_extension_list();

	/* get kernel version */
	if (config.kd >= 0) {
		dev_read_version(config.version, 0, VERSION_LEN);
		get_kernel_version(config.version);
		MSG(0, "Info: format version with\n  \"%s\"\n", config.version);
	} else {
		memset(config.version, 0, VERSION_LEN);
	}

	memcpy(sb->version, config.version, VERSION_LEN);
	memcpy(sb->init_version, config.version, VERSION_LEN);

	sb->feature = config.feature;

	return 0;
}

static int f2fs_init_sit_area(void)
{
	u_int32_t blk_size, seg_size;
	u_int32_t index = 0;
	u_int64_t sit_seg_addr = 0;
	u_int8_t *zero_buf = NULL;

	blk_size = 1 << get_sb(log_blocksize);
	seg_size = (1 << get_sb(log_blocks_per_seg)) * blk_size;

	zero_buf = calloc(sizeof(u_int8_t), seg_size);
	if(zero_buf == NULL) {
		MSG(1, "\tError: Calloc Failed for sit_zero_buf!!!\n");
		return -1;
	}

	sit_seg_addr = get_sb(sit_blkaddr);
	sit_seg_addr *= blk_size;

#ifdef ALFS_SNAPSHOT
	DBG(1, "sit_start_addr: get_sb(sit_blkaddr): %u \n",
			get_sb(sit_blkaddr));
	DBG(1, "sit_length: get_sb(segment_count_sit): %u\n",
			get_sb(segment_count_sit));
#endif

	DBG(1, "\tFilling sit area at offset 0x%08"PRIx64"\n", sit_seg_addr);
	for (index = 0; index < (get_sb(segment_count_sit) / 2); index++) {

#ifndef ALFS_SNAPSHOT
		// if (dev_write(zero_buf, sit_seg_addr, seg_size)) {
		// 	MSG(0, "\tError: While zeroing out the sit area on disk!!!\n");
		// 	free(zero_buf);
		// 	return -1;
		// }
#endif
		sit_seg_addr += seg_size;
	}

	free(zero_buf);
	return 0 ;
}

static int f2fs_init_nat_area(void)
{
	uint32_t blk_size, seg_size;
	uint32_t index = 0;
	uint64_t nat_seg_addr = 0;
	uint8_t *nat_buf = NULL;

	blk_size = 1 << get_sb(log_blocksize);
	seg_size = (1 << get_sb(log_blocks_per_seg)) * blk_size;

	nat_buf = calloc(sizeof(u_int8_t), seg_size);
	if (nat_buf == NULL) {
		MSG(0, "\tError: Calloc Failed for nat_zero_blk!!!\n");
		return -1;
	}

	nat_seg_addr = get_sb(nat_blkaddr);
	nat_seg_addr *= blk_size;

#ifdef ALFS_SNAPSHOT
	DBG(1, "nat_start_addr: get_sb(nat_blkaddr): %u\n", get_sb(nat_blkaddr));
	DBG(1, "nat_length: get_sb(segment_count_nat): %d\n",  get_sb(segment_count_nat));
#endif

	DBG(1, "\tFilling nat area at offset 0x%08"PRIx64"\n", nat_seg_addr);
	for (index = 0; index < get_sb(segment_count_nat) / 2; index++) {

#ifndef ALFS_SNAPSHOT
		// if (dev_write(nat_buf, nat_seg_addr, seg_size)) {
		// 	MSG(0, "\tError: While zeroing out the nat area on disk!!!\n");
		// 	free(nat_buf);
		// 	return -1;
		// }
#endif
		nat_seg_addr = nat_seg_addr + (2 * seg_size);
	}

	free(nat_buf);
	return 0 ;
}

/* cp page 2 of check point pack 2
 */
static uint64_t f2fs_write_check_point_pack_page2_pack2(uint64_t cp_seg_blk_offset, uint32_t blk_size)
{
	/* cp page 2 of check point pack 2 */
	cp_seg_blk_offset += le32_to_cpu(cp->cp_pack_total_block_count) - get_sb(cp_payload) - 1;
	DBG(1, "\tWriting cp page2 pack#2\n");

	if(dev_write_meta_block(cp, cp_seg_blk_offset, blk_size))
	{
		MSG(0, "\tError: While writing the cp page2 pack#2 to disk!!!\n");
		return -1;
	}

	return cp_seg_blk_offset;
}

/* cp page 1 of check point pack 2
 * Initiatialize other checkpoint pack with version zero
 */
static uint64_t f2fs_write_check_point_pack_page1_pack2(uint64_t cp_seg_blk_offset, uint32_t blk_size)
{
	uint32_t crc = 0;
	cp->checkpoint_ver = 0;

	crc = f2fs_cal_crc32(F2FS_SUPER_MAGIC, cp, CHECKSUM_OFFSET);
	*((__le32 *)((unsigned char *)cp + CHECKSUM_OFFSET)) =
							cpu_to_le32(crc);
	cp_seg_blk_offset = get_sb(segment0_blkaddr) + config.blks_per_seg;
	DBG(1, "\tWriting cp page1 pack#2, at %lu b.addr \n", cp_seg_blk_offset);

	if(dev_write_meta_block(cp, cp_seg_blk_offset, blk_size))
	{
		MSG(0, "\tError: While writing the cp page1 pack#2 to disk!!!\n");
		return -1;
	}

	return cp_seg_blk_offset;
}

/* cp page 2 of check point pack 1 */
static uint64_t f2fs_write_check_point_pack_page2_pack1(uint64_t cp_seg_blk_offset, uint32_t blk_size)
{

	cp_seg_blk_offset += 1;
	DBG(1, "\tWriting cp page2 pack#1, at %lu b.addr \n", cp_seg_blk_offset);

	if(dev_write_meta_block(cp, cp_seg_blk_offset, blk_size))
	{
		MSG(0, "\tError: While writing the cp page2 pack#1 to disk!!!\n");
		return -1;
	}

	return cp_seg_blk_offset;
}

/* hot/warm/cold data summary */
static uint64_t f2fs_write_check_point_pack_sum_hwc_data(uint64_t cp_seg_blk_offset, uint32_t blk_size)
{
	struct f2fs_summary_block *sum = NULL;
	struct f2fs_journal *journal;
	char *sum_compact, *sum_compact_p;
	struct f2fs_summary *sum_entry;

	sum = calloc(F2FS_BLKSIZE, 1);
	if (sum == NULL) {
		MSG(0, "\tError: Calloc Failed for summay_node!!!\n");
		free(sum);
		return -1;
	}

	sum_compact = calloc(F2FS_BLKSIZE, 1);
	if (sum_compact == NULL) {
		MSG(0, "\tError: Calloc Failed for summay buffer!!!\n");
		free(sum_compact);
		free(sum);
		return -1;
	}

	sum_compact_p = sum_compact;

	memset(sum, 0, sizeof(struct f2fs_summary_block));
	SET_SUM_TYPE((&sum->footer), SUM_TYPE_DATA);

	journal = &sum->journal;
	journal->n_nats = cpu_to_le16(1);
	journal->nat_j.entries[0].nid = sb->root_ino;
	journal->nat_j.entries[0].ne.version = 0;
	journal->nat_j.entries[0].ne.ino = sb->root_ino;
	journal->nat_j.entries[0].ne.block_addr = cpu_to_le32(
			get_sb(main_blkaddr) +
			get_cp(cur_node_segno[0]) * config.blks_per_seg);

	memcpy(sum_compact_p, &journal->n_nats, SUM_JOURNAL_SIZE);
	sum_compact_p += SUM_JOURNAL_SIZE;

	memset(sum, 0, sizeof(struct f2fs_summary_block));
	/* inode sit for root */
	journal->n_sits = cpu_to_le16(6);
	journal->sit_j.entries[0].segno = cp->cur_node_segno[0];
	journal->sit_j.entries[0].se.vblocks = cpu_to_le16((CURSEG_HOT_NODE << 10) | 1);
	f2fs_set_bit(0, (char *)journal->sit_j.entries[0].se.valid_map);
	journal->sit_j.entries[1].segno = cp->cur_node_segno[1];
	journal->sit_j.entries[1].se.vblocks = cpu_to_le16((CURSEG_WARM_NODE << 10));
	journal->sit_j.entries[2].segno = cp->cur_node_segno[2];
	journal->sit_j.entries[2].se.vblocks = cpu_to_le16((CURSEG_COLD_NODE << 10));

	/* data sit for root */
	journal->sit_j.entries[3].segno = cp->cur_data_segno[0];
	journal->sit_j.entries[3].se.vblocks = cpu_to_le16((CURSEG_HOT_DATA << 10) | 1);
	f2fs_set_bit(0, (char *)journal->sit_j.entries[3].se.valid_map);
	journal->sit_j.entries[4].segno = cp->cur_data_segno[1];
	journal->sit_j.entries[4].se.vblocks = cpu_to_le16((CURSEG_WARM_DATA << 10));
	journal->sit_j.entries[5].segno = cp->cur_data_segno[2];
	journal->sit_j.entries[5].se.vblocks = cpu_to_le16((CURSEG_COLD_DATA << 10));

	memcpy(sum_compact_p, &journal->n_sits, SUM_JOURNAL_SIZE);
	sum_compact_p += SUM_JOURNAL_SIZE;

	/* hot data summary */
	sum_entry = (struct f2fs_summary *)sum_compact_p;
	sum_entry->nid = sb->root_ino;
	sum_entry->ofs_in_node = 0;
	/* warm data summary, nothing to do */
	/* cold data summary, nothing to do */

	cp_seg_blk_offset += 1;
	DBG(1, "\tWriting Segment summary block for HOT/WARM/COLD_DATA, at %lu b.addr\n", cp_seg_blk_offset);

	if(dev_write_meta_block(sum_compact, cp_seg_blk_offset, blk_size))
	{
		MSG(0, "\tError: While writing the sum_compact to disk!!!\n");
		return -1;
	}

	/* Prepare and write Segment summary for HOT_NODE */
	memset(sum, 0, sizeof(struct f2fs_summary_block));
	SET_SUM_TYPE((&sum->footer), SUM_TYPE_NODE);

	sum->entries[0].nid = sb->root_ino;
	sum->entries[0].ofs_in_node = 0;
	cp_seg_blk_offset += 1;

	DBG(1, "\tWriting Segment summary block for HOT_NODE, at %lu b.addr \n", cp_seg_blk_offset );
	if(dev_write_meta_block(sum, cp_seg_blk_offset, blk_size))
	{
		MSG(0, "\tError: While writing the summary block for HOT_NODE to disk!!!\n");
		return -1;
	}

	/* Fill segment summary for WARM_NODE to zero. */
	memset(sum, 0, sizeof(struct f2fs_summary_block));
	SET_SUM_TYPE((&sum->footer), SUM_TYPE_NODE);

	cp_seg_blk_offset += 1;
	DBG(1, "\tWriting Segment summary for WARM_NODE, at %lu b.addr \n", cp_seg_blk_offset );
	if(dev_write_meta_block(sum, cp_seg_blk_offset, blk_size))
	{
		MSG(0, "\tError: While writing the summary block for WARM_NODE to disk!!!\n");
		return -1;
	}

	/* Fill segment summary for COLD_NODE to zero. */
	memset(sum, 0, sizeof(struct f2fs_summary_block));
	SET_SUM_TYPE((&sum->footer), SUM_TYPE_NODE);
	cp_seg_blk_offset += 1;
	DBG(1, "\tWriting Segment summary for COLD_NODE, at %lu b.addr \n", cp_seg_blk_offset);
	if(dev_write_meta_block(sum, cp_seg_blk_offset, blk_size))
	{
		MSG(0, "\tError: While writing the summary block for COLD_NODE to disk!!!\n");
		return -1;
	}

	return cp_seg_blk_offset;
}
static uint64_t f2fs_write_check_point_pack_page1_pack1(uint64_t cp_seg_blk_offset, uint32_t blk_size)
{
	unsigned int i;
	uint32_t crc = 0;

	/*
	 * 1. cp page 1 of checkpoint pack 1
	 */
	set_cp(checkpoint_ver, 1);
	set_cp(cur_node_segno[0], config.cur_seg[CURSEG_HOT_NODE]);
	set_cp(cur_node_segno[1], config.cur_seg[CURSEG_WARM_NODE]);
	set_cp(cur_node_segno[2], config.cur_seg[CURSEG_COLD_NODE]);
	set_cp(cur_data_segno[0], config.cur_seg[CURSEG_HOT_DATA]);
	set_cp(cur_data_segno[1], config.cur_seg[CURSEG_WARM_DATA]);
	set_cp(cur_data_segno[2], config.cur_seg[CURSEG_COLD_DATA]);
	for (i = 3; i < MAX_ACTIVE_NODE_LOGS; i++) {
		set_cp(cur_node_segno[i], 0xffffffff);
		set_cp(cur_data_segno[i], 0xffffffff);
	}

	set_cp(cur_node_blkoff[0], 1);
	set_cp(cur_data_blkoff[0], 1);
	set_cp(valid_block_count, 2);
	set_cp(rsvd_segment_count, config.reserved_segments);
	set_cp(overprov_segment_count, (get_sb(segment_count_main) -
			get_cp(rsvd_segment_count)) *
			config.overprovision / 100);
	set_cp(overprov_segment_count, get_cp(overprov_segment_count) +
			get_cp(rsvd_segment_count));

	MSG(0, "Info: Overprovision ratio = %.3lf%%\n", config.overprovision);
	MSG(0, "Info: Overprovision segments = %u (GC reserved = %u)\n",
					get_cp(overprov_segment_count),
					config.reserved_segments);

	/* main segments - reserved segments - (node + data segments) */
	set_cp(free_segment_count, get_sb(segment_count_main) - 6);
	set_cp(user_block_count, ((get_cp(free_segment_count) + 6 -
			get_cp(overprov_segment_count)) * config.blks_per_seg));
	/* cp page (2), data summaries (1), node summaries (3) */
	set_cp(cp_pack_total_block_count, 6 + get_sb(cp_payload));
	set_cp(ckpt_flags, CP_UMOUNT_FLAG | CP_COMPACT_SUM_FLAG);
	set_cp(cp_pack_start_sum, 1 + get_sb(cp_payload));
	set_cp(valid_node_count, 1);
	set_cp(valid_inode_count, 1);
	set_cp(next_free_nid, get_sb(root_ino) + 1);
	set_cp(sit_ver_bitmap_bytesize, ((get_sb(segment_count_sit) / 2) <<
			get_sb(log_blocks_per_seg)) / 8);

	set_cp(nat_ver_bitmap_bytesize, ((get_sb(segment_count_nat) / 2) <<
			 get_sb(log_blocks_per_seg)) / 8);

	set_cp(checksum_offset, CHECKSUM_OFFSET);

	crc = f2fs_cal_crc32(F2FS_SUPER_MAGIC, cp, CHECKSUM_OFFSET);
	*((__le32 *)((unsigned char *)cp + CHECKSUM_OFFSET)) =
							cpu_to_le32(crc);

	DBG(1, "CRC(%u)\n", crc);

	cp_seg_blk_offset = get_sb(segment0_blkaddr);

	/* 1-1. write cp page 1 of pack #1 to the device. */
	DBG(1, "\tWriting main segments, cp at %lu b.addr\n", cp_seg_blk_offset);
	if(dev_write_meta_block(cp, cp_seg_blk_offset, blk_size))
	{
		MSG(0, "\tError: While writing the cp to disk!!!\n");
		return -1;
	}
	return cp_seg_blk_offset;
}

/* dos (2016.11.21)
 * I divided this part into 4 sub-functions to reduce confusedness which coming from the complexity of source codes.
 * */
static int f2fs_write_check_point_pack(void)
{
	uint32_t blk_size;
	uint64_t cp_seg_blk_offset = 0;
	unsigned int i;
	char *cp_payload = NULL;
	int ret = -1;

	blk_size = 1 << get_sb(log_blocksize);
	cp = calloc(F2FS_BLKSIZE, 1);
	if (cp == NULL) {
		MSG(1, "\tError: Calloc Failed for f2fs_checkpoint!!!\n");
		return ret;
	}

	cp_payload = calloc(F2FS_BLKSIZE, 1);
	if (cp_payload == NULL) {
		MSG(0, "\tError: Calloc Failed for cp_payload!!!\n");
		goto free_cp_payload;
	}

	cp_seg_blk_offset = f2fs_write_check_point_pack_page1_pack1(cp_seg_blk_offset, blk_size);

	if ( -1 == cp_seg_blk_offset ) 	goto free_cp_payload;

	/*
	 * Newly added codes on f2fs 1.7
	 * Since it is just zeroing, alfs ignores `dev_fill`.
	 * cp_seg_blk_offset is still affect to alfs, I guess.
	 */

	for (i = 0; i < get_sb(cp_payload); i++) {
		cp_seg_blk_offset += 1;
#ifndef ALFS_SNAPSHOT
		if (dev_fill(cp_payload, cp_seg_blk_offset * blk_size, blk_size)) {
			MSG(0, "\tError: While zeroing out the sit bitmap area "
					"on disk!!!\n");
			goto free_cp_payload;
		}
#endif
	}

	/* Prepare and write Segment summary for HOT/WARM/COLD DATA
	 *
	 * The structure of compact summary
	 * +-------------------+
	 * | nat_journal       |
	 * +-------------------+
	 * | sit_journal       |
	 * +-------------------+
	 * | hot data summary  |
	 * +-------------------+
	 * | warm data summary |
	 * +-------------------+
	 * | cold data summary |
	 * +-------------------+
	*/
	cp_seg_blk_offset = f2fs_write_check_point_pack_sum_hwc_data(cp_seg_blk_offset, blk_size);
	if ( -1 == cp_seg_blk_offset ) 	goto free_cp_payload;

	cp_seg_blk_offset = f2fs_write_check_point_pack_page2_pack1(cp_seg_blk_offset, blk_size);
	if ( -1 == cp_seg_blk_offset ) 	goto free_cp_payload;

	cp_seg_blk_offset = f2fs_write_check_point_pack_page1_pack2(cp_seg_blk_offset, blk_size);
	if ( -1 == cp_seg_blk_offset ) 	goto free_cp_payload;

	/*
	 * Newly added codes on f2fs 1.7
	 * Since it is just zeroing, alfs ignores `dev_fill`.
	 * cp_seg_blk_offset is still affect to alfs, I guess.
	 */
	for (i = 0; i < get_sb(cp_payload); i++) {
		cp_seg_blk_offset += 1;

#ifndef ALFS_SNAPSHOT
		if (dev_fill(cp_payload, cp_seg_blk_offset * blk_size,  blk_size)) {
			MSG(0, "\tError: While zeroing out the sit bitmap area "
					"on disk!!!\n");
			goto free_cp_payload;
		}
#endif
	}

	cp_seg_blk_offset = f2fs_write_check_point_pack_page2_pack2(cp_seg_blk_offset, blk_size);
	if ( -1 == cp_seg_blk_offset ) 	goto free_cp_payload;

	ret = 0;

free_cp_payload:
	free(cp_payload);

	return ret;
}

static int f2fs_write_super_block(void)
{
	int index;
	u_int8_t *zero_buff;

	zero_buff = calloc(F2FS_BLKSIZE, 1);

	memcpy(zero_buff + F2FS_SUPER_OFFSET, sb, sizeof(*sb));
	DBG(0, "\tWriting super block, at 0x%08x b.addr\n", 0);

	DBG(1, "\n----\n");
	DBG(1, "superblock_start_addr: %u\n", 0);
	for (index = 0; index < 2; index++) {
		if (dev_write(zero_buff, index * F2FS_BLKSIZE, F2FS_BLKSIZE)) {
			MSG(0, "\tError: While while writing supe_blk "
					"on disk!!! index : %d\n", index);
			free(zero_buff);
			return -1;
		}
	}

	free(zero_buff);
	return 0;
}

#ifndef WITH_ANDROID
static int discard_obsolete_dnode(struct f2fs_node *raw_node, u_int64_t offset)
{
	if (config.smr_mode)
		return 0;
	do {
		if (offset < get_sb(main_blkaddr) ||
			offset >= get_sb(main_blkaddr) + get_sb(block_count))
			break;

		if (dev_read_block(raw_node, offset)) {
			MSG(1, "\tError: While traversing direct node!!!\n");
			return -1;
		}

		memset(raw_node, 0, F2FS_BLKSIZE);

		DBG(0, "\tDiscard dnode, at offset 0x%08"PRIx64"\n", offset);
		if (dev_write_block(raw_node, offset)) {
			MSG(1, "\tError: While discarding direct node!!!\n");
			return -1;
		}
		offset = le32_to_cpu(raw_node->footer.next_blkaddr);
	} while (1);

	return 0;
}
#endif

static int f2fs_write_root_inode(void)
{
	struct f2fs_node *raw_node = NULL;
	u_int64_t blk_size_bytes, data_blk_nor;
	u_int64_t main_area_node_seg_blk_offset = 0;

	raw_node = calloc(F2FS_BLKSIZE, 1);
	if (raw_node == NULL) {
		MSG(0, "\tError: Calloc Failed for raw_node!!!\n");
		return -1;
	}

	raw_node->footer.nid = sb->root_ino;
	raw_node->footer.ino = sb->root_ino;
	raw_node->footer.cp_ver = cpu_to_le64(1);
	raw_node->footer.next_blkaddr = cpu_to_le32(
			get_sb(main_blkaddr) +
			config.cur_seg[CURSEG_HOT_NODE] *
			config.blks_per_seg + 1);

	raw_node->i.i_mode = cpu_to_le16(0x41ed);
	raw_node->i.i_links = cpu_to_le32(2);
	raw_node->i.i_uid = cpu_to_le32(getuid());
	raw_node->i.i_gid = cpu_to_le32(getgid());

	blk_size_bytes = 1 << get_sb(log_blocksize);
	raw_node->i.i_size = cpu_to_le64(1 * blk_size_bytes); /* dentry */
	raw_node->i.i_blocks = cpu_to_le64(2);

	raw_node->i.i_atime = cpu_to_le32(time(NULL));
	raw_node->i.i_atime_nsec = 0;
	raw_node->i.i_ctime = cpu_to_le32(time(NULL));
	raw_node->i.i_ctime_nsec = 0;
	raw_node->i.i_mtime = cpu_to_le32(time(NULL));
	raw_node->i.i_mtime_nsec = 0;
	raw_node->i.i_generation = 0;
	raw_node->i.i_xattr_nid = 0;
	raw_node->i.i_flags = 0;
	raw_node->i.i_current_depth = cpu_to_le32(1);
	raw_node->i.i_dir_level = DEF_DIR_LEVEL;

	data_blk_nor = get_sb(main_blkaddr) +
		config.cur_seg[CURSEG_HOT_DATA] * config.blks_per_seg;
	raw_node->i.i_addr[0] = cpu_to_le32(data_blk_nor);

	raw_node->i.i_ext.fofs = 0;
	raw_node->i.i_ext.blk_addr = 0;
	raw_node->i.i_ext.len = 0;

	main_area_node_seg_blk_offset = get_sb(main_blkaddr);
	main_area_node_seg_blk_offset += config.cur_seg[CURSEG_HOT_NODE] *
					config.blks_per_seg;
	main_area_node_seg_blk_offset *= blk_size_bytes;
	DBG(0, "\tWriting root inode (hot node), %u %u %u at offset %"PRIu64"\n", get_sb(main_blkaddr), config.cur_seg[CURSEG_HOT_NODE], config.blks_per_seg, main_area_node_seg_blk_offset/DEFAULT_BLOCKS_PER_SEGMENT);


	DBG(1, "\n----\n");
	DBG(1, "main_start_addr: %u)\n", get_sb(main_blkaddr));
	DBG(1, " * hot_node_start_addr: %u(=%u+%u*%u) \n",
		(get_sb(main_blkaddr) + config.cur_seg[CURSEG_HOT_NODE] * config.blks_per_seg),
		get_sb(main_blkaddr),
		config.cur_seg[CURSEG_HOT_NODE],
		config.blks_per_seg);
	DBG(1, "[W] raw_node:\t" "%" PRIu64 "\t%u\t" "%" PRIu64 "\n",
		main_area_node_seg_blk_offset/F2FS_BLKSIZE,
		F2FS_BLKSIZE/F2FS_BLKSIZE,
		(main_area_node_seg_blk_offset + F2FS_BLKSIZE)/F2FS_BLKSIZE);

	if (dev_write(raw_node, main_area_node_seg_blk_offset, F2FS_BLKSIZE)) {
		MSG(1, "\tError: While writing the raw_node to disk!!!\n");
		free(raw_node);
		return -1;
	}

	/* avoid power-off-recovery based on roll-forward policy */
	main_area_node_seg_blk_offset = get_sb(main_blkaddr);
	main_area_node_seg_blk_offset += config.cur_seg[CURSEG_WARM_NODE] *
					config.blks_per_seg;

#ifndef WITH_ANDROID
	if (discard_obsolete_dnode(raw_node, main_area_node_seg_blk_offset)) {
		free(raw_node);
		return -1;
	}
#endif

	free(raw_node);
	return 0;
}

static int f2fs_update_nat_root(void)
{
	struct f2fs_nat_block *nat_blk = NULL;
	uint64_t nat_seg_blk_offset = 0;

	nat_blk = calloc(F2FS_BLKSIZE, 1);
	if(nat_blk == NULL) {
		MSG(1, "\tError: Calloc Failed for nat_blk!!!\n");
		return -1;
	}

#ifdef ALFS_SNAPSHOT
	DBG(1, "super_block.(root_ino) = %u\n", get_sb(root_ino));
	DBG(1, "super_block.(node_ino) = %u\n", get_sb(node_ino));
	DBG(1, "super_block.(meta_ino) = %u\n", get_sb(meta_ino));
#endif
	/* update root */
	nat_blk->entries[get_sb(root_ino)].block_addr = cpu_to_le32(
		get_sb(main_blkaddr) +
		config.cur_seg[CURSEG_HOT_NODE] * config.blks_per_seg);
	nat_blk->entries[get_sb(root_ino)].ino = get_sb(root_ino);

	/* update node nat */
	nat_blk->entries[get_sb(node_ino)].block_addr = cpu_to_le32(1);
	nat_blk->entries[get_sb(node_ino)].ino = sb->node_ino;

	/* update meta nat */
	nat_blk->entries[get_sb(meta_ino)].block_addr = cpu_to_le32(1);
	nat_blk->entries[get_sb(meta_ino)].ino = sb->meta_ino;

	nat_seg_blk_offset = get_sb(nat_blkaddr);
	DBG(1, "\tWriting nat root, at offset (%lu) b.addr.\n", nat_seg_blk_offset);

#if defined(ALFS_SNAPSHOT) && defined(ALFS_META_LOGGING)
	if (dev_write(nat_blk, alfs_get_meta_logging_index() * F2FS_BLKSIZE, F2FS_BLKSIZE)) {
	//if (dev_write(nat_blk, _meta_log_byteofs, F2FS_BLKSIZE)) {
		MSG(0, "\tError: While writing the nat_blk set0 to disk!\n");
		free(nat_blk);
		return -1;
	}
	alfs_set_mapping_info(nat_seg_blk_offset);
	alfs_add_meta_logging_index();
#else
	DBG(0, "%llu b.addr \n", nat_seg_blk_offset);
	if (dev_write(nat_blk, nat_seg_blk_offset * F2FS_BLKSIZE, F2FS_BLKSIZE)) {
		MSG(1, "\tError: While writing the nat_blk set0 to disk!\n");
		free(nat_blk);
		return -1;
	}
#endif

	free(nat_blk);
	return 0;
}

static int f2fs_add_default_dentry_root(void)
{
	struct f2fs_dentry_block *dent_blk = NULL;
	u_int64_t blk_size_bytes, data_blk_offset = 0;

	dent_blk = calloc(F2FS_BLKSIZE, 1);
	if(dent_blk == NULL) {
		MSG(1, "\tError: Calloc Failed for dent_blk!!!\n");
		return -1;
	}

	dent_blk->dentry[0].hash_code = 0;
	dent_blk->dentry[0].ino = sb->root_ino;
	dent_blk->dentry[0].name_len = cpu_to_le16(1);
	dent_blk->dentry[0].file_type = F2FS_FT_DIR;
	memcpy(dent_blk->filename[0], ".", 1);

	dent_blk->dentry[1].hash_code = 0;
	dent_blk->dentry[1].ino = sb->root_ino;
	dent_blk->dentry[1].name_len = cpu_to_le16(2);
	dent_blk->dentry[1].file_type = F2FS_FT_DIR;
	memcpy(dent_blk->filename[1], "..", 2);

	/* bitmap for . and .. */
	test_and_set_bit_le(0, dent_blk->dentry_bitmap);
	test_and_set_bit_le(1, dent_blk->dentry_bitmap);
	blk_size_bytes = 1 << get_sb(log_blocksize);
	data_blk_offset = get_sb(main_blkaddr);
	data_blk_offset += config.cur_seg[CURSEG_HOT_DATA] *
				config.blks_per_seg;
	data_blk_offset *= blk_size_bytes;

	DBG(1, "\tWriting default dentry root, at offset 0x%08"PRIx64"\n", data_blk_offset);

#ifdef ALFS_SNAPSHOT
	DBG(1, "main_start_addr: %u \n", get_sb(main_blkaddr));
	DBG(1, " * hot_data_start_addr: %u\n",
		(get_sb(main_blkaddr) + config.cur_seg[CURSEG_HOT_DATA] * config.blks_per_seg));
	DBG(1, "[W] dentry_blk:\t" "%" PRIu64 "\t%u\t" "%" PRIu64 "\n", data_blk_offset/F2FS_BLKSIZE, F2FS_BLKSIZE/F2FS_BLKSIZE, (data_blk_offset + F2FS_BLKSIZE)/F2FS_BLKSIZE);
#endif

	if (dev_write(dent_blk, data_blk_offset, F2FS_BLKSIZE)) {
		MSG(1, "\tError: While writing the dentry_blk to disk!!!\n");
		free(dent_blk);
		return -1;
	}

	free(dent_blk);
	return 0;
}

static int f2fs_create_root_dir(void)
{
	int err = 0;

	err = f2fs_write_root_inode();
	if (err < 0) {
		MSG(1, "\tError: Failed to write root inode!!!\n");
		goto exit;
	}

	err = f2fs_update_nat_root();
	if (err < 0) {
		MSG(1, "\tError: Failed to update NAT for root!!!\n");
		goto exit;
	}

	err = f2fs_add_default_dentry_root();
	if (err < 0) {
		MSG(1, "\tError: Failed to add default dentries for root!!!\n");
		goto exit;
	}
exit:
	if (err)
		MSG(1, "\tError: Could not create the root directory!!!\n");

	return err;
}

#ifdef ALFS_SNAPSHOT
int f2fs_write_snapshot(void)
{
	uint32_t nr_segment_count_meta = 0;
	uint32_t nr_meta_blks = 0;
	uint32_t nr_mapping_blks = 0;
	uint64_t mapping_blkofs;
	uint32_t loop = 0;

	nr_segment_count_meta =
					get_sb(segment_count_ckpt) +
					get_sb(segment_count_sit) +
					get_sb(segment_count_nat) +
					get_sb(segment_count_ssa);
	nr_meta_blks = nr_segment_count_meta * config.blks_per_seg;

	nr_mapping_blks = nr_meta_blks / NR_MAPPING_ROOMS;

	if (nr_meta_blks % NR_MAPPING_ROOMS != 0) {
		nr_mapping_blks++;
	}

	/*
	 * (4) write the mapping table & summary table
	 */

	mapping_blkofs = ((config.segs_per_sec * config.blks_per_seg) * NR_SUPERBLK_SECS) * 1; // NOT A BYTE OFF SET

	for (loop = 0; loop < nr_mapping_blks; loop++) {
		if (dev_write ((__le32*)(_ptr_map_blks + loop), mapping_blkofs * F2FS_BLKSIZE, F2FS_BLKSIZE)) {
			MSG(0, "\tError: While writing the mapping table to disk!!!\n");
			return -1;
		}

		mapping_blkofs += 1;
		//mapping_blkofs += F2FS_BLKSIZE;
	}

	/* (5) free the memory space */
	alfs_free_mapping_info_table();
	/*free (ptr_mapping_table);*/
	return 0;
}
#endif


int f2fs_format_device(void)
{
	int err = 0;

	err= f2fs_prepare_super_block();
	if (err < 0) {
		MSG(0, "\tError: Failed to prepare a super block!!!\n");
		goto exit;
	}

#ifdef ALFS_SNAPSHOT
	/* initialize the mapping table */
	alfs_init_mapping_info_table();
#endif

	err = f2fs_trim_device();
	if (err < 0) {
		MSG(0, "\tError: Failed to trim whole device!!!\n");
		goto exit;
	}

	err = f2fs_init_sit_area();
	if (err < 0) {
		MSG(0, "\tError: Failed to Initialise the SIT AREA!!!\n");
		goto exit;
	}

	err = f2fs_init_nat_area();
	if (err < 0) {
		MSG(0, "\tError: Failed to Initialise the NAT AREA!!!\n");
		goto exit;
	}

	err = f2fs_create_root_dir();
	if (err < 0) {
		MSG(0, "\tError: Failed to create the root directory!!!\n");
		goto exit;
	}

	err = f2fs_write_check_point_pack();
	if (err < 0) {
		MSG(0, "\tError: Failed to write the check point pack!!!\n");
		goto exit;
	}

	err = f2fs_write_super_block();
	if (err < 0) {
		MSG(0, "\tError: Failed to write the Super Block!!!\n");
		goto exit;
	}
	/*#if defined(ALFS_SNAPSHOT) || defined(ALFS_META_LOGGING)*/
#ifdef ALFS_SNAPSHOT
	err = f2fs_write_snapshot();
	if (err < 0) {
		MSG(0, "\tError: Failed to write the snapshot!!!\n");
		goto exit;
	}
#endif

exit:
	if (err)
		MSG(0, "\tError: Could not format the device!!!\n");

	return err;
}
