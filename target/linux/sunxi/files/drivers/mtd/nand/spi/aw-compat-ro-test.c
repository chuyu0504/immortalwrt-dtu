// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the driver against physical page fixtures, without NAND hardware. */
#include <kunit/test.h>
#include <linux/mtd/spinand.h>
#include <linux/sizes.h>

#include "aw-compat-ro.h"

struct aw_test_call {
	loff_t from;
	size_t len, ooblen;
	u32 ooboffs, mode;
};

struct aw_test_ctx {
	struct aw_spinand_compat compat;
	struct mtd_info physical;
	struct aw_test_call calls[16];
	int results[16];
	unsigned int ncalls, short_at;
	int bad_block, error_block;
	u32 bad_queries[4];
	unsigned int nqueries;
	u8 data[16384], oob[64];
};

static int aw_fake_read(struct mtd_info *mtd, loff_t from,
			struct mtd_oob_ops *ops)
{
	struct aw_test_ctx *ctx = container_of(mtd, struct aw_test_ctx, physical);
	u32 block = from / SZ_256K, page = (from % SZ_256K) / 4096;
	unsigned int n = ctx->ncalls++;
	int ret;
	size_t i;

	if (n >= ARRAY_SIZE(ctx->calls))
		return -EIO;
	ctx->calls[n] = (struct aw_test_call) {
		.from = from, .len = ops->len, .ooblen = ops->ooblen,
		.ooboffs = ops->ooboffs, .mode = ops->mode,
	};
	if (!ops->len && ops->ooblen == 1 && !ops->ooboffs) {
		if (ctx->nqueries < ARRAY_SIZE(ctx->bad_queries))
			ctx->bad_queries[ctx->nqueries++] = block;
		if (block == ctx->error_block)
			return -ETIMEDOUT;
		ops->oobbuf[0] = block == ctx->bad_block ? 0 : 0xff;
		ops->oobretlen = 1;
		return 0;
	}
	ret = ctx->results[n];
	if (ret < 0 && ret != -EBADMSG)
		return ret;
	if (ops->datbuf)
		memset(ops->datbuf, block ^ page, ops->len);
	for (i = 0; i < ops->ooblen; i++)
		ops->oobbuf[i] = 0x80 + (block % 4) * 16 + ops->ooboffs + i;
	ops->retlen = ops->len;
	ops->oobretlen = ops->ooblen;
	if (ctx->short_at == n + 1 && ops->retlen)
		ops->retlen--;
	if (ret == -EBADMSG) {
		mtd->ecc_stats.failed++;
		if (ops->stats)
			ops->stats->uncorrectable_errors++;
	} else {
		mtd->ecc_stats.corrected += ret;
		if (ops->stats)
			ops->stats->corrected_bitflips += ret;
	}
	return ret;
}

static int aw_test_init(struct kunit *test)
{
	struct aw_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	test->priv = ctx;
	ctx->physical = (struct mtd_info) {
		.size = SZ_512M, .writesize = 4096, .erasesize = SZ_256K,
		.oobsize = 128, .ecc_strength = 8, .ecc_step_size = 512,
		._read_oob = aw_fake_read,
	};
	ctx->bad_block = -1;
	ctx->error_block = -1;
	return aw_spinand_compat_init(&ctx->compat, &ctx->physical);
}

static int aw_test_read(struct aw_test_ctx *ctx, loff_t from, size_t len)
{
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_PLACE_OOB, .datbuf = ctx->data, .len = len,
	};
	int ret = ctx->compat.mtd._read_oob(&ctx->compat.mtd, from, &ops);

	if (!ret && ops.retlen != len)
		return -EIO;
	return ret;
}

static void aw_geometry_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;
	struct mtd_info *mtd = &ctx->compat.mtd;

	KUNIT_EXPECT_EQ(test, mtd->writesize, 8192U);
	KUNIT_EXPECT_EQ(test, mtd->erasesize, (u32)SZ_512K);
	KUNIT_EXPECT_EQ(test, mtd->oobsize, 256U);
	KUNIT_EXPECT_EQ(test, mtd->oobavail, 32U);
	KUNIT_EXPECT_EQ(test, mtd->subpage_sft, 1U);
	KUNIT_EXPECT_FALSE(test, mtd->flags & MTD_WRITEABLE);
	ctx->physical.writesize = 2048;
	KUNIT_EXPECT_EQ(test, aw_spinand_compat_init(&ctx->compat, &ctx->physical), -EINVAL);
}

