/**
 * \file    test_integrity_table.c
 * \author  Kamil Kielbasa
 * \brief   What a damaged, foreign or unreadable volume table does to an
 *          attach.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdint.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_key.h"

/* Test headers: */
#include "flash_faults.h"
#include "flash_shim.h"
#include "flash_stats.h"
#include "forge.h"
#include "partition.h"
#include "suite.h"
#include "table_copies.h"

/* Module defines ---------------------------------------------------------- */

/** Version byte of a volume table record. */
#define RECORD_VERSION_OFFSET (0x04)

/** Volume count of a volume table record, 32-bit big-endian. */
#define RECORD_VOLUME_COUNT_OFFSET (0x1C)

/** Room for any record a test forges: one entry more than the build takes. */
#define RECORD_CAPACITY \
	(UBI_VOLUME_TABLE_RECORD_MAX_SIZE + UBI_VOLUME_TABLE_ENTRY_SIZE)

/** How far relocation may round a record up, on any flash UBI attaches. */
#define RECORD_WRITE_SIZE \
	ROUND_UP(UBI_VOLUME_TABLE_RECORD_MAX_SIZE, UBI_HEADER_SIZE)

/** A byte of the erased tail past any record, where a bit error can land. */
#define RECORD_STRAY_OFFSET (RECORD_WRITE_SIZE + UBI_TEST_WRITE_BLOCK)

#if defined(CONFIG_FLASH_SIMULATOR)

/** Bytes of a new volume table record that reach the flash before it tears. */
#define RECORD_BYTES_WRITTEN (16)

#endif /* CONFIG_FLASH_SIMULATOR */

/* Static function declarations -------------------------------------------- */

/**
 * \brief Find both volume table copies, failing unless there are two.
 */
static void volume_table_copies(uint32_t pnums[UBI_VOLUME_TABLE_LEB_COUNT]);

/**
 * \brief Fail unless the device attaches with \p count volumes, and a record
 *        at \p revision.
 */
static void attach_check(uint32_t count, uint32_t revision);

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_integrity_table);

/* Static function definitions --------------------------------------------- */

static void volume_table_copies(uint32_t pnums[UBI_VOLUME_TABLE_LEB_COUNT])
{
	zassert_equal(UBI_VOLUME_TABLE_LEB_COUNT,
		      volume_table_blocks(config.ikm_key_id, pnums,
					  UBI_VOLUME_TABLE_LEB_COUNT));
}

static void attach_check(uint32_t count, uint32_t revision)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(count, info.volume_count);
	zassert_equal(revision, info.revision);
	zassert_ok(ubi_device_deinit(ubi));
}

/* Module interface function definitions ----------------------------------- */

/* Tests: a copy lost, or both --------------------------------------------- */

/*
 * Given: a formatted device with one of its two volume table copies damaged.
 * When:  it is attached.
 * Then:  the surviving copy carries it through, and the device reports both
 *        the damaged record and that it is now down to one copy.
 */
ZTEST(ubi_integrity_table, test_one_damaged_volume_table_copy_is_survived)
{
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &before));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(1, corrupt_volume_tables(config.ikm_key_id, 1));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &after));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(before.image_seq, after.image_seq);
	zassert_equal(before.revision, after.revision);
	zassert_equal(1, event_count[UBI_EVENT_VOLUME_TABLE_DEGRADED]);

	/* Damage and forgery of a record are told apart by nothing. */
	zassert_equal(1, event_count[UBI_EVENT_VOLUME_TABLE_CORRUPT]);
	zassert_equal(0, event_last[UBI_EVENT_VOLUME_TABLE_CORRUPT].pnum,
		      "the report has to name the block the first copy is in");
	zassert_equal(0, event_count[UBI_EVENT_HDR_TAMPERED],
		      "the headers were not touched");
}

/*
 * Given: a formatted device with both volume table copies damaged.
 * When:  it is attached.
 * Then:  there is no usable table left, so attach refuses; and it does not
 *        report a blank partition, because formatting in response would
 *        destroy what the damage left.
 */
