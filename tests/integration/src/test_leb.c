/**
 * \file    test_leb.c
 * \author  Kamil Kielbasa
 * \brief   Mapping, reading and writing logical erase blocks.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "common.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

/** Payload small enough to sit on the stack, large enough to span writes. */
#define PAYLOAD_SIZE (512)

/* Static function definitions --------------------------------------------- */

/**
 * \brief Format, attach and create one volume to work in.
 */
static uint32_t volume_ready(uint32_t leb_count)
{
	const struct ubi_volume_config wanted = { .name = "logs",
						  .leb_count = leb_count };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	return vol_id;
}

/**
 * \brief Blocks currently backing a logical one, derived from the rest.
 */
static uint32_t mapped_pebs(const struct ubi_device_info *info)
{
	return info->peb_count - info->free_pebs - info->reclaimable_pebs -
	       info->bad_pebs;
}

/**
 * \brief Fill a buffer with a recognisable, position-dependent pattern.
 */
static void pattern_fill(uint8_t *buffer, size_t length, uint8_t seed)
{
	for (size_t i = 0; i < length; ++i)
		buffer[i] = (uint8_t)(seed + i);
}

/* Tests: the mapping ------------------------------------------------------ */

ZTEST(ubi_integration, test_an_unmapped_block_reads_as_erased)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_leb_info info = { 0 };
	uint8_t buffer[PAYLOAD_SIZE];

	memset(buffer, 0x00, sizeof(buffer));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped, "a fresh volume maps nothing");
	zassert_equal(0, info.erase_count);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, buffer, sizeof(buffer)));

	for (size_t i = 0; i < sizeof(buffer); ++i)
		zassert_equal(0xFF, buffer[i], "byte %zu was not erased", i);

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_mapping_gives_an_empty_block)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_leb_info info = { 0 };
	uint8_t buffer[PAYLOAD_SIZE];

	memset(buffer, 0x00, sizeof(buffer));

	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_ok(ubi_leb_map(ubi, vol_id, 1));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_true(after.global_sqnum > before.global_sqnum,
		     "mapping seals a header, so it spends a sequence number");

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 1, &info));
	zassert_true(info.mapped);

	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, buffer, sizeof(buffer)));
	zassert_equal(0xFF, buffer[0]);
	zassert_equal(0xFF, buffer[sizeof(buffer) - 1]);

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_mapping_a_mapped_block_is_refused)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };

	zassert_ok(ubi_leb_map(ubi, vol_id, 0));
	zassert_ok(ubi_device_get_info(ubi, &before));

	/* Mapping is a transition, not a state to assert, so asking for it
	 * twice is a mistake worth reporting. Linux says the same with
	 * -EBADMSG, which here means a failed authentication. */
	zassert_equal(-EEXIST, ubi_leb_map(ubi, vol_id, 0));

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.global_sqnum, after.global_sqnum,
		      "a refusal must not write anything");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_unmapping_queues_the_block_for_reclaim)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	struct ubi_leb_info info = { 0 };

	zassert_ok(ubi_leb_map(ubi, vol_id, 0));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_equal(before.reclaimable_pebs + 1, after.reclaimable_pebs);

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped);

	/* Idempotent, exactly like the contract says. */
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_mapping_survives_a_reattach)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_leb_info info = { 0 };

	zassert_ok(ubi_leb_map(ubi, vol_id, 2));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 2, &info));
	zassert_true(info.mapped, "the mapping has to be rebuilt from flash");

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 1, &info));
	zassert_false(info.mapped);

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: writing through the mapping -------------------------------------- */

ZTEST(ubi_integration, test_a_change_is_readable_after_a_reattach)
{
	const uint32_t vol_id = volume_ready(4);
	uint8_t written[PAYLOAD_SIZE];
	uint8_t read[PAYLOAD_SIZE];

	pattern_fill(written, sizeof(written), 0x11);
	memset(read, 0x00, sizeof(read));

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));

	zassert_mem_equal(written, read, sizeof(written));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_change_leaves_the_old_block_until_reclaim)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint8_t old[PAYLOAD_SIZE];
	uint8_t fresh[PAYLOAD_SIZE];
	uint8_t read[PAYLOAD_SIZE];

	pattern_fill(old, sizeof(old), 0x22);
	pattern_fill(fresh, sizeof(fresh), 0x33);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_equal(mapped_pebs(&before), mapped_pebs(&after),
		      "one logical block may occupy only one physical one");

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(fresh, read, sizeof(fresh),
			  "the LEB has to show the new contents");

	zassert_ok(ubi_device_deinit(ubi));

	/* Queued for reclaim, not erased: both blocks are still on the flash
	 * and the old one stays readable to anyone with raw access. */
	zassert_equal(1, count_data_matching(fresh, sizeof(fresh)));
	zassert_equal(1, count_data_matching(old, sizeof(old)));
}

