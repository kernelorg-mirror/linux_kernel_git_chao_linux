// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for F2FS generic metadata caching layer.
 *
 * Copyright (c) 2026 Google LLC
 * Author: Chao Yu <chao@kernel.org>
 */

#include <kunit/test.h>
#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include "f2fs.h"
#include "cache.h"

struct f2fs_cache_test_ctx {
	struct f2fs_sb_info sbi;
};

static int f2fs_cache_test_init(struct kunit *test)
{
	struct f2fs_cache_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);

	ctx->sbi.blocksize = F2FS_BLKSIZE;
	f2fs_init_cache(&ctx->sbi, META_CACHE(&ctx->sbi), F2FS_META_CACHE);
	f2fs_init_cache(&ctx->sbi, NODE_CACHE(&ctx->sbi), F2FS_NODE_CACHE);
	f2fs_init_cache(&ctx->sbi, COMPRESS_CACHE(&ctx->sbi), F2FS_COMPRESS_CACHE);

	test->priv = ctx;
	return 0;
}

static void f2fs_cache_test_exit(struct kunit *test)
{
	struct f2fs_cache_test_ctx *ctx = test->priv;

	if (ctx) {
		f2fs_destroy_cache(META_CACHE(&ctx->sbi));
		f2fs_destroy_cache(NODE_CACHE(&ctx->sbi));
		f2fs_destroy_cache(COMPRESS_CACHE(&ctx->sbi));
	}
}

/* Module 1: init/destroy flow */
static void test_cache_init_destroy(struct kunit *test)
{
	struct f2fs_cache_test_ctx *ctx = test->priv;
	struct f2fs_cached_block_list cache;
	enum f2fs_cache_type type;

	for (type = F2FS_META_CACHE; type < NR_CACHE_TYPES; type++) {
		f2fs_init_cache(&ctx->sbi, &cache, type);
		KUNIT_EXPECT_EQ(test, cache.num_entries, 0UL);
		KUNIT_EXPECT_TRUE(test, list_empty(&cache.lru_list));
		KUNIT_EXPECT_PTR_EQ(test, cache.sbi, &ctx->sbi);
		KUNIT_EXPECT_EQ(test, (int)cache.type, (int)type);

		/* Empty destroy */
		f2fs_destroy_cache(&cache);
		KUNIT_EXPECT_EQ(test, cache.num_entries, 0UL);
	}

	/* Populated destroy */
	f2fs_init_cache(&ctx->sbi, &cache, F2FS_META_CACHE);
	f2fs_grab_cache(&cache, 1, F2FS_CACHE_CREATE);
	f2fs_grab_cache(&cache, 2, F2FS_CACHE_CREATE);
	KUNIT_EXPECT_EQ(test, cache.num_entries, 2UL);

	f2fs_destroy_cache(&cache);
	KUNIT_EXPECT_EQ(test, cache.num_entries, 0UL);
	KUNIT_EXPECT_TRUE(test, list_empty(&cache.lru_list));
}

/* Module 2: alloc/put lifecycle on cache entry */
static void test_cache_alloc_put_lifecycle(struct kunit *test)
{
	struct f2fs_cache_test_ctx *ctx = test->priv;
	struct f2fs_cached_block_list *cache = META_CACHE(&ctx->sbi);
	struct f2fs_cached_block *entry, *found;

	/* Grab with CREATE allocates and inserts */
	entry = f2fs_grab_cache(cache, 100, F2FS_CACHE_CREATE);
	KUNIT_ASSERT_FALSE(test, IS_ERR(entry));
	KUNIT_EXPECT_NOT_NULL(test, entry->data);
	KUNIT_EXPECT_EQ(test, entry->index, 100UL);
	KUNIT_EXPECT_EQ(test, atomic_read(&entry->refcount), 2); /* 1 tree + 1 caller */
	KUNIT_EXPECT_EQ(test, cache->num_entries, 1UL);

	/* Find bumps refcount */
	found = f2fs_find_cache(cache, 100, 0);
	KUNIT_ASSERT_PTR_EQ(test, found, entry);
	KUNIT_EXPECT_EQ(test, atomic_read(&entry->refcount), 3);

	/* Put caller reference */
	f2fs_put_cache(found, false);
	KUNIT_EXPECT_EQ(test, atomic_read(&entry->refcount), 2);

	/* Truncate drops radix tree refcount */
	f2fs_truncate_cache(entry, false);
	KUNIT_EXPECT_EQ(test, cache->num_entries, 0UL);
	KUNIT_EXPECT_EQ(test, atomic_read(&entry->refcount), 1);

	/* Final put frees entry */
	f2fs_put_cache(entry, false);

	/* Lookup after truncation must fail */
	found = f2fs_find_cache(cache, 100, 0);
	KUNIT_EXPECT_TRUE(test, IS_ERR(found));
}

