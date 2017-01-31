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
#include <linux/fs.h>
#include <uuid/uuid.h>

#include "f2fs_fs.h"
#include "f2fs_format_utils.h"

/* chamdoo - 2013.09.18 */
/*#define ENABLE_DBG_LOG*/
#define ALFS_SNAPSHOT
#define ALFS_META_LOGGING
/*#define ALFS_LARGE_SEGMENT*/

#ifdef ENABLE_DBG_LOG
#define dbg_log(fmt, ...)	\
		do {	\
		printf(fmt, ##__VA_ARGS__);	\
	} while (0);
#else
	#define dbg_log(fmt, ...)
#endif

#if defined(ALFS_SNAPSHOT) && defined(ALFS_META_LOGGING)
#define NR_SUPERBLK_SECS	1	/* # of sections for the super block */
#define NR_MAPPING_SECS		3 	/* # of sections for mapping entries */
#define NR_METALOG_TIMES	2	/* # of sections for meta-log */
#endif
/* end */

extern struct f2fs_configuration config;

static void mkfs_usage()
{
	MSG(0, "\nUsage: mkfs.f2fs [options] device [sectors]\n");
	MSG(0, "[options]:\n");
	MSG(0, "  -a heap-based allocation [default:1]\n");
	MSG(0, "  -d debug level [default:0]\n");
	MSG(0, "  -e [extension list] e.g. \"mp3,gif,mov\"\n");
	MSG(0, "  -l label\n");
	MSG(0, "  -o overprovision ratio [default:5]\n");
	MSG(0, "  -O set feature\n");
	MSG(0, "  -q quiet mode\n");
	MSG(0, "  -s # of segments per section [default:1]\n");
	MSG(0, "  -z # of sections per zone [default:1]\n");
	MSG(0, "  -t 0: nodiscard, 1: discard [default:1]\n");
	MSG(0, "  -m support SMR device [default:0]\n");
	MSG(0, "sectors: number of sectors. [default: determined by device size]\n");
	exit(1);
}

static void f2fs_show_info()
{
	MSG(0, "\n\tF2FS-tools: mkfs.f2fs Ver: %s (%s)\n\n",
				F2FS_TOOLS_VERSION,
				F2FS_TOOLS_DATE);
	if (config.heap == 0)
		MSG(0, "Info: Disable heap-based policy\n");

	MSG(0, "Info: Debug level = %d\n", config.dbg_lv);
	if (config.extension_list)
		MSG(0, "Info: Add new extension list\n");

	if (config.vol_label)
		MSG(0, "Info: Label = %s\n", config.vol_label);
	MSG(0, "Info: Trim is %s\n", config.trim ? "enabled": "disabled");
}

static void parse_feature(char *features)
{
	if (!strcmp(features, "encrypt")) {
		config.feature |= cpu_to_le32(F2FS_FEATURE_ENCRYPT);
	} else {
		MSG(0, "Error: Wrong features\n");
		mkfs_usage();
	}
}

static void f2fs_parse_options(int argc, char *argv[])
{
	static const char *option_string = "qa:d:e:l:mo:O:s:z:t:";
	int32_t option=0;

	while ((option = getopt(argc,argv,option_string)) != EOF) {
		switch (option) {
		case 'q':
			config.dbg_lv = -1;
			break;
		case 'a':
			config.heap = atoi(optarg);
			break;
		case 'd':
			config.dbg_lv = atoi(optarg);
			break;
		case 'e':
			config.extension_list = strdup(optarg);
			break;
		case 'l':		/*v: volume label */
			if (strlen(optarg) > 512) {
				MSG(0, "Error: Volume Label should be less than "
						"512 characters\n");
				mkfs_usage();
			}
			config.vol_label = optarg;
			break;
		case 'm':
			config.smr_mode = 1;
			break;
		case 'o':
			config.overprovision = atof(optarg);
			break;
		case 'O':
			parse_feature(strdup(optarg));
			break;
		case 's':
			config.segs_per_sec = atoi(optarg);
			break;
		case 'z':
			config.secs_per_zone = atoi(optarg);
			break;
		case 't':
			config.trim = atoi(optarg);
			break;
		default:
			MSG(0, "\tError: Unknown option %c\n",option);
			mkfs_usage();
			break;
		}
	}

	if (optind >= argc) {
		MSG(0, "\tError: Device not specified\n");
		mkfs_usage();
	}
	config.device_name = argv[optind];

	if ((optind + 1) < argc)
		config.total_sectors = atoll(argv[optind+1]);

	if (config.smr_mode)
		config.feature |= cpu_to_le32(F2FS_FEATURE_HMSMR);
}

int main(int argc, char *argv[])
{
#ifdef ALFS_SNAPSHOT
	MSG(0, "\n\tModified for RISA (RC.1) (%s %s)\n\n", __TIME__, __DATE__);
#endif

	f2fs_init_configuration(&config);

	f2fs_parse_options(argc, argv);

	f2fs_show_info();

	if (f2fs_dev_is_umounted(&config) < 0) {
		MSG(0, "\tError: Not available on mounted device!\n");
		return -1;
	}

	if (f2fs_get_device_info(&config) < 0)
		return -1;


	/* modified by chamdoo */
	config.reserved_segments = (config.total_sectors / 4 * 0.05) / (2 * 1024);
	MSG (0, "\t %d = %f KB / %d KB\n",
		config.reserved_segments,
		config.total_sectors / 4 * 0.05,
		2 * 1024);
	if (config.reserved_segments % config.segs_per_sec != 0) {
		config.reserved_segments += config.segs_per_sec;
		config.reserved_segments /= config.segs_per_sec;
		config.reserved_segments *= config.segs_per_sec;
	}
	MSG (0, "\tINFO: overprovisioning = %d segs (%d MB)\n",
		config.reserved_segments,
		config.reserved_segments * 2);
	/* end */
#if defined(ALFS_SNAPSHOT) && defined(ALFS_META_LOGGING)
	/* For RISA, two sectors are reserved for check points.
	 * The metalog begins after two check points */
	{
		unsigned long long meta_log_starting_blk_ofs = 0, meta_log_byte_ofs = 0;
		meta_log_starting_blk_ofs = 	(config.segs_per_sec * config.blks_per_seg) *
							(NR_SUPERBLK_SECS + NR_MAPPING_SECS);
		meta_log_byte_ofs = meta_log_starting_blk_ofs * F2FS_BLKSIZE;

		// blkofs = (1*512)*(1+3) = 512*4
		// blkofs = 2048
		// byteofs = 2048 * 4096 = 8,388,608
		alfs_init_meta_log_blk_ofs(meta_log_starting_blk_ofs);
		DBG(1, "meta_log_starting_blk_ofs = %llu (blk)\n", meta_log_starting_blk_ofs);
		DBG(1, "convert it to bytes, %llu (B)\n", meta_log_byte_ofs);
	}

#endif

	if (f2fs_format_device() < 0)
		return -1;

	f2fs_finalize_device(&config);

	MSG(0, "Info: format successful\n");

	return 0;
}
