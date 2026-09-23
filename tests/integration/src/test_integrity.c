/**
 * \file    test_integrity.c
 * \author  Kamil Kielbasa
 * \brief   What a damaged or tampered partition does to an attach.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Test headers: */
#include "common.h"
#include "suite.h"

/* Module defines ---------------------------------------------------------- */

/** First block a fresh format leaves blank, behind the volume table copies. */
#define FIRST_BLANK_PNUM (UBI_VOLUME_TABLE_LEB_COUNT)

/** Erase count field of the erase counter header, 64-bit big-endian. */
#define EC_ERASE_COUNT_OFFSET (0x08)

/** Its lowest byte, which holds the whole count of a freshly stamped block. */
#define EC_ERASE_COUNT_LOW_BYTE (EC_ERASE_COUNT_OFFSET + sizeof(uint64_t) - 1)

/** Volume identifier field of the volume identifier header. */
#define VID_VOL_ID_OFFSET (0x08)

#if defined(CONFIG_FLASH_SIMULATOR)

/** Bytes of a new volume table record that reach the flash before it tears. */
#define RECORD_BYTES_WRITTEN (16)

#endif /* CONFIG_FLASH_SIMULATOR */

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_integrity);

/* Module interface function definitions ----------------------------------- */

/*
 * Given: a formatted device with one of its two volume table copies damaged.
 * When:  it is attached.
 * Then:  the surviving copy carries it through, and the device reports both
 *        the damaged record and that it is now down to one copy.
 */
ZTEST(ubi_integrity, test_one_damaged_volume_table_copy_is_survived)
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
 * Then:  there is no usable table left, so attach reports no device rather
 *        than guessing at what the volumes were.
 */
ZTEST(ubi_integrity, test_both_damaged_volume_table_copies_lose_the_device)
{
	zassert_ok(ubi_device_format(&config));
	zassert_equal(2, corrupt_volume_tables(config.ikm_key_id, 2));

	zassert_equal(-ENODEV, ubi_device_init(ubi, &config));
}

/*
 * Given: a formatted device whose volume table blocks, the only ones a
 *        format stamps, have been erased.
 * When:  it is attached.
 * Then:  nothing is left to recognise, so attach reports no device.
 */
ZTEST(ubi_integrity, test_erasing_every_stamped_block_loses_the_device)
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

/*
 * Given: a formatted device with one bit cleared in an erase counter header
 *        and nobody to repair the checksum behind it.
 * When:  it is attached.
 * Then:  the damage is reported as damage rather than tampering, the block
 *        stops counting as healthy, and the device still opens.
 */
ZTEST(ubi_integrity, test_a_damaged_erase_counter_header_is_reported)
{
	const struct flash_area *flash_area = NULL;
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));

	zassert_ok(flash_area_open(UBI_TEST_PARTITION_ID, &flash_area));
	flash_clear_a_bit(flash_area, EC_ERASE_COUNT_LOW_BYTE);
	flash_area_close(flash_area);

	events_forget();

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_equal(1, event_count[UBI_EVENT_HDR_CORRUPT]);
	zassert_equal(0, event_last[UBI_EVENT_HDR_CORRUPT].pnum);
	zassert_equal(1, event_count[UBI_EVENT_VOLUME_TABLE_DEGRADED],
		      "one copy left has to be reported as such");
	zassert_equal(0, event_count[UBI_EVENT_HDR_TAMPERED]);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.healthy_pebs, "the damaged block is not counted");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: a block whose volume identifier header was damaged but whose data
 *        area still holds what was written.
 * When:  the device is attached and maintenance is asked to run.
 * Then:  the block is kept rather than erased, because it carries the only
 *        copy of data the application may still want to salvage.
 */
ZTEST(ubi_integrity, test_a_damaged_block_that_still_holds_data_is_kept)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xD7);

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_deinit(ubi));

	/* The lowest numbered blank block, which a reclaim would reach first. */
	zassert_equal(FIRST_BLANK_PNUM,
		      pnum_of_data_matching(written, sizeof(written)));
	zassert_equal(1, corrupt_header_of_data_matching(written,
							 sizeof(written)));

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(1, info.corrupt_pebs,
		      "a block whose data survived its header is preserved");

	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));
	zassert_equal(1, result.performed);
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_REPAIR, 1, &result));
	zassert_equal(0, result.performed,
		      "a corrupt block is not a repair's to make");

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.corrupt_pebs,
		      "maintenance must not erase what it cannot read");
	zassert_equal(1, count_data_matching(written, sizeof(written)));

	zassert_ok(ubi_device_deinit(ubi));
}

/* Tests: an update cut short ---------------------------------------------- */

#if defined(CONFIG_FLASH_SIMULATOR)

/*
 * Given: a device with one volume, and a flash that stops taking writes
 *        partway through the next volume table commit.
 * When:  a second volume is created.
 * Then:  the commit fails, and the table that was there is still the one the
 *        next attach finds: a torn update never costs the volumes already on
 *        the device.
 */
ZTEST(ubi_integrity, test_an_interrupted_volume_table_update_keeps_the_old_one)
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

/* Tests: how much damage the device carries ------------------------------- */

/*
 * Given: as many blocks with a damaged header and intact data as the
 *        partition tolerates, less one.
 * When:  the device is attached, and attached again after one more.
 * Then:  the first attach carries the damage and reports it; the second is
 *        one block too many and refuses rather than pretending the device
 *        is sound.
 */
