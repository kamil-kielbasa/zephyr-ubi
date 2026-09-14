/**
 * \file    ubi_volume.c
 * \author  Kamil Kielbasa
 * \brief   Volumes and the logical-to-physical mapping.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stddef.h>
#include <string.h>

/* Zephyr headers: */
#include <zephyr/logging/log.h>

/* UBI headers: */
#include "ubi_peb.h"
#include "ubi_private.h"
#include "ubi_volume.h"
#include "ubi_volume_table.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/**
 * Blocks the volumes may not have.
 *
 * Two of them hold the volume table. The third is what keeps a fully
 * provisioned device usable: ubi_leb_change() writes the new contents before
 * it switches the mapping, so with every block mapped there would be nowhere
 * to write and nothing could ever be rewritten again.
 */
#define UBI_RESERVED_PEB_COUNT (UBI_VOLUME_TABLE_LEB_COUNT + 1)

/* Static function declarations -------------------------------------------- */

/**
 * \brief Logical blocks the volumes may share out between them.
 */
static uint32_t volumes_leb_budget(const struct ubi_device *ubi);

/**
 * \brief One past the last mapping any volume owns.
 */
static uint16_t *volumes_eba_end(const struct ubi_device *ubi);

/**
 * \brief Write the volumes as they stand into a record, one revision on.
 */
static void record_from_volumes(const struct ubi_device *ubi,
				struct ubi_volume_table_record *record);

/**
 * \brief Adopt a record that is now on the flash, keeping the mappings the
 *        volumes that survived it already had.
 */
static void volumes_adopt(struct ubi_device *ubi,
			  const struct ubi_volume_table_record *record);

/**
 * \brief Drop a volume from the volume list, closing the gap it leaves in the
 *        shared mapping table.
 */
static void volumes_remove_at(struct ubi_device *ubi, uint32_t index);

/**
 * \brief Give one volume's slice of the shared mapping table a new length,
 *        sliding everything behind it out of the way or into the gap.
 */
static void volumes_resize_at(struct ubi_device *ubi, uint32_t index,
			      uint32_t leb_count);

/**
 * \brief Find a volume by identifier, reporting where in the list it sits.
 */
static int volumes_index_of(const struct ubi_device *ubi, uint32_t vol_id,
			    uint32_t *index);

/**
 * \brief Reject a name no volume may carry, this device's own included.
 */
static int volume_name_validate(const struct ubi_device *ubi, const char *name);

/* Static function definitions --------------------------------------------- */

static uint32_t volumes_leb_budget(const struct ubi_device *ubi)
{
	if (UBI_RESERVED_PEB_COUNT >= ubi->geometry.peb_count)
		return 0;

	return ubi->geometry.peb_count - UBI_RESERVED_PEB_COUNT;
}

static uint16_t *volumes_eba_end(const struct ubi_device *ubi)
{
	return ubi->volumes.eba_pool + ubi->volumes.eba_used;
}

static void record_from_volumes(const struct ubi_device *ubi,
				struct ubi_volume_table_record *record)
{
	memset(record, 0, sizeof(*record));

	record->revision = ubi->volumes.revision + 1;
	record->image_seq = ubi->image_seq;
	record->peb_size = ubi->geometry.peb_size;
	record->peb_count = ubi->geometry.peb_count;
	record->vol_id_watermark = ubi->volumes.id_watermark;
	record->volume_count = ubi->volumes.count;

	for (uint32_t i = 0; i < ubi->volumes.count; ++i) {
		const struct ubi_volume *volume = &ubi->volumes.entries[i];
		struct ubi_volume_table_entry *entry = &record->entries[i];

		entry->vol_id = volume->vol_id;
		entry->leb_count = volume->leb_count;
		strcpy(entry->name, volume->name);
	}
}

static void volumes_adopt(struct ubi_device *ubi,
			  const struct ubi_volume_table_record *record)
{
	ubi->volumes.revision = record->revision;
	ubi->volumes.id_watermark = record->vol_id_watermark;
}