static void aw_paired_pages_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, aw_test_read(ctx, 0, 16384), 0);
	KUNIT_ASSERT_EQ(test, ctx->ncalls, 4U);
	KUNIT_EXPECT_EQ(test, ctx->calls[0].from, (loff_t)0);
	KUNIT_EXPECT_EQ(test, ctx->calls[1].from, (loff_t)SZ_256K);
	KUNIT_EXPECT_EQ(test, ctx->calls[2].from, (loff_t)4096);
	KUNIT_EXPECT_EQ(test, ctx->calls[3].from, (loff_t)(SZ_256K + 4096));
	KUNIT_EXPECT_EQ(test, ctx->data[0], (u8)0);
	KUNIT_EXPECT_EQ(test, ctx->data[4096], (u8)1);
	KUNIT_EXPECT_EQ(test, ctx->data[8192], (u8)1);
	KUNIT_EXPECT_EQ(test, ctx->data[12288], (u8)0);
}

static void aw_sys_absolute_offset_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, aw_test_read(ctx, SZ_8M + SZ_2M, 8192), 0);
	KUNIT_EXPECT_EQ(test, ctx->calls[0].from, (loff_t)(SZ_8M + SZ_2M));
	KUNIT_EXPECT_EQ(test, ctx->calls[1].from, (loff_t)(SZ_8M + SZ_2M + SZ_256K));
	KUNIT_EXPECT_EQ(test, ctx->data[0], (u8)40);
	KUNIT_EXPECT_EQ(test, ctx->data[4096], (u8)41);
}

static void aw_unaligned_and_block_crossing_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, aw_test_read(ctx, 4090, 20), 0);
	KUNIT_EXPECT_EQ(test, ctx->calls[0].from, (loff_t)4090);
	KUNIT_EXPECT_EQ(test, ctx->calls[0].len, (size_t)6);
	KUNIT_EXPECT_EQ(test, ctx->calls[1].from, (loff_t)SZ_256K);
	KUNIT_EXPECT_EQ(test, ctx->calls[1].len, (size_t)14);
	ctx->ncalls = 0;
	KUNIT_ASSERT_EQ(test, aw_test_read(ctx, SZ_512K - 16, 32), 0);
	KUNIT_EXPECT_EQ(test, ctx->calls[0].from, (loff_t)(SZ_512K - 16));
	KUNIT_EXPECT_EQ(test, ctx->calls[1].from, (loff_t)SZ_512K);
	KUNIT_EXPECT_EQ(test, ctx->data[0], (u8)(1 ^ 63));
	KUNIT_EXPECT_EQ(test, ctx->data[16], (u8)2);
}

static void aw_oob_packing_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_AUTO_OOB, .oobbuf = ctx->oob,
		.ooboffs = 12, .ooblen = 32,
	};

	KUNIT_ASSERT_EQ(test, ctx->compat.mtd._read_oob(&ctx->compat.mtd, 0, &ops), 0);
	KUNIT_EXPECT_EQ(test, ops.oobretlen, (size_t)32);
	KUNIT_ASSERT_EQ(test, ctx->ncalls, 3U);
	KUNIT_EXPECT_EQ(test, ctx->calls[0].mode, (u32)MTD_OPS_PLACE_OOB);
	KUNIT_EXPECT_EQ(test, ctx->calls[0].ooboffs, 12U);
	KUNIT_EXPECT_EQ(test, ctx->calls[1].from, (loff_t)SZ_256K);
	KUNIT_EXPECT_EQ(test, ctx->calls[2].from, (loff_t)4096);
	KUNIT_EXPECT_EQ(test, ctx->oob[0], (u8)0x8c);
	KUNIT_EXPECT_EQ(test, ctx->oob[4], (u8)0x90);
	KUNIT_EXPECT_EQ(test, ctx->oob[20], (u8)0x80);
	KUNIT_EXPECT_EQ(test, ctx->oob[31], (u8)0x8b);
}

static void aw_bad_block_pair_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;
	struct mtd_info *mtd = &ctx->compat.mtd;
	loff_t sys = SZ_8M + SZ_2M;

	KUNIT_ASSERT_EQ(test, mtd->_block_isbad(mtd, sys), 0);
	KUNIT_EXPECT_EQ(test, ctx->bad_queries[0], 40U);
	KUNIT_EXPECT_EQ(test, ctx->bad_queries[1], 41U);
	KUNIT_EXPECT_EQ(test, ctx->calls[0].ooblen, (size_t)1);
	KUNIT_EXPECT_EQ(test, ctx->calls[0].mode, (u32)MTD_OPS_PLACE_OOB);
	ctx->bad_block = 41;
	KUNIT_EXPECT_EQ(test, mtd->_block_isbad(mtd, sys + 8192), 1);
	ctx->bad_block = 40;
	KUNIT_EXPECT_EQ(test, mtd->_block_isbad(mtd, sys), 1);
	ctx->bad_block = -1;
	ctx->error_block = 41;
	KUNIT_EXPECT_EQ(test, mtd->_block_isbad(mtd, sys), -ETIMEDOUT);
}