ZTEST(ubi_integrity_table,
      test_both_damaged_volume_table_copies_are_not_blank_flash)
{
	zassert_ok(ubi_device_format(&config));
	zassert_equal(2, corrupt_volume_tables(config.ikm_key_id, 2));

	zassert_equal(-EBADMSG, ubi_device_init(ubi, &config));
}

/*
 * Given: a device holding data, whose volume table blocks were erased.
 * When:  it is attached.
 * Then:  attach refuses and does not report a blank partition: the data is
 *        still there, and a format would throw it away.
 */
ZTEST(ubi_integrity_table, test_data_without_a_volume_table_is_not_blank_flash)
{
	const struct flash_area *flash_area = NULL;
	uint32_t pnums[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);

	pattern_fill(written, sizeof(written), 0x3A);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_deinit(ubi));

	volume_table_copies(pnums);

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	for (uint32_t i = 0; i < ARRAY_SIZE(pnums); ++i) {
		zassert_ok(flash_area_erase(flash_area,
					    (off_t)pnums[i] * UBI_TEST_PEB_SIZE,
					    UBI_TEST_PEB_SIZE));
	}

	flash_area_close(flash_area);

	zassert_equal(-EBADMSG, ubi_device_init(ubi, &config));
}

/*
 * Given: a formatted device whose volume table blocks, the only ones a
 *        format stamps, have been erased.
 * When:  it is attached.
 * Then:  nothing is left to recognise, so attach reports no device.
 */
ZTEST(ubi_integrity_table, test_erasing_every_stamped_block_loses_the_device)
{
	const struct flash_area *flash_area = NULL;
	const uint32_t blank = partition_fingerprint();

	zassert_ok(ubi_device_format(&config));

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	for (uint32_t pnum = 0; pnum < UBI_VOLUME_TABLE_LEB_COUNT; ++pnum) {
		zassert_ok(flash_area_erase(flash_area,
					    (off_t)pnum * UBI_TEST_PEB_SIZE,
					    UBI_TEST_PEB_SIZE));
	}

	flash_area_close(flash_area);

	zassert_equal(blank, partition_fingerprint(),
		      "a format stamps the volume table blocks and nothing "
		      "else");

	zassert_equal(-ENODEV, ubi_device_init(ubi, &config));
}

/* Tests: what a record may carry ------------------------------------------ */

/*
 * Given: a device whose volume table copies carry a record version newer
 *        than this build reads, authenticated under the right key.
 * When:  it is attached.
 * Then:  it is refused as unsupported rather than as absent, so a firmware
 *        rolled back over a newer image does not format it away.
 */