/* Module 3: f2fs_cache_gang_lookup{,tag} and boundary checks */
static void test_cache_gang_lookups(struct kunit *test)
{
	struct f2fs_cache_test_ctx *ctx = test->priv;
	struct f2fs_cached_block_list *cache = META_CACHE(&ctx->sbi);
	struct f2fs_cached_block *entries[F2FS_ONSTACK_CACHES];
	struct f2fs_cached_block *entry;
	pgoff_t index;
	unsigned int nr, i;

	/* 1. Empty and inverted boundary checks */
	index = 0;
	nr = f2fs_cache_gang_lookup(cache, entries, &index, 100);
	KUNIT_EXPECT_EQ(test, nr, 0U);

	index = 10;
	nr = f2fs_cache_gang_lookup(cache, entries, &index, 10);
	KUNIT_EXPECT_EQ(test, nr, 0U);

	index = 100;
	nr = f2fs_cache_gang_lookup(cache, entries, &index, 50);
	KUNIT_EXPECT_EQ(test, nr, 0U);

	/* 2. Sub-range clamping and hole handling */
	f2fs_grab_cache(cache, 10, F2FS_CACHE_CREATE);
	f2fs_grab_cache(cache, 100, F2FS_CACHE_CREATE);

	index = 20;
	nr = f2fs_cache_gang_lookup(cache, entries, &index, 50);
	KUNIT_EXPECT_EQ(test, nr, 0U);

	index = 0;
	nr = f2fs_cache_gang_lookup(cache, entries, &index, 10);
	KUNIT_EXPECT_EQ(test, nr, 0U);

	index = 10;
	nr = f2fs_cache_gang_lookup(cache, entries, &index, 11);
	KUNIT_EXPECT_EQ(test, nr, 1U);
	KUNIT_EXPECT_EQ(test, entries[0]->index, 10UL);
	KUNIT_EXPECT_EQ(test, index, 11UL);
	f2fs_cache_gang_release(entries, nr);

	/* 3. Upper boundaries (ULONG_MAX - 1 and ULONG_MAX) */
	entry = f2fs_grab_cache(cache, ULONG_MAX, F2FS_CACHE_CREATE);
	KUNIT_EXPECT_TRUE(test, IS_ERR(entry));

	entry = f2fs_grab_cache(cache, ULONG_MAX - 1, F2FS_CACHE_CREATE);
	KUNIT_ASSERT_FALSE(test, IS_ERR(entry));

	index = ULONG_MAX - 1;
	nr = f2fs_cache_gang_lookup(cache, entries, &index, ULONG_MAX);
	KUNIT_EXPECT_EQ(test, nr, 1U);
	KUNIT_EXPECT_EQ(test, entries[0]->index, ULONG_MAX - 1);
	KUNIT_EXPECT_EQ(test, index, ULONG_MAX);
	f2fs_cache_gang_release(entries, nr);

	index = ULONG_MAX;
	nr = f2fs_cache_gang_lookup(cache, entries, &index, ULONG_MAX);
	KUNIT_EXPECT_EQ(test, nr, 0U);

	/* 4. Tag lookup at boundary */
	f2fs_mark_cache_dirty(entry);
	index = ULONG_MAX - 1;
	nr = f2fs_cache_gang_lookup_tag(cache, entries, &index, 10, F2FS_CACHE_TAG_DIRTY);
	KUNIT_EXPECT_EQ(test, nr, 1U);
	KUNIT_EXPECT_EQ(test, index, ULONG_MAX);
	f2fs_cache_gang_release(entries, nr);

	index = ULONG_MAX;
	nr = f2fs_cache_gang_lookup_tag(cache, entries, &index, 10, F2FS_CACHE_TAG_DIRTY);
	KUNIT_EXPECT_EQ(test, nr, 0U);

	/* 5. Pagination across F2FS_ONSTACK_CACHES (32) boundaries */
	f2fs_destroy_cache(cache);
	f2fs_init_cache(&ctx->sbi, cache, F2FS_META_CACHE);

	for (i = 0; i < 64; i++)
		f2fs_grab_cache(cache, i, F2FS_CACHE_CREATE);

	index = 0;
	nr = f2fs_cache_gang_lookup(cache, entries, &index, 100);
	KUNIT_EXPECT_EQ(test, nr, 32U);
	KUNIT_EXPECT_EQ(test, index, 32UL);
	f2fs_cache_gang_release(entries, nr);

	nr = f2fs_cache_gang_lookup(cache, entries, &index, 100);
	KUNIT_EXPECT_EQ(test, nr, 32U);
	KUNIT_EXPECT_EQ(test, index, 64UL);
	f2fs_cache_gang_release(entries, nr);

	nr = f2fs_cache_gang_lookup(cache, entries, &index, 100);
	KUNIT_EXPECT_EQ(test, nr, 0U);
}