static void volumes_remove_at(struct ubi_device *ubi, uint32_t index)
{
	struct ubi_volume *volume = &ubi->volumes.entries[index];
	const uint32_t hole = volume->leb_count;
	uint16_t *tail = volume->eba + hole;
	const size_t tail_length = (size_t)(volumes_eba_end(ubi) - tail);

	/* The volumes behind it keep their mappings, so those move down with
	 * them rather than being rebuilt. */
	memmove(volume->eba, tail, tail_length * sizeof(*tail));

	for (uint32_t i = index + 1; i < ubi->volumes.count; ++i) {
		ubi->volumes.entries[i - 1] = ubi->volumes.entries[i];
		ubi->volumes.entries[i - 1].eba -= hole;
	}

	ubi->volumes.count -= 1;
	ubi->volumes.eba_used -= hole;

	memset(&ubi->volumes.entries[ubi->volumes.count], 0,
	       sizeof(ubi->volumes.entries[0]));
}

static void volumes_resize_at(struct ubi_device *ubi, uint32_t index,
			      uint32_t leb_count)
{
	struct ubi_volume *volume = &ubi->volumes.entries[index];
	const uint32_t was = volume->leb_count;
	uint16_t *tail = volume->eba + was;
	const size_t tail_length = (size_t)(volumes_eba_end(ubi) - tail);
	const ptrdiff_t shift = (ptrdiff_t)leb_count - (ptrdiff_t)was;

	/* Growing pushes the tail into itself, so this has to be a memmove. */
	memmove(tail + shift, tail, tail_length * sizeof(*tail));

	if (0 < shift)
		memset(tail, 0xFF, (size_t)shift * sizeof(*tail));

	for (uint32_t i = index + 1; i < ubi->volumes.count; ++i)
		ubi->volumes.entries[i].eba += shift;

	volume->leb_count = leb_count;
	ubi->volumes.eba_used -= was;
	ubi->volumes.eba_used += leb_count;
}

static int volumes_index_of(const struct ubi_device *ubi, uint32_t vol_id,
			    uint32_t *index)
{
	for (uint32_t i = 0; i < ubi->volumes.count; ++i) {
		if (ubi->volumes.entries[i].vol_id != vol_id)
			continue;

		*index = i;

		return 0;
	}

	return -ENOENT;
}

static int volume_name_validate(const struct ubi_device *ubi, const char *name)
{
	/* Bounded on purpose: a name without a terminator inside the length a
	 * volume may have is not a name this device can store. */
	const char *end = memchr(name, '\0', UBI_VOLUME_NAME_MAX_LEN + 1);

	if (NULL == end)
		return -EINVAL;

	const size_t length = (size_t)(end - name);

	if (0 == length)
		return -EINVAL;

	if (0 == strcmp(name, UBI_VOLUME_TABLE_NAME))
		return -EINVAL;

	for (uint32_t i = 0; i < ubi->volumes.count; ++i) {
		if (0 == strcmp(ubi->volumes.entries[i].name, name))
			return -EEXIST;
	}

	return 0;
}

/* Module interface function definitions ----------------------------------- */

uint32_t ubi_volumes_leb_free(const struct ubi_device *ubi)
{
	return volumes_leb_budget(ubi) - ubi->volumes.eba_used;
}

struct ubi_volume *ubi_volume_by_id(struct ubi_device *ubi, uint32_t vol_id)
{
	if (UBI_VOLUME_TABLE_VOL_ID == vol_id)
		return &ubi->volume_table.volume;

	for (uint32_t i = 0; i < ubi->volumes.count; ++i) {
		if (ubi->volumes.entries[i].vol_id == vol_id)
			return &ubi->volumes.entries[i];
	}

	return NULL;
}

int ubi_volumes_build(struct ubi_device *ubi,
		      const struct ubi_volume_table_record *record)
{
	ubi->volumes.count = record->volume_count;
	ubi->volumes.id_watermark = record->vol_id_watermark;
	ubi->volumes.revision = record->revision;
	ubi->volumes.eba_used = 0;

	for (uint32_t i = 0; i < record->volume_count; ++i) {
		const struct ubi_volume_table_entry *entry =
			&record->entries[i];
		struct ubi_volume *volume = &ubi->volumes.entries[i];

		if (entry->leb_count >
		    volumes_leb_budget(ubi) - ubi->volumes.eba_used)
			return -ENOSPC;

		volume->vol_id = entry->vol_id;
		volume->leb_count = entry->leb_count;
		strcpy(volume->name, entry->name);
		volume->eba = &ubi->volumes.eba_pool[ubi->volumes.eba_used];

		ubi->volumes.eba_used += entry->leb_count;
	}

	return 0;
}