ZTEST(ubi_integrity_table, test_a_newer_volume_table_is_not_blank_flash)
{
	uint32_t pnums[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint8_t record[RECORD_CAPACITY] = { 0 };

	zassert_ok(ubi_device_format(&config));

	volume_table_copies(pnums);

	for (uint32_t i = 0; i < ARRAY_SIZE(pnums); ++i) {
		const size_t length = volume_table_record_read(
			config.ikm_key_id, pnums[i], record, sizeof(record));

		record[RECORD_VERSION_OFFSET] += 1;
		volume_table_record_forge(config.ikm_key_id, pnums[i], record,
					  length);
	}

	zassert_equal(-ENOTSUP, ubi_device_init(ubi, &config));
}

/*
 * Given: a device whose volume table lists one volume more than this build
 *        allows, authenticated under the right key.
 * When:  it is attached.
 * Then:  it is refused as unsupported rather than as absent, so shrinking
 *        CONFIG_UBI_MAX_NR_OF_VOLUMES in an update does not format it away.
 */
ZTEST(ubi_integrity_table,
      test_more_volumes_than_the_build_allows_is_not_blank_flash)
{
	const uint32_t count = CONFIG_UBI_MAX_NR_OF_VOLUMES + 1;
	const size_t length = UBI_VOLUME_TABLE_PREAMBLE_SIZE +
			      count * UBI_VOLUME_TABLE_ENTRY_SIZE +
			      UBI_MAC_SIZE;
	uint32_t pnums[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint8_t record[RECORD_CAPACITY] = { 0 };

	zassert_true(length <= sizeof(record));

	zassert_ok(ubi_device_format(&config));

	volume_table_copies(pnums);

	for (uint32_t i = 0; i < ARRAY_SIZE(pnums); ++i) {
		const size_t held = volume_table_record_read(
			config.ikm_key_id, pnums[i], record, sizeof(record));

		zassert_true(UBI_VOLUME_TABLE_PREAMBLE_SIZE < held);

		memset(&record[UBI_VOLUME_TABLE_PREAMBLE_SIZE], 0,
		       length - UBI_VOLUME_TABLE_PREAMBLE_SIZE);
		sys_put_be32(count, &record[RECORD_VOLUME_COUNT_OFFSET]);

		volume_table_record_forge(config.ikm_key_id, pnums[i], record,
					  length);
	}

	zassert_equal(-ENOTSUP, ubi_device_init(ubi, &config));
}

/*
 * Given: volume table copies resealed as relocation may seal them: short of
 *        their record, past it to a write block, and past any record over a
 *        bit error in the erased tail.
 * When:  the device is attached after each.
 * Then:  both copies read every time with nothing to report: the record's
 *        own length decides what it is, not the seal.
 */
ZTEST(ubi_integrity_table, test_a_seal_that_misses_the_record_still_attaches)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	const struct flash_area *flash_area = NULL;
	uint32_t pnums[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint8_t record[RECORD_CAPACITY] = { 0 };
	struct ubi_device_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	volume_table_copies(pnums);
	events_forget();

	/* Short: a record whose MAC ends in an erased byte. */
	for (uint32_t i = 0; i < ARRAY_SIZE(pnums); ++i) {
		const size_t length = volume_table_record_read(
			config.ikm_key_id, pnums[i], record, sizeof(record));

		volume_table_reseal(config.ikm_key_id, pnums[i], length - 1);
	}

	attach_check(1, info.revision);

	/* Long: rounded up to the widest write block. */
	for (uint32_t i = 0; i < ARRAY_SIZE(pnums); ++i)
		volume_table_reseal(config.ikm_key_id, pnums[i],
				    RECORD_WRITE_SIZE);

	attach_check(1, info.revision);

	/* Past any record, up to a bit error in the erased tail. */
	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));

	for (uint32_t i = 0; i < ARRAY_SIZE(pnums); ++i) {
		flash_clear_a_bit(flash_area,
				  (off_t)pnums[i] * UBI_TEST_PEB_SIZE +
					  UBI_DATA_OFFSET +
					  RECORD_STRAY_OFFSET);
	}

	flash_area_close(flash_area);

	for (uint32_t i = 0; i < ARRAY_SIZE(pnums); ++i)
		volume_table_reseal(config.ikm_key_id, pnums[i],
				    RECORD_STRAY_OFFSET + UBI_TEST_WRITE_BLOCK);

	attach_check(1, info.revision);

	zassert_equal(0, events_total);
}

/* Tests: copies another image left ---------------------------------------- */

/*
 * Given: a device one of whose volume table copies was replaced by the copy
 *        of the same number an earlier format left, authentic for its
 *        block, with no other block erased and waiting.
 * When:  the device is attached, the block that copy sits in is handed out
 *        for data, and the table is repaired.
 * Then:  the repair does not erase that data: the table forgets a copy that
 *        belongs to another image rather than keeping a claim on its block.
 */