/* Module 4: F2FS_CACHE_* request flags */
static void test_cache_request_flags(struct kunit *test)
{
	struct f2fs_cache_test_ctx *ctx = test->priv;
	struct f2fs_cached_block_list *cache = META_CACHE(&ctx->sbi);
	struct f2fs_cached_block *entry, *found;

	/* Non-existent without CREATE returns -ENOENT */
	entry = f2fs_grab_cache(cache, 42, 0);
	KUNIT_EXPECT_TRUE(test, IS_ERR(entry));

	/* With CREATE allocates */
	entry = f2fs_grab_cache(cache, 42, F2FS_CACHE_CREATE);
	KUNIT_ASSERT_FALSE(test, IS_ERR(entry));
	KUNIT_EXPECT_FALSE(test, f2fs_cache_test_locked(entry));

	/* With LOCK acquires bitlock */
	entry = f2fs_grab_cache(cache, 43, F2FS_CACHE_LOCK_CREATE);
	KUNIT_ASSERT_FALSE(test, IS_ERR(entry));
	KUNIT_EXPECT_TRUE(test, f2fs_cache_test_locked(entry));
	f2fs_unlock_cache(entry);

	/* ACCESS flag sets referenced */
	found = f2fs_find_cache(cache, 42, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(found));
	f2fs_put_cache(found, false);

	found = f2fs_find_cache(cache, 42, F2FS_CACHE_ACCESS);
	KUNIT_ASSERT_FALSE(test, IS_ERR(found));
	KUNIT_EXPECT_TRUE(test, f2fs_cache_test_and_clear_referenced(found));
	f2fs_put_cache(found, false);
}