ZTEST(ubi_integrity, test_more_damage_than_the_device_carries_loses_it)
{
	struct ubi_volume_config wanted = { .name = "logs", .leb_count = 0 };
	struct ubi_device_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t bulk[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t last[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(bulk, sizeof(bulk), 0x21);
	pattern_fill(last, sizeof(last), 0x22);

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));

	const uint32_t allowed =
		MAX(info.peb_count / CORRUPT_PEB_SHARE, CORRUPT_PEB_FLOOR);

	wanted.leb_count = allowed;
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));

	for (uint32_t lnum = 0; lnum < allowed - 1; ++lnum) {
		zassert_ok(
			ubi_leb_change(ubi, vol_id, lnum, bulk, sizeof(bulk)));
	}

	zassert_ok(
		ubi_leb_change(ubi, vol_id, allowed - 1, last, sizeof(last)));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(allowed - 1,
		      corrupt_header_of_data_matching(bulk, sizeof(bulk)));

	zassert_ok(ubi_device_init(ubi, &config),
		   "%u damaged blocks are within what the device carries",
		   allowed - 1);
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(allowed - 1, info.corrupt_pebs);
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(1, corrupt_header_of_data_matching(last, sizeof(last)));

	zassert_equal(-EINVAL, ubi_device_init(ubi, &config),
		      "%u damaged blocks are one too many", allowed);
}

/* Tests: a forgery, rather than damage ------------------------------------ */

/*
 * Given: a block whose erase counter header was edited and its checksum
 *        repaired, which is what damage can never look like.
 * When:  the device is attached.
 * Then:  the MAC catches it and it is reported as tampering rather than
 *        damage, the block is taken out of service, and the surviving volume
 *        table copy still carries the device.
 */
ZTEST(ubi_integrity, test_a_forged_erase_counter_header_is_reported)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	zassert_equal(UBI_VOLUME_TABLE_LEB_COUNT, info.healthy_pebs,
		      "a format stamps the volume table blocks");

	forge_header_byte(0, EC_ERASE_COUNT_OFFSET);

	events_forget();

	zassert_ok(ubi_device_init(ubi, &config));

	zassert_equal(1, event_count[UBI_EVENT_HDR_TAMPERED],
		      "a repaired checksum leaves only the MAC to object");
	zassert_equal(0, event_count[UBI_EVENT_HDR_CORRUPT],
		      "the checksum agrees, so this is not damage");
	zassert_equal(0, event_last[UBI_EVENT_HDR_TAMPERED].pnum,
		      "the report has to name the block it came from");
	zassert_equal(1, event_count[UBI_EVENT_VOLUME_TABLE_DEGRADED]);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.bad_pebs, "a forged block is out of service");
	zassert_equal(1, info.healthy_pebs, "and is not counted as healthy");

	zassert_ok(ubi_device_deinit(ubi));
}

/*
 * Given: both volume table blocks forged, so nothing verifies under the key
 *        this build holds.
 * When:  the device is attached.
 * Then:  it is refused as unauthentic rather than as absent, because a
 *        mistyped key must never look like a blank partition.
 */
ZTEST(ubi_integrity, test_a_partition_of_forgeries_is_refused_as_unauthentic)
{
	zassert_ok(ubi_device_format(&config));

	for (uint32_t pnum = 0; pnum < UBI_VOLUME_TABLE_LEB_COUNT; ++pnum)
		forge_header_byte(pnum, EC_ERASE_COUNT_OFFSET);

	events_forget();

	zassert_equal(-EBADMSG, ubi_device_init(ubi, &config));
	zassert_equal(UBI_VOLUME_TABLE_LEB_COUNT,
		      event_count[UBI_EVENT_HDR_TAMPERED]);
}

/*
 * Given: a block of application data whose volume identifier header was
 *        forged, leaving the erase counter header in front of it intact.
 * When:  the device is attached.
 * Then:  the forgery is reported and that one block is condemned, while the
 *        device itself opens: damage behind a verified erase counter header
 *        costs one block, not the partition.
 */
ZTEST(ubi_integrity, test_a_forged_volume_identifier_header_costs_one_block)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	struct ubi_device_info info = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0x9E);

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_deinit(ubi));

	forge_header_byte(pnum_of_data_matching(written, sizeof(written)),
			  UBI_VID_HEADER_OFFSET + VID_VOL_ID_OFFSET);

	events_forget();

	zassert_ok(ubi_device_init(ubi, &config),
		   "one forged block must not cost the device");

	zassert_equal(1, event_count[UBI_EVENT_HDR_TAMPERED]);
	zassert_equal(0, event_count[UBI_EVENT_HDR_CORRUPT]);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.corrupt_pebs,
		      "the block is condemned but kept as evidence");

	zassert_ok(ubi_device_deinit(ubi));
}

#if defined(CONFIG_UBI_VERIFY_ON_READ)

/*
 * Given: an attached device whose block is forged behind the library's back,
 *        after the attach that checked it.
 * When:  the logical block is read.
 * Then:  the read refuses rather than handing back bytes it cannot vouch
 *        for, which is what the build switch is for.
 */
ZTEST(ubi_integrity, test_a_read_refuses_a_block_forged_while_attached)
{
	const struct ubi_volume_config wanted = {
		.name = "logs", .leb_count = UBI_TEST_VOLUME_LEBS
	};
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };

	pattern_fill(written, sizeof(written), 0xB4);

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_volume_create(ubi, &wanted, &vol_id));
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(written, read, sizeof(written));

	forge_header_byte(pnum_of_data_matching(written, sizeof(written)),
			  UBI_VID_HEADER_OFFSET + VID_VOL_ID_OFFSET);

	zassert_equal(-EBADMSG,
		      ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)),
		      "the seal is checked before the bytes are handed over");

	zassert_ok(ubi_device_deinit(ubi));
}

#endif /* CONFIG_UBI_VERIFY_ON_READ */