static void aw_ecc_and_partial_read_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;
	struct mtd_info *mtd = &ctx->compat.mtd;
	struct mtd_req_stats stats;
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_PLACE_OOB, .datbuf = ctx->data,
		.len = 8192, .stats = &stats,
	};

	ctx->results[0] = 4;
	ctx->results[1] = 6;
	KUNIT_ASSERT_EQ(test, mtd_read_oob(mtd, 0, &ops), -EUCLEAN);
	KUNIT_EXPECT_EQ(test, stats.corrected_bitflips, 10U);
	KUNIT_EXPECT_EQ(test, stats.max_bitflips, 6U);
	KUNIT_EXPECT_EQ(test, mtd->ecc_stats.corrected, 10U);
	ctx->ncalls = 0;
	ctx->results[0] = -EBADMSG;
	ctx->results[1] = 0;
	KUNIT_ASSERT_EQ(test, mtd_read_oob(mtd, 0, &ops), -EBADMSG);
	KUNIT_EXPECT_EQ(test, ops.retlen, (size_t)8192);
	KUNIT_EXPECT_EQ(test, ctx->ncalls, 2U);
	KUNIT_EXPECT_EQ(test, stats.uncorrectable_errors, 1U);
	ctx->ncalls = 0;
	ctx->results[0] = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, mtd_read_oob(mtd, 0, &ops), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ops.retlen, (size_t)0);
	ctx->ncalls = 0;
	ctx->results[0] = 0;
	ctx->short_at = 1;
	KUNIT_EXPECT_EQ(test, mtd_read_oob(mtd, 0, &ops), -EIO);
	KUNIT_EXPECT_EQ(test, ops.retlen, (size_t)4095);
}

static void aw_bounds_and_modes_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;
	struct mtd_info *mtd = &ctx->compat.mtd;
	struct mtd_oob_ops ops = { .mode = MTD_OPS_RAW };

	KUNIT_EXPECT_EQ(test, mtd->_read_oob(mtd, 0, &ops), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, aw_test_read(ctx, -1, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, aw_test_read(ctx, SZ_512M - 1, 2), -EINVAL);
	KUNIT_EXPECT_EQ(test, aw_test_read(ctx, SZ_512M, 0), 0);
	KUNIT_EXPECT_EQ(test, aw_test_read(ctx, SZ_512M - 1, 1), 0);
	KUNIT_EXPECT_EQ(test, ctx->calls[0].from, (loff_t)(SZ_512M - 1));
	ops = (struct mtd_oob_ops) {
		.mode = MTD_OPS_AUTO_OOB, .oobbuf = ctx->oob, .ooblen = 33,
	};
	KUNIT_EXPECT_EQ(test, mtd->_read_oob(mtd, SZ_512M - 8192, &ops), -EINVAL);
	ops.mode = MTD_OPS_PLACE_OOB;
	KUNIT_EXPECT_EQ(test, mtd->_read_oob(mtd, 0, &ops), -EOPNOTSUPP);
}

static void aw_write_rejection_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;
	struct mtd_info *mtd = &ctx->compat.mtd;
	struct spinand_device *spinand = kunit_kzalloc(test, sizeof(*spinand), GFP_KERNEL);
	struct erase_info erase = { .len = SZ_512K };
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_AUTO_OOB, .datbuf = ctx->data, .len = 8192,
	};

	KUNIT_ASSERT_NOT_NULL(test, spinand);
	KUNIT_EXPECT_EQ(test, mtd_write_oob(mtd, 0, &ops), -EROFS);
	KUNIT_EXPECT_EQ(test, mtd_erase(mtd, &erase), -EROFS);
	/* The callbacks still reject writes if an upper layer changes flags. */
	mtd->flags |= MTD_WRITEABLE;
	KUNIT_EXPECT_EQ(test, mtd->_write_oob(mtd, 0, &ops), -EROFS);
	KUNIT_EXPECT_EQ(test, mtd->_erase(mtd, &erase), -EROFS);
	KUNIT_EXPECT_EQ(test, mtd->_block_markbad(mtd, 0), -EROFS);
	spinand->aw_compat_readonly = true;
	/* spimem is NULL: reaching the bus would fail this test. */
	KUNIT_EXPECT_EQ(test, spinand_write_enable_op(spinand), -EROFS);
	KUNIT_EXPECT_EQ(test, ctx->ncalls, 0U);
}

static struct kunit_case aw_test_cases[] = {
	KUNIT_CASE(aw_geometry_test),
	KUNIT_CASE(aw_paired_pages_test),
	KUNIT_CASE(aw_sys_absolute_offset_test),
	KUNIT_CASE(aw_unaligned_and_block_crossing_test),
	KUNIT_CASE(aw_oob_packing_test),
	KUNIT_CASE(aw_bad_block_pair_test),
	KUNIT_CASE(aw_ecc_and_partial_read_test),
	KUNIT_CASE(aw_bounds_and_modes_test),
	KUNIT_CASE(aw_write_rejection_test),
	{}
};

static struct kunit_suite aw_test_suite = {
	.name = "ecm30-aw-spinand-ro",
	.init = aw_test_init,
	.test_cases = aw_test_cases,
};
kunit_test_suite(aw_test_suite);