/* Module 5: F2FS_BLOCK_* block states and bit-locks */
static void test_cache_block_states_and_bitlocks(struct kunit *test)
{
	struct f2fs_cache_test_ctx *ctx = test->priv;
	struct f2fs_cached_block_list *cache = META_CACHE(&ctx->sbi);
	struct f2fs_cached_block *entry;

	entry = f2fs_grab_cache(cache, 1, F2FS_CACHE_CREATE);
	KUNIT_ASSERT_FALSE(test, IS_ERR(entry));

	/* Lock / Trylock / Unlock */
	KUNIT_EXPECT_TRUE(test, f2fs_trylock_cache(entry));
	KUNIT_EXPECT_TRUE(test, f2fs_cache_test_locked(entry));
	KUNIT_EXPECT_FALSE(test, f2fs_trylock_cache(entry));
	f2fs_unlock_cache(entry);
	KUNIT_EXPECT_FALSE(test, f2fs_cache_test_locked(entry));

	/* Uptodate */
	KUNIT_EXPECT_FALSE(test, f2fs_cache_test_uptodate(entry));
	f2fs_cache_set_uptodate(entry);
	KUNIT_EXPECT_TRUE(test, f2fs_cache_test_uptodate(entry));
	f2fs_cache_clear_uptodate(entry);
	KUNIT_EXPECT_FALSE(test, f2fs_cache_test_uptodate(entry));

	/* Dirty */
	KUNIT_EXPECT_FALSE(test, f2fs_cache_test_dirty(entry));
	f2fs_mark_cache_dirty(entry);
	KUNIT_EXPECT_TRUE(test, f2fs_cache_test_dirty(entry));
	KUNIT_EXPECT_TRUE(test, f2fs_cache_test_and_clear_dirty(entry));
	KUNIT_EXPECT_FALSE(test, f2fs_cache_test_dirty(entry));

	/* Inline */
	f2fs_cache_set_inline(entry);
	KUNIT_EXPECT_TRUE(test, f2fs_cache_test_inline(entry));
	f2fs_cache_clear_inline(entry);
	KUNIT_EXPECT_FALSE(test, f2fs_cache_test_inline(entry));

	/* Referenced */
	KUNIT_EXPECT_FALSE(test, f2fs_cache_test_and_set_referenced(entry));
	KUNIT_EXPECT_TRUE(test, f2fs_cache_test_and_set_referenced(entry));
	KUNIT_EXPECT_TRUE(test, f2fs_cache_test_and_clear_referenced(entry));
	KUNIT_EXPECT_FALSE(test, f2fs_cache_test_and_clear_referenced(entry));
}

/* Module 6: f2fs_writeback flow */
static void test_cache_writeback_flow(struct kunit *test)
{
	struct f2fs_cache_test_ctx *ctx = test->priv;
	struct f2fs_cached_block_list *cache = META_CACHE(&ctx->sbi);
	struct f2fs_cached_block *entries[F2FS_ONSTACK_CACHES];
	struct f2fs_cached_block *entry;
	pgoff_t index = 0;
	unsigned int nr;

	entry = f2fs_grab_cache(cache, 10, F2FS_CACHE_CREATE);
	KUNIT_ASSERT_FALSE(test, IS_ERR(entry));

	/* Mark dirty: Tag becomes DIRTY */
	f2fs_mark_cache_dirty(entry);
	index = 0;
	nr = f2fs_cache_gang_lookup_tag(cache, entries, &index, 10, F2FS_CACHE_TAG_DIRTY);
	KUNIT_EXPECT_EQ(test, nr, 1U);
	f2fs_cache_gang_release(entries, nr);

	/* Start writeback: Tag transitions to WRITEBACK */
	f2fs_start_cache_writeback(entry);
	KUNIT_EXPECT_TRUE(test, f2fs_cache_test_writeback(entry));

	index = 0;
	nr = f2fs_cache_gang_lookup_tag(cache, entries, &index, 10, F2FS_CACHE_TAG_DIRTY);
	KUNIT_EXPECT_EQ(test, nr, 0U);

	index = 0;
	nr = f2fs_cache_gang_lookup_tag(cache, entries, &index, 10, F2FS_CACHE_TAG_WRITEBACK);
	KUNIT_EXPECT_EQ(test, nr, 1U);
	f2fs_cache_gang_release(entries, nr);

	/* End writeback: Tag cleared */
	f2fs_end_cache_writeback(entry);
	KUNIT_EXPECT_FALSE(test, f2fs_cache_test_writeback(entry));

	index = 0;
	nr = f2fs_cache_gang_lookup_tag(cache, entries, &index, 10, F2FS_CACHE_TAG_WRITEBACK);
	KUNIT_EXPECT_EQ(test, nr, 0U);
}

