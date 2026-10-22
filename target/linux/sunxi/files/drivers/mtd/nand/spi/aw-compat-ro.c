// SPDX-License-Identifier: GPL-2.0-only
/*
 * Allwinner simulated-multiplane view for Ebyte ECM30.
 *
 * Verified against the supplied ECM30 Linux 5.4 ELF's super_to_phy(),
 * addr_to_req() and aw_spinand_chip_isbad_super_block(). A logical page
 * concatenates the same page of two adjacent physical blocks. Offsets are
 * absolute NAND offsets; the 10 MiB sys offset must not be subtracted here.
 *
 * The native MTD stays private. The original compatible remains read-only;
 * the separate writable compatible only permits writes within sys.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/spinand.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "aw-compat-ro.h"

#define AW_PHY_PAGE	4096U
#define AW_PHY_BLOCK	SZ_256K
#define AW_PHY_OOB	128U
#define AW_SPARE		16U
#define AW_PAGE		(2 * AW_PHY_PAGE)
#define AW_BLOCK		(2 * AW_PHY_BLOCK)
#define AW_OOB		(2 * AW_PHY_OOB)
#define AW_OOB_AVAIL	(2 * AW_SPARE)
#define AW_WRITE_START	(SZ_8M + SZ_2M)

static struct aw_spinand_compat *aw_to_compat(struct mtd_info *mtd)
{
	return container_of(mtd, struct aw_spinand_compat, mtd);
}

static loff_t aw_physical_offset(loff_t offset)
{
	u64 block = (u64)offset / AW_BLOCK;
	u32 within = offset % AW_BLOCK;
	u32 page = within / AW_PAGE;
	u32 half = (within % AW_PAGE) / AW_PHY_PAGE;

	return block * AW_BLOCK + half * AW_PHY_BLOCK +
	       page * AW_PHY_PAGE + within % AW_PHY_PAGE;
}

static int aw_ooblayout_free(struct mtd_info *mtd, int section,
			     struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;

	/* The vendor ABI packs 16 protected spare bytes from each half. */
	region->offset = 0;
	region->length = AW_OOB_AVAIL;
	return 0;
}

static const struct mtd_ooblayout_ops aw_ooblayout = {
	.free = aw_ooblayout_free,
};

static int aw_read_half(struct aw_spinand_compat *compat, loff_t page,
			unsigned int half, unsigned int column, size_t datalen,
			unsigned int ooboff, size_t ooblen,
			struct mtd_oob_ops *ops)
{
	struct mtd_info *physical = compat->physical;
	unsigned int start = clamp_t(unsigned int, column,
				    half * AW_PHY_PAGE, (half + 1) * AW_PHY_PAGE);
	unsigned int end = clamp_t(unsigned int, column + datalen,
				  half * AW_PHY_PAGE, (half + 1) * AW_PHY_PAGE);
	unsigned int os = clamp_t(unsigned int, ooboff,
				 half * AW_SPARE, (half + 1) * AW_SPARE);
	unsigned int oe = clamp_t(unsigned int, ooboff + ooblen,
				 half * AW_SPARE, (half + 1) * AW_SPARE);
	struct mtd_oob_ops req = {
		.mode = MTD_OPS_PLACE_OOB,
		.len = end - start,
		.ooblen = oe - os,
		.ooboffs = os - half * AW_SPARE,
		.stats = ops->stats,
	};
	struct mtd_ecc_stats before;
	loff_t from;
	int ret;

	if (!req.len && !req.ooblen)
		return 0;
	if (req.len)
		req.datbuf = ops->datbuf + ops->retlen;
	if (req.ooblen)
		req.oobbuf = ops->oobbuf + ops->oobretlen;
	from = aw_physical_offset(page + half * AW_PHY_PAGE);
	if (req.len)
		from += start - half * AW_PHY_PAGE;

