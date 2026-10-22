// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the driver against physical page fixtures, without NAND hardware. */
#include <kunit/test.h>
#include <linux/mtd/spinand.h>
#include <linux/sizes.h>

#include "aw-compat.h"

struct aw_test_call {
	loff_t from;
	size_t len, ooblen;
	u32 ooboffs, mode;
	u8 first, last, spare[16];
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
	struct aw_test_call writes[16];
	loff_t erases[8];
	int write_results[16], erase_results[8];
	unsigned int nwrites, nerases, short_write_at;
};

static int aw_fake_write(struct mtd_info *mtd, loff_t to,
			 struct mtd_oob_ops *ops)
{
	struct aw_test_ctx *ctx = container_of(mtd, struct aw_test_ctx, physical);
	unsigned int n = ctx->nwrites++;
	struct aw_test_call *call;

	if (n >= ARRAY_SIZE(ctx->writes))
		return -EIO;
	call = &ctx->writes[n];
	*call = (struct aw_test_call) {
		.from = to, .len = ops->len, .ooblen = ops->ooblen,
		.ooboffs = ops->ooboffs, .mode = ops->mode,
	};
	if (ops->len) {
		call->first = ops->datbuf[0];
		call->last = ops->datbuf[ops->len - 1];
	}
	if (ops->ooblen <= sizeof(call->spare) && ops->ooblen)
		memcpy(call->spare, ops->oobbuf, ops->ooblen);
	if (ctx->write_results[n])
		return ctx->write_results[n];
	ops->retlen = ops->len;
	ops->oobretlen = ops->ooblen;
	if (ctx->short_write_at == n + 1 && ops->retlen)
		ops->retlen--;
	return 0;
}

static int aw_fake_erase(struct mtd_info *mtd, struct erase_info *info)
{
	struct aw_test_ctx *ctx = container_of(mtd, struct aw_test_ctx, physical);
	unsigned int n = ctx->nerases++;

	if (n >= ARRAY_SIZE(ctx->erases) || info->len != SZ_256K)
		return -EIO;
	ctx->erases[n] = info->addr;
	return ctx->erase_results[n];
}

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
		._read_oob = aw_fake_read, ._write_oob = aw_fake_write,
		._erase = aw_fake_erase,
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
	KUNIT_EXPECT_TRUE(test, mtd->flags & MTD_WRITEABLE);
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

static void aw_backend_requirements_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;

	ctx->physical._write_oob = NULL;
	KUNIT_EXPECT_EQ(test, aw_spinand_compat_init(&ctx->compat, &ctx->physical), -EINVAL);
	ctx->physical._write_oob = aw_fake_write;
	ctx->physical._erase = NULL;
	KUNIT_EXPECT_EQ(test, aw_spinand_compat_init(&ctx->compat, &ctx->physical), -EINVAL);
}

static void aw_write_mapping_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;
	struct mtd_info *mtd = &ctx->compat.mtd;
	loff_t start = SZ_8M + SZ_2M + SZ_512K - 8192;
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_PLACE_OOB, .datbuf = ctx->data, .len = 16384,
	};
	unsigned int i;

	KUNIT_ASSERT_EQ(test, aw_spinand_compat_init(&ctx->compat, &ctx->physical), 0);
	for (i = 0; i < 4; i++)
		memset(ctx->data + i * 4096, 0x30 + i, 4096);
	KUNIT_ASSERT_EQ(test, mtd_write_oob(mtd, start, &ops), 0);
	KUNIT_EXPECT_EQ(test, ops.retlen, (size_t)16384);
	KUNIT_ASSERT_EQ(test, ctx->nwrites, 4U);
	KUNIT_EXPECT_EQ(test, ctx->writes[0].from, start - SZ_256K + 4096);
	KUNIT_EXPECT_EQ(test, ctx->writes[1].from, start + 4096);
	KUNIT_EXPECT_EQ(test, ctx->writes[2].from, start + 8192);
	KUNIT_EXPECT_EQ(test, ctx->writes[3].from, start + 8192 + SZ_256K);
	for (i = 0; i < 4; i++) {
		KUNIT_EXPECT_EQ(test, ctx->writes[i].len, (size_t)4096);
		KUNIT_EXPECT_EQ(test, ctx->writes[i].first, (u8)(0x30 + i));
		KUNIT_EXPECT_EQ(test, ctx->writes[i].last, (u8)(0x30 + i));
	}
}