ZTEST(ubi_integration, test_writing_nothing_does_nothing)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint8_t written[PAYLOAD_SIZE];
	uint8_t read[PAYLOAD_SIZE];

	pattern_fill(written, sizeof(written), 0x44);
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_get_info(ubi, &before));

	/* A length of zero is a degenerate argument, not an instruction to
	 * throw the block away. Spending an erase on it would make an empty
	 * loop wear the flash out. */
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, NULL, 0));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, 0, NULL, 0));

	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_equal(before.global_sqnum, after.global_sqnum);

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(written, read, sizeof(written),
			  "the contents had to stay exactly as they were");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_an_append_maps_the_block_it_needs)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_leb_info info = { 0 };
	uint8_t written[PAYLOAD_SIZE];
	uint8_t read[PAYLOAD_SIZE];

	pattern_fill(written, sizeof(written), 0x55);

	/* No ubi_leb_map first: the write takes a block on its own, exactly
	 * as ubi_eba_write_leb() does in Linux. */
	zassert_ok(
		ubi_leb_write_at(ubi, vol_id, 0, 0, written, sizeof(written)));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_true(info.mapped);

	/* A second record, where the caller says, with nothing in between. */
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, sizeof(written), written,
				    sizeof(written)));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, sizeof(written), read,
				sizeof(read)));
	zassert_mem_equal(written, read, sizeof(written));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_range_past_the_end_of_a_block_is_refused)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_device_info device = { 0 };
	uint8_t buffer[PAYLOAD_SIZE];

	pattern_fill(buffer, sizeof(buffer), 0x66);
	zassert_ok(ubi_device_get_info(ubi, &device));
	zassert_ok(ubi_leb_map(ubi, vol_id, 0));

	zassert_equal(-EINVAL, ubi_leb_read(ubi, vol_id, 0, device.leb_size - 1,
					    buffer, sizeof(buffer)));
	zassert_equal(-EINVAL, ubi_leb_write_at(ubi, vol_id, 0, device.leb_size,
						buffer, sizeof(buffer)));
	zassert_equal(-EINVAL, ubi_leb_change(ubi, vol_id, 0, buffer,
					      device.leb_size + 1));

	/* Exactly the whole block still has to go through. */
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, device.leb_size - PAYLOAD_SIZE,
				buffer, sizeof(buffer)));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_a_block_the_volume_does_not_reach_is_refused)
{
	const uint32_t vol_id = volume_ready(2);
	struct ubi_leb_info info = { 0 };
	uint8_t buffer[PAYLOAD_SIZE];

	pattern_fill(buffer, sizeof(buffer), 0x77);

	zassert_equal(-EINVAL, ubi_leb_map(ubi, vol_id, 2));
	zassert_equal(-EINVAL, ubi_leb_unmap(ubi, vol_id, 2));
	zassert_equal(-EINVAL, ubi_leb_get_info(ubi, vol_id, 2, &info));
	zassert_equal(-EINVAL,
		      ubi_leb_read(ubi, vol_id, 2, 0, buffer, sizeof(buffer)));
	zassert_equal(-EINVAL,
		      ubi_leb_change(ubi, vol_id, 2, buffer, sizeof(buffer)));

	zassert_equal(-ENOENT, ubi_leb_map(ubi, vol_id + 1, 0));

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: what a power loss may leave behind -------------------------------- */

ZTEST(ubi_integration, test_an_interrupted_change_leaves_the_old_contents)
{
	const uint32_t vol_id = volume_ready(4);
	uint8_t old[PAYLOAD_SIZE];
	uint8_t fresh[PAYLOAD_SIZE];
	uint8_t read[PAYLOAD_SIZE];

	pattern_fill(old, sizeof(old), 0x88);
	pattern_fill(fresh, sizeof(fresh), 0x99);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, old, sizeof(old)));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_device_deinit(ubi));

	/* Standing in for a power loss between the header and the last byte
	 * of data: the header promises a checksum the data no longer has. */
	zassert_equal(1, corrupt_data_matching(fresh, sizeof(fresh)));

	memset(event_seen, 0, sizeof(event_seen));
	zassert_ok(ubi_device_init(ubi, &config));

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));

	zassert_mem_equal(old, read, sizeof(old),
			  "a cut-short write must not replace what was there");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_an_interrupted_first_change_leaves_nothing)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_leb_info info = { 0 };
	uint8_t fresh[PAYLOAD_SIZE];
	uint8_t read[PAYLOAD_SIZE];

	pattern_fill(fresh, sizeof(fresh), 0xA5);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, fresh, sizeof(fresh)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(1, corrupt_data_matching(fresh, sizeof(fresh)));

	zassert_ok(ubi_device_init(ubi, &config));

	/* There was no older copy to fall back to, so the block has to read
	 * as one nobody ever wrote. */
	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped);

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_equal(0xFF, read[0]);

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_an_interrupted_append_is_the_callers_problem)
{
	const uint32_t vol_id = volume_ready(4);
	uint8_t written[PAYLOAD_SIZE];
	uint8_t read[PAYLOAD_SIZE];

	pattern_fill(written, sizeof(written), 0xC3);

	zassert_ok(ubi_leb_map(ubi, vol_id, 0));
	zassert_ok(
		ubi_leb_write_at(ubi, vol_id, 0, 0, written, sizeof(written)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(1, corrupt_data_matching(written, sizeof(written)));

	zassert_ok(ubi_device_init(ubi, &config));

	/* No copy_flag, so UBI promised nothing about these bytes and hands
	 * back exactly what is on the flash. */
	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_not_equal(0, memcmp(written, read, sizeof(written)));

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: getting rid of a block ------------------------------------------- */

ZTEST(ubi_integration, test_an_unmap_may_not_survive_a_reboot)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_leb_info info = { 0 };
	uint8_t written[PAYLOAD_SIZE];

	pattern_fill(written, sizeof(written), 0x1D);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped);

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	/*
	 * Unmapping writes nothing, so the block still carries a header
	 * naming this LEB and the attach maps it back. Linux UBI documents
	 * the same behaviour; ubi_leb_erase() is the durable way.
	 */
	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_true(info.mapped, "unmap alone does not reach the flash");
	zassert_equal(1, count_data_matching(written, sizeof(written)));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_an_erase_takes_the_contents_off_the_flash)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_leb_info info = { 0 };
	uint8_t written[PAYLOAD_SIZE];

	pattern_fill(written, sizeof(written), 0x2E);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_equal(1, count_data_matching(written, sizeof(written)));

	zassert_ok(ubi_leb_erase(ubi, vol_id, 0));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped);
	zassert_equal(0, count_data_matching(written, sizeof(written)),
		      "the contents have to be gone from the flash itself");

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &info));
	zassert_false(info.mapped, "and it has to stay gone across a reboot");

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_an_erase_returns_the_block_to_the_free_pool)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint8_t written[PAYLOAD_SIZE];

	pattern_fill(written, sizeof(written), 0x3F);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_get_info(ubi, &before));

	zassert_ok(ubi_leb_erase(ubi, vol_id, 0));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_equal(before.free_pebs + 1, after.free_pebs,
		      "an erased block is allocatable without another erase");
	zassert_true(after.total_erase_count > before.total_erase_count);

	/* Idempotent, like unmap. */
	zassert_ok(ubi_leb_erase(ubi, vol_id, 0));

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: what the write block size imposes -------------------------------- */

