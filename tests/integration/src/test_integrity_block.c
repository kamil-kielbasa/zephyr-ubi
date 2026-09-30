/**
 * \file    test_integrity_block.c
 * \author  Kamil Kielbasa
 * \brief   What a damaged, forged, foreign or unreadable block does to an
 *          attach and to a read.
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
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_key.h"

/* Test headers: */
#include "flash_shim.h"
#include "forge.h"
#include "partition.h"
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

/* Static function declarations -------------------------------------------- */

/**
 * \brief Erase block \p pnum and stamp it, under the right key, with an erase
 *        counter header of a layout this build cannot address.
 */
static void stamp_foreign_layout(uint32_t pnum, uint32_t image_seq);

/* Module variables and constants ------------------------------------------ */

UBI_TEST_SUITE(ubi_integrity_block);

/* Static function definitions --------------------------------------------- */

static void stamp_foreign_layout(uint32_t pnum, uint32_t image_seq)
{
	const struct ubi_ec_header header = {
		.erase_count = 1,
		.image_seq = image_seq,
		.vid_header_offset = 2 * UBI_VID_HEADER_OFFSET,
		.data_offset = 2 * UBI_DATA_OFFSET,
	};
	psa_key_id_t key_header = PSA_KEY_ID_NULL;
	psa_key_id_t key_volume_table = PSA_KEY_ID_NULL;
	uint8_t buffer[UBI_HEADER_SIZE] = { 0 };

	zassert_ok(ubi_impl_key_derive(config.ikm_key_id, NULL, 0, &key_header,
				       &key_volume_table));
	zassert_ok(ubi_impl_header_ec_serialize(&header, key_header, pnum,
						buffer, sizeof(buffer)));
	ubi_impl_key_destroy(&key_header);
	ubi_impl_key_destroy(&key_volume_table);

	memset(saved_blocks[0], UBI_TEST_ERASED, sizeof(saved_blocks[0]));
	memcpy(saved_blocks[0], buffer, sizeof(buffer));
	block_restore(pnum, saved_blocks[0]);
}

/* Module interface function definitions ----------------------------------- */

/* Tests: damage ----------------------------------------------------------- */

/*
 * Given: a formatted device with one bit cleared in an erase counter header
 *        and nobody to repair the checksum behind it.
 * When:  it is attached.
 * Then:  the damage is reported as damage rather than tampering, the block
 *        stops counting as healthy, and the device still opens.
 */
ZTEST(ubi_integrity_block, test_a_damaged_erase_counter_header_is_reported)
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
ZTEST(ubi_integrity_block, test_a_damaged_block_that_still_holds_data_is_kept)
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

	/* Below every block the format left blank, so a reclaim would reach
	 * it first. */
	zassert_true(FIRST_BLANK_PNUM >
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

/*
 * Given: as many blocks with a damaged header and intact data as the
 *        partition tolerates, less one.
 * When:  the device is attached, and attached again after one more.
 * Then:  the first attach carries the damage and reports it; the second is
 *        one block too many and refuses rather than pretending the device
 *        is sound.
 */
ZTEST(ubi_integrity_block, test_more_damage_than_the_device_carries_loses_it)
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

	/* What types.h promises: a twentieth of the good blocks, rounded down,
	 * or eight when that is zero. */
	const uint32_t share = (info.peb_count - info.bad_pebs) / 20;
	const uint32_t allowed = (0 != share) ? share : 8;

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

/* Tests: forgery ---------------------------------------------------------- */

/*
 * Given: a block whose erase counter header was edited and its checksum
 *        repaired, which is what damage can never look like.
 * When:  the device is attached.
 * Then:  the MAC catches it and it is reported as tampering rather than
 *        damage, the block no longer counts as this device's and waits to
 *        be erased, and the surviving volume table copy still carries the
 *        device.
 */
ZTEST(ubi_integrity_block, test_a_forged_erase_counter_header_is_reported)
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
	zassert_equal(0, info.bad_pebs, "the block itself is not broken");
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
ZTEST(ubi_integrity_block,
      test_a_partition_of_forgeries_is_refused_as_unauthentic)
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
ZTEST(ubi_integrity_block,
      test_a_forged_volume_identifier_header_costs_one_block)
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

/* Tests: blocks from elsewhere -------------------------------------------- */

/*
 * Given: a device holding one block stamped, under the right key, in a
 *        layout this build cannot address, as a newer release might.
 * When:  it is attached.
 * Then:  it is refused as unsupported, rather than the block taken for blank
 *        and erased.
 */
ZTEST(ubi_integrity_block, test_a_block_of_a_newer_release_stops_the_attach)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_format(&config));
	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_device_deinit(ubi));

	stamp_foreign_layout(FIRST_BLANK_PNUM, info.image_seq);

	const uint32_t before = partition_fingerprint();

	zassert_equal(-ENOTSUP, ubi_device_init(ubi, &config));
	zassert_equal(before, partition_fingerprint());
}

/*
 * Given: a block of data an earlier format left, whose volume identifier
 *        header and data were put behind the erase counter header the same
 *        block carries in this image. Every header is authentic for that
 *        block.
 * When:  the device is attached.
 * Then:  the old data does not appear in the volume that reused its
 *        identifier: a volume identifier header has to name this image too.
 */