int ubi_volume_leb_get(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		       uint16_t *pnum)
{
	const struct ubi_volume *volume = ubi_volume_by_id(ubi, vol_id);

	if (NULL == volume)
		return -ENOENT;

	if (lnum >= volume->leb_count)
		return -ERANGE;

	*pnum = volume->eba[lnum];

	return 0;
}

int ubi_volume_leb_set(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		       uint16_t pnum)
{
	struct ubi_volume *volume = ubi_volume_by_id(ubi, vol_id);

	if (NULL == volume)
		return -ENOENT;

	if (lnum >= volume->leb_count)
		return -ERANGE;

	volume->eba[lnum] = pnum;

	return 0;
}

int ubi_volume_add(struct ubi_device *ubi,
		   const struct ubi_volume_config *config, uint32_t *vol_id)
{
	int ret = volume_name_validate(ubi, config->name);

	if (0 != ret) {
		LOG_ERR("\"%s\" is not a name this device can give a volume "
			"(%d)",
			config->name, ret);
		return ret;
	}

	if (CONFIG_UBI_MAX_NR_OF_VOLUMES == ubi->volumes.count) {
		LOG_ERR("the device already holds the %d volumes this build "
			"allows",
			CONFIG_UBI_MAX_NR_OF_VOLUMES);
		return -ENOSPC;
	}

	if (config->leb_count >
	    volumes_leb_budget(ubi) - ubi->volumes.eba_used) {
		LOG_ERR("\"%s\" asks for %u logical blocks and %u of the %u "
			"this partition can share out are left",
			config->name, config->leb_count,
			volumes_leb_budget(ubi) - ubi->volumes.eba_used,
			volumes_leb_budget(ubi));
		return -ENOSPC;
	}

	if (UBI_VOLUME_TABLE_VOL_ID <= ubi->volumes.id_watermark) {
		LOG_ERR("this device has handed out every volume identifier "
			"it has");
		return -ENOSPC;
	}

	struct ubi_volume_table_record record = { 0 };
	struct ubi_volume_table_entry *entry = NULL;

	record_from_volumes(ubi, &record);

	entry = &record.entries[record.volume_count];
	entry->vol_id = ubi->volumes.id_watermark;
	entry->leb_count = config->leb_count;
	strcpy(entry->name, config->name);

	record.volume_count += 1;
	record.vol_id_watermark = entry->vol_id + 1;

	ret = ubi_volume_table_commit(ubi, &record);

	if (0 != ret) {
		LOG_ERR("\"%s\" could not be written to the volume table (%d)",
			config->name, ret);
		return ret;
	}

	struct ubi_volume *volume = &ubi->volumes.entries[ubi->volumes.count];

	volume->vol_id = entry->vol_id;
	volume->leb_count = entry->leb_count;
	strcpy(volume->name, entry->name);
	volume->eba = &ubi->volumes.eba_pool[ubi->volumes.eba_used];

	memset(volume->eba, 0xFF, volume->leb_count * sizeof(*volume->eba));

	ubi->volumes.count += 1;
	ubi->volumes.eba_used += volume->leb_count;

	volumes_adopt(ubi, &record);

	*vol_id = volume->vol_id;

	LOG_INF("created volume %u \"%s\" of %u logical blocks, revision %u",
		volume->vol_id, volume->name, volume->leb_count,
		ubi->volumes.revision);

	return 0;
}