/* Module 7: shrink flow & 2nd-chance LRU clock */
static void test_cache_shrinker_and_aging(struct kunit *test)
{
	struct f2fs_cache_test_ctx *ctx = test->priv;
	struct f2fs_cached_block_list *cache = META_CACHE(&ctx->sbi);
	struct f2fs_cached_block *e1, *e2, *e3, *e4;
	struct f2fs_cached_block *found;
	unsigned long freed;

	e1 = f2fs_grab_cache(cache, 1, F2FS_CACHE_CREATE);
	e2 = f2fs_grab_cache(cache, 2, F2FS_CACHE_CREATE);
	e3 = f2fs_grab_cache(cache, 3, F2FS_CACHE_CREATE);
	e4 = f2fs_grab_cache(cache, 4, F2FS_CACHE_CREATE);

	/* Release caller references so refcount == 1 (reclaimable) */
	f2fs_put_cache(e1, false);
	f2fs_put_cache(e2, false);
	f2fs_put_cache(e3, false);
	f2fs_put_cache(e4, false);

	/* Touch entries 1 and 3 with ACCESS flag */
	found = f2fs_find_cache(cache, 1, F2FS_CACHE_ACCESS);
	f2fs_put_cache(found, false);
	found = f2fs_find_cache(cache, 3, F2FS_CACHE_ACCESS);
	f2fs_put_cache(found, false);

	/* Scan all 4 entries: 1 and 3 rotated to tail, 2 and 4 freed */
	freed = f2fs_shrink_cache(&ctx->sbi, 4);
	KUNIT_EXPECT_EQ(test, freed, 2UL);

	/* Entries 2 and 4 should be freed, 1 and 3 survived via 2nd-chance */
	KUNIT_EXPECT_TRUE(test, IS_ERR(f2fs_find_cache(cache, 2, 0)));
	KUNIT_EXPECT_TRUE(test, IS_ERR(f2fs_find_cache(cache, 4, 0)));

	found = f2fs_find_cache(cache, 1, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(found));
	f2fs_put_cache(found, false);

	found = f2fs_find_cache(cache, 3, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(found));
	f2fs_put_cache(found, false);

	/* Second shrink pass frees 1 and 3 */
	freed = f2fs_shrink_cache(&ctx->sbi, 4);
	KUNIT_EXPECT_EQ(test, freed, 2UL);
	KUNIT_EXPECT_EQ(test, cache->num_entries, 0UL);
}

/* Module 8: truncate and range drops */
static void test_cache_truncate_and_range_drops(struct kunit *test)
{
	struct f2fs_cache_test_ctx *ctx = test->priv;
	struct f2fs_cached_block_list *cache = META_CACHE(&ctx->sbi);
	struct f2fs_cached_block *entry;
	int i;

	/* 1. drop_dirty false vs true */
	entry = f2fs_grab_cache(cache, 1, F2FS_CACHE_CREATE);
	f2fs_mark_cache_dirty(entry);
	f2fs_put_cache(entry, false);

	/* drop_dirty == false preserves dirty entry */
	f2fs_drop_cache_range(cache, 1, 1, false);
	KUNIT_EXPECT_FALSE(test, IS_ERR(f2fs_find_cache(cache, 1, 0)));

	/* drop_dirty == true drops dirty entry */
	f2fs_drop_cache_range(cache, 1, 1, true);
	KUNIT_EXPECT_TRUE(test, IS_ERR(f2fs_find_cache(cache, 1, 0)));

	/* 2. Sub-range drop */
	for (i = 0; i < 50; i++) {
		entry = f2fs_grab_cache(cache, i, F2FS_CACHE_CREATE);
		f2fs_put_cache(entry, false);
	}
	KUNIT_EXPECT_EQ(test, cache->num_entries, 50UL);

	/* Drop range [10, 30) */
	f2fs_drop_cache_range(cache, 10, 20, true);
	KUNIT_EXPECT_EQ(test, cache->num_entries, 30UL);
	KUNIT_EXPECT_FALSE(test, IS_ERR(f2fs_find_cache(cache, 9, 0)));
	KUNIT_EXPECT_TRUE(test, IS_ERR(f2fs_find_cache(cache, 15, 0)));
	KUNIT_EXPECT_FALSE(test, IS_ERR(f2fs_find_cache(cache, 30, 0)));

	/* 3. Full range drop (ULONG_MAX) */
	f2fs_drop_cache_range(cache, 0, ULONG_MAX, true);
	KUNIT_EXPECT_EQ(test, cache->num_entries, 0UL);
}

/* Module 9: race conditions between shrink, truncate, find, grab, get, put */
struct stress_worker_arg {
	struct f2fs_cache_test_ctx *ctx;
};

static int shrink_worker_fn(void *data)
{
	struct stress_worker_arg *arg = data;

	while (!kthread_should_stop()) {
		f2fs_shrink_cache(&arg->ctx->sbi, 16);
		cond_resched();
	}
	return 0;
}