ZTEST(ubi_integration, test_a_length_that_is_not_a_write_block_is_refused)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_device_info device = { 0 };
	uint8_t written[PAYLOAD_SIZE];
	uint8_t read[PAYLOAD_SIZE];

	pattern_fill(written, sizeof(written), 0xD1);
	zassert_ok(ubi_device_get_info(ubi, &device));

	/* A flash that writes single bytes has no unaligned length to refuse,
	 * so there the same call is simply accepted. */
	const int expected = (1 == device.write_block_size) ? 0 : -EINVAL;

	zassert_equal(expected, ubi_leb_change(ubi, vol_id, 0, written,
					       sizeof(written) - 1));
	zassert_equal(expected, ubi_leb_write_at(ubi, vol_id, 1, 0, written,
						 sizeof(written) - 1));

	/* Whole write blocks always go through. */
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(ubi_device_init(ubi, &config));

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(written, read, sizeof(written));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_an_offset_that_is_not_a_write_block_is_refused)
{
	const uint32_t vol_id = volume_ready(4);
	struct ubi_device_info device = { 0 };
	uint8_t written[PAYLOAD_SIZE];

	pattern_fill(written, sizeof(written), 0xE2);
	zassert_ok(ubi_device_get_info(ubi, &device));

	const uint32_t block = device.write_block_size;
	const int expected = (1 == block) ? 0 : -EINVAL;

	zassert_equal(expected,
		      ubi_leb_write_at(ubi, vol_id, 0, 1, written, block),
		      "a misaligned offset decides where the next write may "
		      "start, so UBI will not take it");

	zassert_ok(ubi_leb_write_at(ubi, vol_id, 1, block, written, block));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST(ubi_integration, test_an_append_after_a_change_survives_the_attach)
{
	const uint32_t vol_id = volume_ready(4);
	uint8_t changed[PAYLOAD_SIZE];
	uint8_t appended[PAYLOAD_SIZE];
	uint8_t read[PAYLOAD_SIZE];

	pattern_fill(changed, sizeof(changed), 0x4A);
	pattern_fill(appended, sizeof(appended), 0x5B);

	/* The sealed header promises a checksum over the first write only, so
	 * the attach has to accept bytes written past what it claims. */
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, changed, sizeof(changed)));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, 0, sizeof(changed), appended,
				    sizeof(appended)));

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(changed, read, sizeof(changed));

	memset(read, 0x00, sizeof(read));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, sizeof(changed), read,
				sizeof(read)));
	zassert_mem_equal(appended, read, sizeof(appended));

	zassert_ok(ubi_device_deinit(ubi));
}
