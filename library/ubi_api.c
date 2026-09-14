/**
 * \file    ubi_api.c
 * \author  Kamil Kielbasa
 * \brief   The boundary between the application and the library.
 *
 *          Every function \c <ubi/ubi.h> declares is defined here and nowhere
 *          else, and none of them does any real work. Each one checks its
 *          arguments, takes the device lock, asks whether the device is still
 *          trusted when it is about to write, delegates, and reports what
 *          happened.
 *
 *          Keeping that in one file is what lets the modules behind it assume
 *          their arguments are already good, and what makes it impossible to
 *          forget the lock on one entry point out of fifteen.
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
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include <ubi/ubi.h>

#include "ubi_device.h"
#include "ubi_private.h"
#include "ubi_state.h"
#include "ubi_volume.h"
#include "ubi_volume_table.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations -------------------------------------------- */

/**
 * \brief Reject a configuration that cannot drive the library.
 */
static int config_validate(const struct ubi_config *config);

/**
 * \brief Report whether a handle is one this library attached.
 */
static bool device_is_attached(const struct ubi_device *ubi);

/**
 * \brief Report whether an identifier is one the application may name.
 *
 *        The volume holding the volume table has one of its own, and it is
 *        not the application's to touch.
 */
static bool vol_id_is_public(uint32_t vol_id);

/* Static function definitions --------------------------------------------- */

static int config_validate(const struct ubi_config *config)
{
	if (NULL == config)
		return -EINVAL;

	if (PSA_KEY_ID_NULL == config->ikm_key_id)
		return -EINVAL;

	if (NULL == config->event_cb || NULL == config->state_cb)
		return -EINVAL;

	return 0;
}

static bool device_is_attached(const struct ubi_device *ubi)
{
	return (NULL != ubi) && (UBI_DEVICE_MAGIC == ubi->magic);
}

static bool vol_id_is_public(uint32_t vol_id)
{
	return (UBI_VOLUME_TABLE_VOL_ID != vol_id) &&
	       (UBI_VOL_ID_INVALID != vol_id);
}

/* Module interface function definitions ----------------------------------- */

size_t ubi_device_size(void)
{
	return sizeof(struct ubi_device);
}

int ubi_device_format(const struct ubi_config *config)
{
	int ret = config_validate(config);

	if (0 != ret) {
		LOG_ERR("format needs a key handle and both callbacks");
		return ret;
	}

	return ubi_device_format_partition(config);
}

int ubi_device_init(struct ubi_device *ubi, const struct ubi_config *config)
{
	if (NULL == ubi) {
		LOG_ERR("attach needs a device handle");
		return -EINVAL;
	}

	if (device_is_attached(ubi)) {
		LOG_ERR("this handle is already attached");
		return -EBUSY;
	}

	int ret = config_validate(config);

	if (0 != ret) {
		LOG_ERR("attach needs a key handle and both callbacks");
		return ret;
	}

	/* Nothing to lock yet: the mutex lives in the handle this call is
	 * about to build. */
	return ubi_device_attach(ubi, config);
}

int ubi_device_deinit(struct ubi_device *ubi)
{
	if (!device_is_attached(ubi)) {
		LOG_ERR("this handle is not attached");
		return -EINVAL;
	}

	/* Detaching a handle another thread is using is a defect the library
	 * cannot paper over, so this does not lock either. */
	ubi_device_detach(ubi);

	return 0;
}

int ubi_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info)
{
	if (!device_is_attached(ubi) || NULL == info) {
		LOG_ERR("device info needs an attached handle");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	const int ret = ubi_state_describe(ubi, info);

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_volume_create(struct ubi_device *ubi,
		      const struct ubi_volume_config *config, uint32_t *vol_id)
{
	if (!device_is_attached(ubi) || NULL == config ||
	    NULL == config->name || 0 == config->leb_count || NULL == vol_id) {
		LOG_ERR("creating a volume needs an attached handle, a name, "
			"a size of at least one logical block and somewhere "
			"to put the identifier");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	int ret = ubi_state_guard(ubi);

	if (0 != ret) {
		LOG_ERR("volume \"%s\" may not be created on a device that is "
			"no longer trusted",
			config->name);
		goto unlock;
	}

	ret = ubi_volume_add(ubi, config, vol_id);

unlock:
	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_volume_resize(struct ubi_device *ubi, uint32_t vol_id,
		      uint32_t leb_count)
{
	if (!device_is_attached(ubi) || !vol_id_is_public(vol_id) ||
	    0 == leb_count) {
		LOG_ERR("resizing a volume needs an attached handle, an "
			"identifier the application owns and a size of at "
			"least one logical block");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	int ret = ubi_state_guard(ubi);

	if (0 != ret) {
		LOG_ERR("volume %u may not be resized on a device that is no "
			"longer trusted",
			vol_id);
		goto unlock;
	}

	ret = ubi_volume_set_size(ubi, vol_id, leb_count);

unlock:
	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_volume_remove(struct ubi_device *ubi, uint32_t vol_id)
{
	if (!device_is_attached(ubi) || !vol_id_is_public(vol_id)) {
		LOG_ERR("removing a volume needs an attached handle and an "
			"identifier the application owns");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	int ret = ubi_state_guard(ubi);

	if (0 != ret) {
		LOG_ERR("volume %u may not be removed from a device that is "
			"no longer trusted",
			vol_id);
		goto unlock;
	}

	ret = ubi_volume_drop(ubi, vol_id);

unlock:
	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_volume_find(struct ubi_device *ubi, const char *name, uint32_t *vol_id)
{
	if (!device_is_attached(ubi) || NULL == name || NULL == vol_id) {
		LOG_ERR("finding a volume needs an attached handle, a name "
			"and somewhere to put the answer");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	const int ret = ubi_volume_by_name(ubi, name, vol_id);

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_volume_get_info(struct ubi_device *ubi, uint32_t vol_id,
			struct ubi_volume_info *info)
{
	if (!device_is_attached(ubi) || !vol_id_is_public(vol_id) ||
	    NULL == info) {
		LOG_ERR("volume info needs an attached handle and an "
			"identifier the application owns");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	const int ret = ubi_volume_describe(ubi, vol_id, info);

	k_mutex_unlock(&ubi->lock);

	return ret;
}
