/*
 * Copyright (c) 2025 OpenBMC Shim Layer
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_IPMB_FS_H_
#define ZEPHYR_IPMB_FS_H_

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPMB_MNT_POINT "/dev/ipmb"

/* 初始化函数声明 */
int ipmb_fs_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_IPMB_FS_H_ */