static int truncate_worker_fn(void *data)
{
	struct stress_worker_arg *arg = data;
	unsigned long start = 0;

	while (!kthread_should_stop()) {
		start = (start + 7) % 64;
		f2fs_drop_cache_range(META_CACHE(&arg->ctx->sbi), start, 8, true);
		cond_resched();
	}
	return 0;
}

static int grab_put_worker_fn(void *data)
{
	struct stress_worker_arg *arg = data;
	unsigned long idx = 0;

	while (!kthread_should_stop()) {
		struct f2fs_cached_block *e;

		idx = (idx + 1) % 64;
		e = f2fs_grab_cache(META_CACHE(&arg->ctx->sbi), idx, F2FS_CACHE_LOCK_CREATE);
		if (!IS_ERR(e)) {
			f2fs_cache_set_uptodate(e);
			f2fs_put_cache(e, true);
		}

		e = f2fs_find_cache(META_CACHE(&arg->ctx->sbi), idx, F2FS_CACHE_ACCESS);
		if (!IS_ERR(e))
			f2fs_put_cache(e, false);

		cond_resched();
	}
	return 0;
}

static void test_cache_concurrent_races(struct kunit *test)
{
	struct f2fs_cache_test_ctx *ctx = test->priv;
	struct stress_worker_arg arg = { .ctx = ctx };
	struct task_struct *t_shrink, *t_trunc, *t_grab1, *t_grab2;

	t_shrink = kthread_run(shrink_worker_fn, &arg, "kunit_f2fs_shrink");
	KUNIT_ASSERT_FALSE(test, IS_ERR(t_shrink));
	get_task_struct(t_shrink);

	t_trunc = kthread_run(truncate_worker_fn, &arg, "kunit_f2fs_trunc");
	KUNIT_ASSERT_FALSE(test, IS_ERR(t_trunc));
	get_task_struct(t_trunc);

	t_grab1 = kthread_run(grab_put_worker_fn, &arg, "kunit_f2fs_grab1");
	KUNIT_ASSERT_FALSE(test, IS_ERR(t_grab1));
	get_task_struct(t_grab1);

	t_grab2 = kthread_run(grab_put_worker_fn, &arg, "kunit_f2fs_grab2");
	KUNIT_ASSERT_FALSE(test, IS_ERR(t_grab2));
	get_task_struct(t_grab2);

	/* Run concurrent stress for 10s */
	msleep(10000);

	kthread_stop(t_shrink);
	put_task_struct(t_shrink);

	kthread_stop(t_trunc);
	put_task_struct(t_trunc);

	kthread_stop(t_grab1);
	put_task_struct(t_grab1);

	kthread_stop(t_grab2);
	put_task_struct(t_grab2);

	/* Final cleanup must be sound */
	f2fs_destroy_cache(META_CACHE(&ctx->sbi));
	f2fs_init_cache(&ctx->sbi, META_CACHE(&ctx->sbi), F2FS_META_CACHE);
	KUNIT_EXPECT_EQ(test, META_CACHE(&ctx->sbi)->num_entries, 0UL);
}

static struct kunit_case f2fs_cache_test_cases[] = {
	KUNIT_CASE(test_cache_init_destroy),
	KUNIT_CASE(test_cache_alloc_put_lifecycle),
	KUNIT_CASE(test_cache_gang_lookups),
	KUNIT_CASE(test_cache_request_flags),
	KUNIT_CASE(test_cache_block_states_and_bitlocks),
	KUNIT_CASE(test_cache_writeback_flow),
	KUNIT_CASE(test_cache_shrinker_and_aging),
	KUNIT_CASE(test_cache_truncate_and_range_drops),
	KUNIT_CASE(test_cache_concurrent_races),
	{}
};

static struct kunit_suite f2fs_cache_test_suite = {
	.name = "f2fs_cache_test",
	.init = f2fs_cache_test_init,
	.exit = f2fs_cache_test_exit,
	.test_cases = f2fs_cache_test_cases,
};
kunit_test_suite(f2fs_cache_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for F2FS Metadata Cache");
MODULE_AUTHOR("Chao Yu <chao@kernel.org>");