	before = physical->ecc_stats;
	/* The backend is deliberately unregistered and keeps native geometry. */
	ret = physical->_read_oob(physical, from, &req);
	compat->mtd.ecc_stats.corrected += physical->ecc_stats.corrected - before.corrected;
	compat->mtd.ecc_stats.failed += physical->ecc_stats.failed - before.failed;
	if (req.retlen > req.len || req.oobretlen > req.ooblen)
		return -EIO;
	ops->retlen += req.retlen;
	ops->oobretlen += req.oobretlen;
	if (ret < 0 && ret != -EBADMSG)
		return ret;
	if (req.retlen != req.len || req.oobretlen != req.ooblen)
		return -EIO;
	return ret;
}

static int aw_read_oob(struct mtd_info *mtd, loff_t from,
		       struct mtd_oob_ops *ops)
{
	struct aw_spinand_compat *compat = aw_to_compat(mtd);
	size_t remaining = ops->len, spare = ops->ooblen;
	unsigned int column, ooboff = ops->ooboffs;
	loff_t page;
	int ret = 0, max_bitflips = 0;
	bool failed = false;

	ops->retlen = 0;
	ops->oobretlen = 0;
	/* Never provide an ECC-bypassing raw alias of the vendor view. */
	if (ops->mode == MTD_OPS_RAW ||
	    (spare && ops->mode != MTD_OPS_AUTO_OOB))
		return -EOPNOTSUPP;
	if (ops->mode != MTD_OPS_PLACE_OOB && ops->mode != MTD_OPS_AUTO_OOB)
		return -EINVAL;
	if (from < 0 || from > mtd->size || remaining > mtd->size - from ||
	    (remaining && !ops->datbuf) || (spare && !ops->oobbuf))
		return -EINVAL;
	if (spare && (from == mtd->size || ooboff >= AW_OOB_AVAIL ||
	    spare > ((mtd->size - (from / AW_PAGE) * AW_PAGE) / AW_PAGE) *
		    AW_OOB_AVAIL - ooboff))
		return -EINVAL;

	column = from % AW_PAGE;
	page = from - column;
	mutex_lock(&compat->lock);
	while (remaining || spare) {
		size_t bytes = min_t(size_t, AW_PAGE - column, remaining);
		size_t oob = min_t(size_t, AW_OOB_AVAIL - ooboff, spare);
		unsigned int half;

		for (half = 0; half < 2; half++) {
			ret = aw_read_half(compat, page, half, column, bytes,
					   ooboff, oob, ops);
			if (ret == -EBADMSG)
				failed = true;
			else if (ret < 0)
				goto out;
			else
				max_bitflips = max(max_bitflips, ret);
		}
		remaining -= bytes;
		spare -= oob;
		page += AW_PAGE;
		column = 0;
		ooboff = 0;
	}
	ret = failed ? -EBADMSG : max_bitflips;
out:
	mutex_unlock(&compat->lock);
	return ret;
}

static int aw_isbad_half(struct aw_spinand_compat *compat, loff_t base,
			 unsigned int half)
{
	u8 marker = 0;
	struct mtd_oob_ops ops = { .oobbuf = &marker };
	int ret;

	/*
	 * AW checks the first protected spare byte of page zero. The native
	 * core checks TWO raw bytes, which would mistake vendor metadata in
	 * byte one for a bad-block marker. Do not use that native callback.
	 */
	ret = aw_read_half(compat, base, half, 0, 0, half * AW_SPARE, 1, &ops);
	if (ret < 0 && ret != -EBADMSG) {
		ops.oobretlen = 0;
		ret = aw_read_half(compat, base, half, 0, 0, half * AW_SPARE, 1, &ops);
	}
	if (ret < 0 && ret != -EBADMSG)
		return ret;
	return marker != 0xff;
}

static int aw_check_pair(struct mtd_info *mtd, loff_t offset, bool reserved)
{
	struct aw_spinand_compat *compat = aw_to_compat(mtd);
	struct mtd_info *physical = compat->physical;
	loff_t base;
	int ret;

	if (offset < 0 || offset >= mtd->size)
		return -EINVAL;
	if (reserved && !physical->_block_isreserved)
		return 0;
	base = (offset / AW_BLOCK) * AW_BLOCK;
	mutex_lock(&compat->lock);
	if (reserved) {
		ret = physical->_block_isreserved(physical, base);
		if (!ret)
			ret = physical->_block_isreserved(physical, base + AW_PHY_BLOCK);
	} else {
		ret = aw_isbad_half(compat, base, 0);
		if (!ret)
			ret = aw_isbad_half(compat, base, 1);
	}
	mutex_unlock(&compat->lock);
	return ret;
}