static void aw_write_subpage_oob_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;
	struct mtd_info *mtd = &ctx->compat.mtd;
	loff_t start = SZ_8M + SZ_2M;
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_PLACE_OOB, .datbuf = ctx->data, .len = 4096,
	};

	KUNIT_ASSERT_EQ(test, aw_spinand_compat_init(&ctx->compat, &ctx->physical), 0);
	KUNIT_ASSERT_EQ(test, mtd_write_oob(mtd, start + 4096, &ops), 0);
	KUNIT_EXPECT_EQ(test, ctx->nwrites, 1U);
	KUNIT_EXPECT_EQ(test, ctx->writes[0].from, start + SZ_256K);
	memset(ctx->oob, 0xa1, 16);
	memset(ctx->oob + 16, 0xb2, 16);
	ops = (struct mtd_oob_ops) {
		.mode = MTD_OPS_AUTO_OOB, .oobbuf = ctx->oob, .ooblen = 32,
	};
	KUNIT_ASSERT_EQ(test, mtd_write_oob(mtd, start + 8192, &ops), 0);
	KUNIT_ASSERT_EQ(test, ctx->nwrites, 3U);
	KUNIT_EXPECT_EQ(test, ops.oobretlen, (size_t)32);
	KUNIT_EXPECT_EQ(test, ctx->writes[1].from, start + 4096);
	KUNIT_EXPECT_EQ(test, ctx->writes[2].from, start + SZ_256K + 4096);
	KUNIT_EXPECT_EQ(test, ctx->writes[1].spare[0], (u8)0xa1);
	KUNIT_EXPECT_EQ(test, ctx->writes[2].spare[15], (u8)0xb2);
	KUNIT_EXPECT_EQ(test, ctx->writes[1].mode, (u32)MTD_OPS_PLACE_OOB);
}

static void aw_write_failure_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;
	struct mtd_info *mtd = &ctx->compat.mtd;
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_PLACE_OOB, .datbuf = ctx->data, .len = 8192,
	};

	KUNIT_ASSERT_EQ(test, aw_spinand_compat_init(&ctx->compat, &ctx->physical), 0);
	ctx->write_results[1] = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, mtd_write_oob(mtd, SZ_8M + SZ_2M, &ops), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ops.retlen, (size_t)4096);
	KUNIT_EXPECT_EQ(test, ctx->nwrites, 2U);
	ctx->nwrites = 0;
	ctx->short_write_at = 1;
	KUNIT_EXPECT_EQ(test, mtd_write_oob(mtd, SZ_8M + SZ_2M, &ops), -EIO);
	KUNIT_EXPECT_EQ(test, ops.retlen, (size_t)4095);
	KUNIT_EXPECT_EQ(test, ctx->nwrites, 1U);
}

static void aw_write_protection_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;
	struct mtd_info *mtd = &ctx->compat.mtd;
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_PLACE_OOB, .datbuf = ctx->data, .len = 4096,
	};
	struct erase_info erase = { .addr = SZ_8M, .len = SZ_512K };
	loff_t start = SZ_8M + SZ_2M;

	KUNIT_ASSERT_EQ(test, aw_spinand_compat_init(&ctx->compat, &ctx->physical), 0);
	KUNIT_EXPECT_EQ(test, mtd->_write_oob(mtd, SZ_8M, &ops), -EINVAL);
	KUNIT_EXPECT_EQ(test, mtd->_write_oob(mtd, start + 1, &ops), -EINVAL);
	KUNIT_EXPECT_EQ(test, mtd->_write_oob(mtd, SZ_512M, &ops), -EINVAL);
	ops.len = 16;
	KUNIT_EXPECT_EQ(test, mtd->_write_oob(mtd, start, &ops), -EINVAL);
	ops.mode = MTD_OPS_RAW;
	KUNIT_EXPECT_EQ(test, mtd->_write_oob(mtd, start, &ops), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, mtd->_erase(mtd, &erase), -EINVAL);
	KUNIT_EXPECT_EQ(test, mtd->_block_markbad(mtd, SZ_8M), -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->nwrites, 0U);
	KUNIT_EXPECT_EQ(test, ctx->nerases, 0U);
}

