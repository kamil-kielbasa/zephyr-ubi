/**
 * \file    main.c
 * \author  Kamil Kielbasa
 * \brief   Attach a UBI device, format only a partition that holds none, and
 *          count boots in a volume of one block.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Module defines ---------------------------------------------------------- */

#define UBI_PARTITION_ID DT_FIXED_PARTITION_ID(DT_NODELABEL(ubi_partition))

/** One header's worth, a whole number of write blocks on any flash UBI
 *  accepts. */
#define RECORD_SIZE (64)

/* Types and type definitions ---------------------------------------------- */

/**
 * \brief What the state check compares against. A product keeps it where
 *        the flash cannot reach, such as PSA ITS or a monotonic counter;
 *        this sample keeps it in RAM, so it only lasts until a reboot.
 */
struct trust_anchor {
	/** Something has been recorded. */
	bool anchored;
	/** The application asked for the format the next check sees. */
	bool format_authorised;
	/** Image last seen. */
	uint32_t image_seq;
	/** Highest layout revision seen within it. */
	uint32_t revision;
};

/* Static function declarations -------------------------------------------- */

/**
 * \brief Report what UBI found. Must not block or call into UBI.
 */
static void on_event(const struct ubi_event *event, void *user_context);

/**
 * \brief Trust the same image at a revision no older than the last one.
 */
static enum ubi_state_verdict on_state(const struct ubi_device_info *info,
				       void *user_context);

/**
 * \brief Import the keying material UBI derives its keys from.
 */
static int ikm_import(psa_key_id_t *key_id);

/**
 * \brief Attach, formatting first only when the partition holds no UBI
 *        metadata at all.
 */
static int attach(struct ubi_device *ubi, const struct ubi_config *config);

/**
 * \brief Find the volume the count lives in, creating it on the first boot.
 */
static int boots_volume(struct ubi_device *ubi, uint32_t *vol_id);

/**
 * \brief Read the boot count, add one, and write it back.
 */
static int boots_count(struct ubi_device *ubi, uint32_t *boots);

/**
 * \brief The error to report: \p ret if there is one, \p next otherwise.
 */
static int first_error(int ret, int next);

/* Module variables and constants ------------------------------------------ */

static struct trust_anchor anchor = { 0 };

/* Static function definitions --------------------------------------------- */

static void on_event(const struct ubi_event *event, void *user_context)
{
	ARG_UNUSED(user_context);

	printk("ubi: event %d on block %u\n", (int)event->type, event->pnum);
}

static enum ubi_state_verdict on_state(const struct ubi_device_info *info,
				       void *user_context)
{
	ARG_UNUSED(user_context);

	if (anchor.anchored && !anchor.format_authorised) {
		if (info->image_seq != anchor.image_seq)
			return UBI_STATE_UNTRUSTED;

		if (info->revision < anchor.revision)
			return UBI_STATE_UNTRUSTED;
	}

	anchor.anchored = true;
	anchor.format_authorised = false;
	anchor.image_seq = info->image_seq;
	anchor.revision = info->revision;

	return UBI_STATE_TRUSTED;
}

static int ikm_import(psa_key_id_t *key_id)
{
	/* Never ship a key in the source. A product provisions keying
	 * material per device and per partition into its key store and hands
	 * UBI the handle. */
	static const uint8_t ikm[32] = {
		0x75, 0x62, 0x69, 0x2d, 0x73, 0x61, 0x6d, 0x70,
		0x6c, 0x65, 0x2d, 0x6f, 0x6e, 0x6c, 0x79, 0x2d,
		0x6e, 0x6f, 0x74, 0x2d, 0x61, 0x2d, 0x73, 0x65,
		0x63, 0x72, 0x65, 0x74, 0x2d, 0x6b, 0x65, 0x79,
	};
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_type(&attributes, PSA_KEY_TYPE_DERIVE);
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(&attributes, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);

	const psa_status_t status =
		psa_import_key(&attributes, ikm, sizeof(ikm), key_id);

	return (PSA_SUCCESS == status) ? 0 : -EIO;
}