int ubi_volume_drop(struct ubi_device *ubi, uint32_t vol_id)
{
	uint32_t index = 0;

	if (0 != volumes_index_of(ubi, vol_id, &index)) {
		LOG_ERR("no volume %u to remove", vol_id);
		return -ENOENT;
	}

	struct ubi_volume_table_record record = { 0 };
	int ret = 0;

	record_from_volumes(ubi, &record);

	for (uint32_t i = index + 1; i < record.volume_count; ++i)
		record.entries[i - 1] = record.entries[i];

	record.volume_count -= 1;
	memset(&record.entries[record.volume_count], 0,
	       sizeof(record.entries[0]));

	ret = ubi_volume_table_commit(ubi, &record);

	if (0 != ret) {
		LOG_ERR("volume %u could not be struck from the volume table "
			"(%d)",
			vol_id, ret);
		return ret;
	}

	struct ubi_volume *volume = &ubi->volumes.entries[index];

	/*
	 * The blocks are queued rather than erased: reclaiming costs an erase
	 * each, and when that happens is the application's call. Until then
	 * their contents remain readable to anyone with raw flash access.
	 */
	for (uint32_t lnum = 0; lnum < volume->leb_count; ++lnum) {
		if (UBI_LEB_UNMAPPED != volume->eba[lnum])
			ubi_peb_state_set(ubi, volume->eba[lnum],
					  UBI_PEB_RECLAIM);
	}

	LOG_INF("removed volume %u \"%s\", revision %u", vol_id, volume->name,
		record.revision);

	volumes_remove_at(ubi, index);
	volumes_adopt(ubi, &record);

	return 0;
}

int ubi_volume_set_size(struct ubi_device *ubi, uint32_t vol_id,
			uint32_t leb_count)
{
	uint32_t index = 0;

	if (0 != volumes_index_of(ubi, vol_id, &index)) {
		LOG_ERR("no volume %u to resize", vol_id);
		return -ENOENT;
	}

	struct ubi_volume *volume = &ubi->volumes.entries[index];
	const uint32_t was = volume->leb_count;

	if (leb_count == was)
		return 0;

	if (leb_count > was) {
		const uint32_t left =
			volumes_leb_budget(ubi) - ubi->volumes.eba_used;

		if (leb_count - was > left) {
			LOG_ERR("\"%s\" asks to grow by %u logical blocks and "
				"%u of the %u this partition can share out "
				"are left",
				volume->name, leb_count - was, left,
				volumes_leb_budget(ubi));
			return -ENOSPC;
		}
	}

	for (uint32_t lnum = leb_count; lnum < was; ++lnum) {
		if (UBI_LEB_UNMAPPED == volume->eba[lnum])
			continue;

		LOG_ERR("\"%s\" still maps logical block %u, which %u blocks "
			"would not reach",
			volume->name, lnum, leb_count);
		return -EBUSY;
	}

	struct ubi_volume_table_record record = { 0 };

	record_from_volumes(ubi, &record);
	record.entries[index].leb_count = leb_count;

	const int ret = ubi_volume_table_commit(ubi, &record);

	if (0 != ret) {
		LOG_ERR("the new size of volume %u could not be written to "
			"the volume table (%d)",
			vol_id, ret);
		return ret;
	}

	volumes_resize_at(ubi, index, leb_count);
	volumes_adopt(ubi, &record);

	LOG_INF("resized volume %u \"%s\" from %u to %u logical blocks, "
		"revision %u",
		vol_id, volume->name, was, leb_count, ubi->volumes.revision);

	return 0;
}

int ubi_volume_by_name(const struct ubi_device *ubi, const char *name,
		       uint32_t *vol_id)
{
	for (uint32_t i = 0; i < ubi->volumes.count; ++i) {
		if (0 != strcmp(ubi->volumes.entries[i].name, name))
			continue;

		*vol_id = ubi->volumes.entries[i].vol_id;

		return 0;
	}

	LOG_ERR("no volume is named \"%s\"", name);

	return -ENOENT;
}

int ubi_volume_describe(struct ubi_device *ubi, uint32_t vol_id,
			struct ubi_volume_info *info)
{
	const struct ubi_volume *volume = ubi_volume_by_id(ubi, vol_id);

	if (NULL == volume) {
		LOG_ERR("no volume %u to describe", vol_id);
		return -ENOENT;
	}

	struct ubi_volume_info described = {
		.vol_id = volume->vol_id,
		.leb_count = volume->leb_count,
	};

	strcpy(described.name, volume->name);

	for (uint32_t lnum = 0; lnum < volume->leb_count; ++lnum) {
		if (UBI_LEB_UNMAPPED != volume->eba[lnum])
			described.mapped_lebs += 1;
	}

	*info = described;

	return 0;
}