static int aw_block_isbad(struct mtd_info *mtd, loff_t offset)
{
	return aw_check_pair(mtd, offset, false);
}

static int aw_block_isreserved(struct mtd_info *mtd, loff_t offset)
{
	return aw_check_pair(mtd, offset, true);
}

static int aw_max_bad_blocks(struct mtd_info *mtd, loff_t offset, size_t len)
{
	struct mtd_info *physical = aw_to_compat(mtd)->physical;

	return physical->_max_bad_blocks(physical, offset, len);
}

static int aw_erase(struct mtd_info *mtd, struct erase_info *info)
{
	struct aw_spinand_compat *compat = aw_to_compat(mtd);
	loff_t base, end;
	int ret = 0;
	unsigned int half;

	if (!compat->writable)
		return -EROFS;
	if (info->addr < AW_WRITE_START || info->addr > mtd->size ||
	    info->len > mtd->size - info->addr ||
	    info->addr % AW_BLOCK || info->len % AW_BLOCK)
		return -EINVAL;
	info->fail_addr = MTD_FAIL_ADDR_UNKNOWN;
	end = info->addr + info->len;
	mutex_lock(&compat->lock);
	for (base = info->addr; base < end; base += AW_BLOCK) {
		for (half = 0; half < 2; half++) {
			ret = aw_isbad_half(compat, base, half);
			if (ret) {
				ret = ret < 0 ? ret : -EIO;
				goto failed;
			}
		}
		for (half = 0; half < 2; half++) {
			struct erase_info physical = {
				.addr = base + half * AW_PHY_BLOCK,
				.len = AW_PHY_BLOCK,
			};

			/* The private backend bypasses the native two-byte BBM test. */
			ret = compat->physical->_erase(compat->physical, &physical);
			if (ret)
				goto failed;
		}
		cond_resched();
	}
	goto out;
failed:
	info->fail_addr = base;
out:
	mutex_unlock(&compat->lock);
	return ret;
}

static int aw_write_half(struct aw_spinand_compat *compat, loff_t page,
			 unsigned int half, unsigned int column, size_t datalen,
			 unsigned int ooboff, size_t ooblen,
			 struct mtd_oob_ops *ops)
{
	unsigned int start = clamp_t(unsigned int, column,
				    half * AW_PHY_PAGE, (half + 1) * AW_PHY_PAGE);
	unsigned int end = clamp_t(unsigned int, column + datalen,
				  half * AW_PHY_PAGE, (half + 1) * AW_PHY_PAGE);
	unsigned int os = clamp_t(unsigned int, ooboff,
				 half * AW_SPARE, (half + 1) * AW_SPARE);
	unsigned int oe = clamp_t(unsigned int, ooboff + ooblen,
				 half * AW_SPARE, (half + 1) * AW_SPARE);
	struct mtd_oob_ops req = {
		.mode = MTD_OPS_PLACE_OOB, .len = end - start,
		.ooblen = oe - os, .ooboffs = os - half * AW_SPARE,
	};
	loff_t to = aw_physical_offset(page + half * AW_PHY_PAGE);
	int ret;

	if (!req.len && !req.ooblen)
		return 0;
	if (req.len) {
		req.datbuf = ops->datbuf + ops->retlen;
		to += start - half * AW_PHY_PAGE;
	}
	if (req.ooblen)
		req.oobbuf = ops->oobbuf + ops->oobretlen;
	ret = compat->physical->_write_oob(compat->physical, to, &req);
	if (req.retlen > req.len || req.oobretlen > req.ooblen)
		return -EIO;
	ops->retlen += req.retlen;
	ops->oobretlen += req.oobretlen;
	if (ret)
		return ret;
	return req.retlen == req.len && req.oobretlen == req.ooblen ? 0 : -EIO;
}