ZTEST(ubi_integrity_block, test_data_of_an_earlier_image_cannot_be_smuggled_in)
{
	struct ubi_device_info info = { 0 };
	struct ubi_maintenance_result result = { 0 };
	struct ubi_leb_info leb = { 0 };
	uint8_t secret[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t *const earlier_block = saved_blocks[0];
	uint8_t *const later_block = saved_blocks[1];
	uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);

	pattern_fill(secret, sizeof(secret), 0x5C);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, secret, sizeof(secret)));
	zassert_ok(ubi_device_deinit(ubi));

	const uint32_t pnum = pnum_of_data_matching(secret, sizeof(secret));

	block_save(pnum, earlier_block);

	zassert_equal(vol_id, volume_ready(UBI_TEST_VOLUME_LEBS),
		      "the new image hands out the same identifier");

	/* Every block stamped for the new image, that one included. */
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM,
				   info.reclaimable_pebs, &result));
	zassert_ok(ubi_device_deinit(ubi));

	block_save(pnum, later_block);
	memcpy(&later_block[UBI_VID_HEADER_OFFSET],
	       &earlier_block[UBI_VID_HEADER_OFFSET],
	       UBI_TEST_PEB_SIZE - UBI_VID_HEADER_OFFSET);
	block_restore(pnum, later_block);

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_leb_get_info(ubi, vol_id, 0, &leb));
	zassert_false(leb.mapped, "data of an earlier image was let in");

	zassert_ok(ubi_device_deinit(ubi));
}

#if defined(CONFIG_FLASH_SIMULATOR)

/* Tests: a block that cannot be read -------------------------------------- */

/*
 * Given: a device holding data, and a block whose reads fail, first as a
 *        whole and then only past its headers.
 * When:  the device is attached.
 * Then:  the attach fails rather than carrying on without the block, and
 *        once the reads come back the data is all there: a read error says
 *        nothing about what the block holds, so it must not cost it.
 */
ZTEST(ubi_integrity_block, test_a_block_that_cannot_be_read_stops_the_attach)
{
	uint8_t written[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);

	pattern_fill(written, sizeof(written), 0x6D);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, written, sizeof(written)));
	zassert_ok(ubi_device_deinit(ubi));

	const uint32_t pnum = pnum_of_data_matching(written, sizeof(written));

	flash_fail_reads_of(pnum);
	zassert_equal(-EIO, ubi_device_init(ubi, &config));

	flash_fail_reads_in(pnum, UBI_DATA_OFFSET, UBI_TEST_PEB_SIZE);
	zassert_equal(-EIO, ubi_device_init(ubi, &config));

	flash_fail_reads_never();

	zassert_ok(ubi_device_init(ubi, &config));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, read, sizeof(read)));
	zassert_mem_equal(written, read, sizeof(written));
	zassert_ok(ubi_device_deinit(ubi));
}

#endif /* CONFIG_FLASH_SIMULATOR */

#if defined(CONFIG_UBI_VERIFY_ON_READ)

/* Tests: a block changed while attached ----------------------------------- */

/*
 * Given: an attached device whose block is forged behind the library's back,
 *        after the attach that checked it.
 * When:  the logical block is read.
 * Then:  the read refuses rather than handing back bytes it cannot vouch
 *        for, which is what the build switch is for.
 */
ZTEST(ubi_integrity_block, test_a_read_refuses_a_block_forged_while_attached)
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

/*
 * Given: an attached device, and a block that held logical block 0 earlier
 *        and holds logical block 1 now, whose earlier contents are put back
 *        behind the library's back. The header is authentic for that block.
 * When:  logical block 1 is read.
 * Then:  the read refuses, because the header names another logical block.
 */
ZTEST(ubi_integrity_block, test_a_read_refuses_a_block_that_names_another)
{
	struct ubi_maintenance_result result = { 0 };
	uint8_t first[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t second[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t third[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	uint8_t read[UBI_TEST_PAYLOAD_SIZE] = { 0 };
	const uint32_t vol_id = volume_ready(UBI_TEST_VOLUME_LEBS);

	pattern_fill(first, sizeof(first), 0x7E);
	pattern_fill(second, sizeof(second), 0x8F);
	pattern_fill(third, sizeof(third), 0x90);

	zassert_ok(ubi_leb_change(ubi, vol_id, 0, first, sizeof(first)));

	const uint32_t pnum = pnum_of_data_matching(first, sizeof(first));

	block_save(pnum, saved_blocks[0]);

	/* The block goes back to the pool, and the next change takes it. */
	zassert_ok(ubi_leb_change(ubi, vol_id, 0, second, sizeof(second)));
	zassert_ok(ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result));
	zassert_ok(ubi_leb_change(ubi, vol_id, 1, third, sizeof(third)));
	zassert_equal(pnum, pnum_of_data_matching(third, sizeof(third)));

	block_restore(pnum, saved_blocks[0]);

	zassert_equal(-EBADMSG,
		      ubi_leb_read(ubi, vol_id, 1, 0, read, sizeof(read)),
		      "the header names logical block 0");

	zassert_ok(ubi_device_deinit(ubi));
}

#endif /* CONFIG_UBI_VERIFY_ON_READ */
