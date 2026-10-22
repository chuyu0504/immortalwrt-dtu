/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AW_SPINAND_COMPAT_H
#define AW_SPINAND_COMPAT_H

#include <linux/mtd/mtd.h>
#include <linux/mutex.h>

struct spinand_device;

/* The physical MTD is private; only the paired-block view is registered. */
struct aw_spinand_compat {
	struct mtd_info mtd;
	struct mtd_info *physical;
	struct mutex lock;
};

int aw_spinand_compat_init(struct aw_spinand_compat *compat,
			 struct mtd_info *physical);
int aw_spinand_compat_register(struct spinand_device *spinand);
int aw_spinand_compat_unregister(struct spinand_device *spinand);

#endif