static void aw_erase_pair_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;
	struct mtd_info *mtd = &ctx->compat.mtd;
	loff_t start = SZ_8M + SZ_2M;
	struct erase_info erase = { .addr = start, .len = SZ_1M };
	unsigned int i;

	KUNIT_ASSERT_EQ(test, aw_spinand_compat_init(&ctx->compat, &ctx->physical), 0);
	KUNIT_ASSERT_EQ(test, mtd_erase(mtd, &erase), 0);
	KUNIT_ASSERT_EQ(test, ctx->nerases, 4U);
	for (i = 0; i < 4; i++)
		KUNIT_EXPECT_EQ(test, ctx->erases[i], start + i * SZ_256K);
	ctx->nerases = 0;
	ctx->erase_results[1] = -EIO;
	KUNIT_EXPECT_EQ(test, mtd_erase(mtd, &erase), -EIO);
	KUNIT_EXPECT_EQ(test, erase.fail_addr, (u64)start);
	KUNIT_EXPECT_EQ(test, ctx->nerases, 2U);
}

static void aw_erase_bad_block_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;
	struct mtd_info *mtd = &ctx->compat.mtd;
	struct erase_info erase = { .addr = SZ_8M + SZ_2M, .len = SZ_512K };

	KUNIT_ASSERT_EQ(test, aw_spinand_compat_init(&ctx->compat, &ctx->physical), 0);
	ctx->bad_block = 41;
	KUNIT_EXPECT_EQ(test, mtd_erase(mtd, &erase), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->nerases, 0U);
	KUNIT_EXPECT_EQ(test, erase.fail_addr, (u64)(SZ_8M + SZ_2M));
}

static void aw_markbad_pair_test(struct kunit *test)
{
	struct aw_test_ctx *ctx = test->priv;
	struct mtd_info *mtd = &ctx->compat.mtd;
	loff_t start = SZ_8M + SZ_2M;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, aw_spinand_compat_init(&ctx->compat, &ctx->physical), 0);
	ctx->erase_results[0] = -EIO;
	KUNIT_ASSERT_EQ(test, mtd->_block_markbad(mtd, start), 0);
	KUNIT_ASSERT_EQ(test, ctx->nerases, 2U);
	KUNIT_ASSERT_EQ(test, ctx->nwrites, 4U);
	KUNIT_EXPECT_EQ(test, ctx->writes[0].from, start);
	KUNIT_EXPECT_EQ(test, ctx->writes[1].from, start + SZ_256K - 4096);
	KUNIT_EXPECT_EQ(test, ctx->writes[2].from, start + SZ_256K);
	KUNIT_EXPECT_EQ(test, ctx->writes[3].from, start + SZ_512K - 4096);
	for (i = 0; i < 4; i++) {
		KUNIT_EXPECT_EQ(test, ctx->writes[i].ooblen, (size_t)16);
		KUNIT_EXPECT_EQ(test, ctx->writes[i].spare[0], (u8)0);
		KUNIT_EXPECT_EQ(test, ctx->writes[i].spare[15], (u8)0);
	}
	KUNIT_EXPECT_EQ(test, mtd->ecc_stats.badblocks, 1U);
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
	KUNIT_CASE(aw_backend_requirements_test),
	KUNIT_CASE(aw_write_mapping_test),
	KUNIT_CASE(aw_write_subpage_oob_test),
	KUNIT_CASE(aw_write_failure_test),
	KUNIT_CASE(aw_write_protection_test),
	KUNIT_CASE(aw_erase_pair_test),
	KUNIT_CASE(aw_erase_bad_block_test),
	KUNIT_CASE(aw_markbad_pair_test),
	{}
};

static struct kunit_suite aw_test_suite = {
	.name = "ecm30-aw-spinand",
	.init = aw_test_init,
	.test_cases = aw_test_cases,
};
kunit_test_suite(aw_test_suite);
