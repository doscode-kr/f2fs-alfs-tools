/**
 * f2fs_format_utils.c
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * Dual licensed under the GPL or LGPL version 2 licenses.
 */
#define _LARGEFILE64_SOURCE
#define ALFS_SNAPSHOT
#define ALFS_META_LOGGING

#include "f2fs_fs.h"

extern struct f2fs_configuration config;

int f2fs_trim_device(void);
int f2fs_format_device(void);

#if defined(ALFS_SNAPSHOT) && defined(ALFS_META_LOGGING)
void alfs_init_meta_log_blk_ofs(uint64_t value);
void alfs_init_mapping_info_table();
void alfs_set_mapping_info(uint64_t source);
#endif