static int aw_write_oob(struct mtd_info *mtd, loff_t to, struct mtd_oob_ops *ops)
{
	struct aw_spinand_compat *compat = aw_to_compat(mtd);
	size_t remaining = ops->len, spare = ops->ooblen;
	unsigned int column, ooboff = ops->ooboffs, half;
	loff_t page;
	int ret = 0;

	ops->retlen = 0;
	ops->oobretlen = 0;
	if (!compat->writable)
		return -EROFS;
	if (ops->mode == MTD_OPS_RAW ||
	    (spare && ops->mode != MTD_OPS_AUTO_OOB))
		return -EOPNOTSUPP;
	if (ops->mode != MTD_OPS_PLACE_OOB && ops->mode != MTD_OPS_AUTO_OOB)
		return -EINVAL;
	/* UBI subpages are whole physical pages; never reprogram a partial page. */
	if (to < AW_WRITE_START || to > mtd->size || remaining > mtd->size - to ||
	    to % AW_PHY_PAGE || remaining % AW_PHY_PAGE ||
	    (remaining && !ops->datbuf) || (spare && !ops->oobbuf))
		return -EINVAL;
	if (spare && (to == mtd->size || ooboff >= AW_OOB_AVAIL ||
	    spare > ((mtd->size - (to / AW_PAGE) * AW_PAGE) / AW_PAGE) *
		    AW_OOB_AVAIL - ooboff))
		return -EINVAL;
	column = to % AW_PAGE;
	page = to - column;
	mutex_lock(&compat->lock);
	while (remaining || spare) {
		size_t bytes = min_t(size_t, AW_PAGE - column, remaining);
		size_t oob = min_t(size_t, AW_OOB_AVAIL - ooboff, spare);

		for (half = 0; half < 2; half++) {
			ret = aw_write_half(compat, page, half, column, bytes,
					    ooboff, oob, ops);
			if (ret)
				goto out;
		}
		remaining -= bytes;
		spare -= oob;
		page += AW_PAGE;
		column = 0;
		ooboff = 0;
		cond_resched();
	}
out:
	mutex_unlock(&compat->lock);
	return ret;
}

static int aw_markbad(struct mtd_info *mtd, loff_t offset)
{
	struct aw_spinand_compat *compat = aw_to_compat(mtd);
	u8 marker[AW_SPARE] = { 0 };
	loff_t base;
	unsigned int half, last;
	int ret = 0, err;
	bool newly_marked = false;

	if (!compat->writable)
		return -EROFS;
	if (offset < AW_WRITE_START || offset >= mtd->size)
		return -EINVAL;
	base = (offset / AW_BLOCK) * AW_BLOCK;
	mutex_lock(&compat->lock);
	/* Match the vendor markers on both the first and last physical pages. */
	for (half = 0; half < 2; half++) {
		struct erase_info erase = {
			.addr = base + half * AW_PHY_BLOCK, .len = AW_PHY_BLOCK,
		};

		err = aw_isbad_half(compat, base, half);
		if (err > 0)
			continue;
		newly_marked = true;
		/* Erase is best effort: a worn-out block must still be marked. */
		compat->physical->_erase(compat->physical, &erase);
		for (last = 0; last < 2; last++) {
			struct mtd_oob_ops req = {
				.mode = MTD_OPS_PLACE_OOB, .oobbuf = marker,
				.ooblen = sizeof(marker),
			};
			loff_t to = erase.addr + last * (AW_PHY_BLOCK - AW_PHY_PAGE);

			err = compat->physical->_write_oob(compat->physical, to, &req);
			if (!err && req.oobretlen != sizeof(marker))
				err = -EIO;
			if (err && !ret)
				ret = err;
		}
	}
	if (!ret && newly_marked)
		mtd->ecc_stats.badblocks++;
	mutex_unlock(&compat->lock);
	return ret;
}

static void aw_resume(struct mtd_info *mtd)
{
	struct aw_spinand_compat *compat = aw_to_compat(mtd);

	if (compat->physical->_resume) {
		mutex_lock(&compat->lock);
		compat->physical->_resume(compat->physical);
		mutex_unlock(&compat->lock);
	}
}

int aw_spinand_compat_init(struct aw_spinand_compat *compat,
			 struct mtd_info *physical, bool writable)
{
	struct mtd_info *mtd = &compat->mtd;