ZTEST(ubi_integrity_table, test_an_earlier_images_table_copy_never_costs_data)
{
	psa_key_id_t key_header = PSA_KEY_ID_NULL;
	psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info earlier = { 0 };
	uint32_t earlier_copies[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint32_t later_copies[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x4B);

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &earlier));
	zassert_ok(ubi_device_deinit(ubi));

	volume_table_copy_blocks(config.ikm_key_id, earlier_copies);

	for (uint32_t lnum = 0; lnum < UBI_VOLUME_TABLE_LEB_COUNT; ++lnum)
		block_save(earlier_copies[lnum], saved_blocks[lnum]);

	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);

	zassert_ok(ubi_device_deinit(ubi));

	volume_table_copy_blocks(config.ikm_key_id, later_copies);

	/* The earlier copy goes back to its own block, which the later copy
	 * of the other number must not be in. */
	const uint32_t replaced = (earlier_copies[1] != later_copies[0]) ? 1 :
									   0;
	const uint32_t other = 1 - replaced;
	const uint32_t pnum = earlier_copies[replaced];

	zassert_not_equal(pnum, later_copies[other]);

	/* Worn less than anything else, so it is the next block handed out. */
	const struct ubi_ec_header least_worn = {
		.erase_count = 0,
		.image_seq = earlier.image_seq,
		.vid_header_offset = UBI_VID_HEADER_OFFSET,
		.data_offset = UBI_DATA_OFFSET,
	};

	zassert_ok(ubi_impl_key_derive(config.ikm_key_id, NULL, 0, &key_header,
				       &key_volume_table));
	zassert_ok(ubi_impl_header_ec_serialize(&least_worn, key_header, pnum,
						saved_blocks[replaced],
						UBI_HEADER_SIZE));
	ubi_impl_key_destroy(&key_header);
	ubi_impl_key_destroy(&key_volume_table);

	/* Nothing but the other later copy and the earlier one is left. */
	block_save(later_copies[other], saved_blocks[other]);
	partition_erase_dirty();
	block_restore(later_copies[other], saved_blocks[other]);
	block_restore(pnum, saved_blocks[replaced]);

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_equal(pnum, pnum_of_data_matching(written, sizeof(written)),
		      "the data has to land where the earlier copy was");
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(written, read, sizeof(written),
			  "the repair wrote over data");

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(written, read, sizeof(written));

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a device whose newer volume table copy holds a later record under
 *        a header naming another image, every part of it authentic, and
 *        whose older copy holds the record before it.
 * When:  the device is attached, the blocks waiting for reclaim are erased,
 *        and it is attached again.
 * Then:  both attaches serve the older record: the copy refused lends the
 *        one adopted nothing.
 */
ZTEST(ubi_integrity_table,
      test_a_refused_table_copy_lends_the_adopted_one_nothing)
{
	const struct ubi_volume_config second = {
		.name = "second", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info earlier = { 0 };
	struct ubi_device_info info = { 0 };
	uint32_t older[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint32_t newer[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint32_t created = UBI_VOL_ID_INVALID;

	zassert_not_equal(UBI_VOL_ID_INVALID,
			  volume_ready(UBI_TEST_VOLUME_LEBS));
	zassert_ok(ubi_device_get_info(ubi, &earlier));
	zassert_ok(ubi_device_deinit(ubi));

	volume_table_copy_blocks(config.ikm_key_id, older);

	for (uint32_t lnum = 0; lnum < UBI_VOLUME_TABLE_LEB_COUNT; ++lnum)
		block_save(older[lnum], saved_blocks[lnum]);

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &second, &created));
	zassert_ok(ubi_device_deinit(ubi));

	volume_table_copy_blocks(config.ikm_key_id, newer);

	/* One older copy goes back to its own block, which the newer copy of
	 * the other number must not be in. */
	const uint32_t kept = (older[1] != newer[0]) ? 1 : 0;
	const uint32_t refused = 1 - kept;

	zassert_not_equal(older[kept], newer[refused]);

	stamp_erase_count(config.ikm_key_id, newer[kept], earlier.image_seq, 1);
	block_restore(older[kept], saved_blocks[kept]);
	vid_image_rewrite(config.ikm_key_id, newer[refused],
			  earlier.image_seq + 1);

	events_forget();

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(earlier.revision, info.revision,
		      "the record of the copy refused was served");
	zassert_equal(earlier.volume_count, info.volume_count);
	zassert_equal(1, event_count[UBI_EVENT_VOLUME_TABLE_CORRUPT]);
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM,
				   info.reclaimable_pebs, &result));
	zassert_ok(ubi_device_deinit(ubi));

	attach_check(earlier.volume_count, earlier.revision);
}