static int attach(struct ubi_device *ubi, const struct ubi_config *config)
{
	int ret = ubi_device_init(ubi, config);

	/* Any other failure leaves metadata a format would destroy. */
	if (-ENODEV != ret)
		return ret;

	printk("ubi: no device on the partition, formatting\n");

	anchor.format_authorised = true;
	ret = ubi_device_format(config);

	if (0 != ret)
		return ret;

	return ubi_device_init(ubi, config);
}

static int boots_volume(struct ubi_device *ubi, uint32_t *vol_id)
{
	const struct ubi_volume_config wanted = { .name = "boots",
						  .leb_count = 1 };
	const int ret = ubi_volume_find(ubi, wanted.name, vol_id);

	if (-ENOENT != ret)
		return ret;

	return ubi_volume_create(ubi, &wanted, vol_id);
}

static int boots_count(struct ubi_device *ubi, uint32_t *boots)
{
	uint8_t record[RECORD_SIZE] = { 0 };
	struct ubi_leb_info leb = { 0 };
	uint32_t vol_id = UBI_VOL_ID_INVALID;
	int ret = boots_volume(ubi, &vol_id);

	if (0 != ret)
		return ret;

	ret = ubi_leb_get_info(ubi, vol_id, 0, &leb);

	if (0 != ret)
		return ret;

	/* A block never written reads as erased; start from zero instead. */
	if (leb.mapped) {
		ret = ubi_leb_read(ubi, vol_id, 0, 0, record, sizeof(record));

		if (0 != ret)
			return ret;
	}

	*boots = sys_get_le32(record) + 1;
	sys_put_le32(*boots, record);

	/* Replaces the block whole: after a power cut it holds the old count
	 * or the new one. */
	return ubi_leb_change(ubi, vol_id, 0, record, sizeof(record));
}

static int first_error(int ret, int next)
{
	return (0 != ret) ? ret : next;
}

/* Module interface function definitions ----------------------------------- */

int main(void)
{
	struct ubi_config config = {
		.flash_area_id = UBI_PARTITION_ID,
		.event_cb = on_event,
		.state_cb = on_state,
	};
	struct ubi_maintenance_result result = { 0 };
	struct ubi_device_info info = { 0 };
	struct ubi_device *ubi = NULL;
	uint32_t boots = 0;
	psa_status_t status = psa_crypto_init();
	int ret = 0;

	if (PSA_SUCCESS != status) {
		printk("ubi: PSA crypto did not start (%d)\n", (int)status);
		return -EIO;
	}

	ret = ikm_import(&config.ikm_key_id);

	if (0 != ret) {
		printk("ubi: no keying material (%d)\n", ret);
		return ret;
	}

	ubi = k_calloc(1, ubi_device_size());

	if (NULL == ubi) {
		printk("ubi: no memory for a handle\n");
		ret = -ENOMEM;
		goto destroy_key;
	}

	ret = attach(ubi, &config);

	if (0 != ret) {
		printk("ubi: attach failed (%d)\n", ret);
		goto free_handle;
	}

	ret = ubi_device_get_info(ubi, &info);

	if (0 != ret) {
		printk("ubi: no device info (%d)\n", ret);
		goto detach;
	}

	printk("ubi: %u blocks of %u bytes, image 0x%08x, revision %u\n",
	       info.peb_count, info.peb_size, info.image_seq, info.revision);

	ret = boots_count(ubi, &boots);

	if (0 != ret) {
		printk("ubi: counting the boot failed (%d)\n", ret);
		goto detach;
	}

	printk("boot count: %u\n", boots);

	/* Pay for the next write's erase now, while nothing waits on it. */
	ret = ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 1, &result);

	if (0 != ret)
		printk("ubi: reclaim failed (%d)\n", ret);

detach:
	ret = first_error(ret, ubi_device_deinit(ubi));
free_handle:
	k_free(ubi);
destroy_key:
	status = psa_destroy_key(config.ikm_key_id);
	ret = first_error(ret, (PSA_SUCCESS == status) ? 0 : -EIO);

	if (0 == ret)
		printk("done\n");

	return ret;
}