	/* Fail closed: this compatibility format was checked only for ECM30. */
	if (physical->size != SZ_512M || physical->writesize != AW_PHY_PAGE ||
	    physical->erasesize != AW_PHY_BLOCK || physical->oobsize != AW_PHY_OOB ||
	    physical->ecc_strength != 8 || physical->ecc_step_size != 512 ||
	    !physical->_read_oob ||
	    (writable && (!physical->_write_oob || !physical->_erase)))
		return -EINVAL;

	compat->physical = physical;
	compat->writable = writable;
	mutex_init(&compat->lock);
	mtd->name = "sunxi_mtd_nand";
	mtd->type = MTD_NANDFLASH;
	mtd->flags = MTD_CAP_NANDFLASH & (writable ? ~0U : ~MTD_WRITEABLE);
	mtd->owner = THIS_MODULE;
	mtd->size = physical->size;
	mtd->writesize = AW_PAGE;
	mtd->writebufsize = AW_PAGE;
	mtd->erasesize = AW_BLOCK;
	mtd->oobsize = AW_OOB;
	mtd->oobavail = AW_OOB_AVAIL;
	mtd->subpage_sft = 1;
	mtd->ecc_strength = physical->ecc_strength;
	mtd->ecc_step_size = physical->ecc_step_size;
	/* Vendor ECC_LIMIT starts at 5 corrected bits (also at 8). */
	mtd->bitflip_threshold = 5;
	mtd->_read_oob = aw_read_oob;
	mtd->_write_oob = aw_write_oob;
	mtd->_erase = aw_erase;
	mtd->_block_markbad = aw_markbad;
	mtd->_block_isbad = aw_block_isbad;
	mtd->_block_isreserved = aw_block_isreserved;
	if (physical->_max_bad_blocks)
		mtd->_max_bad_blocks = aw_max_bad_blocks;
	mtd->_resume = aw_resume;
	mtd_set_ooblayout(mtd, &aw_ooblayout);
	return 0;
}

/* Keep the original boundaries even if a stale bootargs contains mtdparts. */
static const struct mtd_partition aw_partitions[] = {
	{ .name = "boot0", .offset = 0, .size = SZ_2M,
	  .mask_flags = MTD_WRITEABLE },
	{ .name = "uboot", .offset = SZ_2M, .size = SZ_4M + SZ_2M,
	  .mask_flags = MTD_WRITEABLE },
	{ .name = "secure_storage", .offset = SZ_8M, .size = SZ_2M,
	  .mask_flags = MTD_WRITEABLE },
	{ .name = "sys", .offset = SZ_8M + SZ_2M, .size = SZ_512M - SZ_8M - SZ_2M,
	  .mask_flags = 0 },
};

int aw_spinand_compat_register(struct spinand_device *spinand)
{
	static const char * const no_parsers[] = { NULL };
	struct device *dev = &spinand->spimem->spi->dev;
	struct aw_spinand_compat *compat;
	int ret;

	if (!spinand->aw_compat_enabled || spinand->id.data[0] != 0xc8 ||
	    spinand->id.data[1] != 0x94)
		return -ENODEV;
	compat = devm_kzalloc(dev, sizeof(*compat), GFP_KERNEL);
	if (!compat)
		return -ENOMEM;
	ret = aw_spinand_compat_init(compat, spinand_to_mtd(spinand),
				    !spinand->aw_compat_readonly);
	if (ret)
		return dev_err_probe(dev, ret, "Unsupported ECM30 NAND geometry\n");
	compat->mtd.dev.parent = dev;
	mtd_set_of_node(&compat->mtd, dev->of_node);
	ret = mtd_device_parse_register(&compat->mtd, no_parsers, NULL,
					aw_partitions, ARRAY_SIZE(aw_partitions));
	if (ret)
		return ret;
	spinand->aw_compat = compat;
	dev_info(dev, "AW NAND compatibility: %s, 8192-byte pages, 524288-byte blocks\n",
		 compat->writable ? "sys writable" : "read-only");
	return 0;
}

int aw_spinand_compat_unregister(struct spinand_device *spinand)
{
	int ret = mtd_device_unregister(&spinand->aw_compat->mtd);

	if (!ret)
		spinand->aw_compat = NULL;
	return ret;
}