#if defined(CONFIG_FLASH_SIMULATOR)

/* Tests: an update cut short, or unreadable ------------------------------- */

/*
 * Given: a device whose newer volume table copy holds a later record than
 *        the older one, and reads of one copy that fail past its headers.
 * When:  the device is attached.
 * Then:  an older copy that cannot be read costs nothing, but a newer one
 *        stops the attach rather than let the older one stand in for it,
 *        and once the reads come back the later record is served.
 */
ZTEST(ubi_integrity_table, test_an_unreadable_newer_table_copy_stops_the_attach)
{
	const struct ubi_volume_config second = {
		.name = "second", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	const struct flash_area *flash_area = NULL;
	struct ubi_device_info later = { 0 };
	uint32_t earlier[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint32_t newer[UBI_VOLUME_TABLE_LEB_COUNT] = { 0 };
	uint32_t created = UBI_VOL_ID_INVALID;

	zassert_not_equal(UBI_VOL_ID_INVALID,
			  volume_ready(UBI_TEST_VOLUME_LEBS));
	zassert_ok(ubi_device_deinit(ubi));

	volume_table_copy_blocks(config.ikm_key_id, earlier);
	block_save(earlier[1], saved_blocks[0]);

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &second, &created));
	zassert_ok(ubi_device_get_info(ubi, &later));
	zassert_ok(ubi_device_deinit(ubi));

	/* Each later copy went into a fresh block while the earlier second
	 * copy still held its own. Put that one back in place of the later
	 * second copy. */
	volume_table_copy_blocks(config.ikm_key_id, newer);
	zassert_true(earlier[1] != newer[0] && earlier[1] != newer[1]);

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));
	zassert_ok(flash_area_erase(flash_area,
				    (off_t)newer[1] * UBI_TEST_PEB_SIZE,
				    UBI_TEST_PEB_SIZE));
	flash_area_close(flash_area);

	block_restore(earlier[1], saved_blocks[0]);

	flash_fail_reads_in(earlier[1], UBI_DATA_OFFSET, UBI_TEST_PEB_SIZE);
	attach_check(later.volume_count, later.revision);

	flash_fail_reads_in(newer[0], UBI_DATA_OFFSET, UBI_TEST_PEB_SIZE);
	zassert_equal(-EIO, ubi_device_init(ubi, &config),
		      "the older copy stood in for one that could not be read");

	flash_fail_reads_never();
	attach_check(later.volume_count, later.revision);
}

/*
 * Given: a device with one volume, and a flash that stops taking writes
 *        partway through the next volume table commit.
 * When:  a second volume is created.
 * Then:  the commit fails, and the table that was there is still the one the
 *        next attach finds: a torn update never costs the volumes already on
 *        the device.
 */
ZTEST(ubi_integrity_table,
      test_an_interrupted_volume_table_update_keeps_the_old_one)
{
	const struct ubi_volume_config first = {
		.name = "first", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	const struct ubi_volume_config second = {
		.name = "second", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_device_info before = { 0 };
	struct ubi_device_info after = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &first, &vol_id));
	zassert_ok(ubi_device_get_info(ubi, &before));

	flash_ops_forget();
	flash_fail_writes_after(UBI_DATA_OFFSET + RECORD_BYTES_WRITTEN);

	zassert_equal(-EIO, ubi_volume_create(ubi, &second, &vol_id));

	flash_fail_writes_never();

	zassert_true(0 < flash_ops("bytes_written"),
		     "the point was to tear a write, not to refuse one");

	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &after));

	zassert_ok(ubi_volume_find(ubi, first.name, &vol_id),
		   "the volume that was committed has to still be there");
	zassert_equal(-ENOENT, ubi_volume_find(ubi, second.name, &vol_id),
		      "and the one whose commit was torn must not appear");
	zassert_equal(before.volume_count, after.volume_count);

	zassert_ok(ubi_device_deinit(ubi));
}

#endif /* CONFIG_FLASH_SIMULATOR */
