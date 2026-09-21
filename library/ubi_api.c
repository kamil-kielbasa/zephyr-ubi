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
#include "ubi_leb.h"
#include "ubi_maintenance.h"
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
static int api_config_validate(const struct ubi_config *config);

/**
 * \brief Report whether a handle is one this library attached.
 */
static bool api_device_is_attached(const struct ubi_device *ubi);

/**
 * \brief Report whether an identifier is one the application may name.
 *
 *        The volume holding the volume table has one of its own, and it is
 *        not the application's to touch.
 */
static bool api_vol_id_is_public(uint32_t vol_id);

/* Static function definitions --------------------------------------------- */

static int api_config_validate(const struct ubi_config *config)
{
	if (NULL == config)
		return -EINVAL;

	if (PSA_KEY_ID_NULL == config->ikm_key_id)
		return -EINVAL;

	if (NULL == config->event_cb || NULL == config->state_cb)
		return -EINVAL;

	return 0;
}

static bool api_device_is_attached(const struct ubi_device *ubi)
{
	return (NULL != ubi) && (UBI_DEVICE_MAGIC == ubi->magic);
}

static bool api_vol_id_is_public(uint32_t vol_id)
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
	int ret = api_config_validate(config);

	if (0 != ret) {
		LOG_ERR("format needs a key handle and both callbacks");
		return ret;
	}

	return ubi_impl_device_format(config);
}

int ubi_device_init(struct ubi_device *ubi, const struct ubi_config *config)
{
	if (NULL == ubi) {
		LOG_ERR("attach needs a device handle");
		return -EINVAL;
	}

	if (api_device_is_attached(ubi)) {
		LOG_ERR("this handle is already attached");
		return -EBUSY;
	}

	int ret = api_config_validate(config);

	if (0 != ret) {
		LOG_ERR("attach needs a key handle and both callbacks");
		return ret;
	}

	/* Nothing to lock yet: the mutex lives in the handle this call is
	 * about to build. */
	return ubi_impl_device_init(ubi, config);
}

int ubi_device_deinit(struct ubi_device *ubi)
{
	if (!api_device_is_attached(ubi)) {
		LOG_ERR("this handle is not attached");
		return -EINVAL;
	}

	/* Detaching a handle another thread is using is a defect the library
	 * cannot paper over, so this does not lock either. */
	ubi_impl_device_deinit(ubi);

	return 0;
}

int ubi_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info)
{
	if (!api_device_is_attached(ubi) || NULL == info) {
		LOG_ERR("device info needs an attached handle");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	const int ret = ubi_impl_state_describe(ubi, info);

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_volume_create(struct ubi_device *ubi,
		      const struct ubi_volume_config *config, uint32_t *vol_id)
{
	if (!api_device_is_attached(ubi) || NULL == config ||
	    NULL == config->name || 0 == config->leb_count || NULL == vol_id) {
		LOG_ERR("volume create: needs a handle, a name and a size");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	int ret = ubi_impl_state_guard(ubi);

	if (0 == ret)
		ret = ubi_impl_volume_create(ubi, config, vol_id);

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_volume_resize(struct ubi_device *ubi, uint32_t vol_id,
		      uint32_t leb_count)
{
	if (!api_device_is_attached(ubi) || !api_vol_id_is_public(vol_id) ||
	    0 == leb_count) {
		LOG_ERR("volume resize: needs a handle, a volume and a size");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	int ret = ubi_impl_state_guard(ubi);

	if (0 == ret)
		ret = ubi_impl_volume_resize(ubi, vol_id, leb_count);

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_volume_remove(struct ubi_device *ubi, uint32_t vol_id)
{
	if (!api_device_is_attached(ubi) || !api_vol_id_is_public(vol_id)) {
		LOG_ERR("volume remove: needs a handle and a volume");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	int ret = ubi_impl_state_guard(ubi);

	if (0 == ret)
		ret = ubi_impl_volume_remove(ubi, vol_id);

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_volume_find(struct ubi_device *ubi, const char *name, uint32_t *vol_id)
{
	if (!api_device_is_attached(ubi) || NULL == name || NULL == vol_id) {
		LOG_ERR("volume find: needs a handle, a name and an answer");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	const int ret = ubi_impl_volume_find(ubi, name, vol_id);

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_volume_get_info(struct ubi_device *ubi, uint32_t vol_id,
			struct ubi_volume_info *info)
{
	if (!api_device_is_attached(ubi) || !api_vol_id_is_public(vol_id) ||
	    NULL == info) {
		LOG_ERR("volume info: needs a handle, a volume and an answer");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	const int ret = ubi_impl_volume_get_info(ubi, vol_id, info);

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_leb_map(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum)
{
	if (!api_device_is_attached(ubi) || !api_vol_id_is_public(vol_id)) {
		LOG_ERR("leb map: needs a handle and a volume");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	int ret = ubi_impl_state_guard(ubi);

	if (0 == ret)
		ret = ubi_impl_leb_map(ubi, vol_id, lnum);

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_leb_unmap(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum)
{
	if (!api_device_is_attached(ubi) || !api_vol_id_is_public(vol_id)) {
		LOG_ERR("leb unmap: needs a handle and a volume");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	int ret = ubi_impl_state_guard(ubi);

	if (0 == ret)
		ret = ubi_impl_leb_unmap(ubi, vol_id, lnum);

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_leb_erase(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum)
{
	if (!api_device_is_attached(ubi) || !api_vol_id_is_public(vol_id)) {
		LOG_ERR("leb erase: needs a handle and a volume");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	int ret = ubi_impl_state_guard(ubi);

	if (0 == ret)
		ret = ubi_impl_leb_erase(ubi, vol_id, lnum);

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_leb_read(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		 uint32_t offset, void *buffer, size_t length)
{
	if (!api_device_is_attached(ubi) || !api_vol_id_is_public(vol_id) ||
	    NULL == buffer) {
		LOG_ERR("leb read: needs a handle, a volume and a destination");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	/* Reads are served even after the application withdraws its trust:
	 * it still has to be able to look at what it decided to distrust. */
	const int ret =
		ubi_impl_leb_read(ubi, vol_id, lnum, offset, buffer, length);

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_leb_change(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		   const void *buffer, size_t length)
{
	if (!api_device_is_attached(ubi) || !api_vol_id_is_public(vol_id) ||
	    (NULL == buffer && 0 != length)) {
		LOG_ERR("leb change: needs a handle, a volume and the data");
		return -EINVAL;
	}

	if (0 == length)
		return 0;

	k_mutex_lock(&ubi->lock, K_FOREVER);

	int ret = ubi_impl_state_guard(ubi);

	if (0 == ret)
		ret = ubi_impl_leb_change(ubi, vol_id, lnum, buffer, length);

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_leb_write_at(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		     uint32_t offset, const void *buffer, size_t length)
{
	if (!api_device_is_attached(ubi) || !api_vol_id_is_public(vol_id) ||
	    (NULL == buffer && 0 != length)) {
		LOG_ERR("leb write: needs a handle, a volume and the data");
		return -EINVAL;
	}

	if (0 == length)
		return 0;

	k_mutex_lock(&ubi->lock, K_FOREVER);

	int ret = ubi_impl_state_guard(ubi);

	if (0 == ret) {
		ret = ubi_impl_leb_write_at(ubi, vol_id, lnum, offset, buffer,
					    length);
	}

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_leb_get_info(struct ubi_device *ubi, uint32_t vol_id, uint32_t lnum,
		     struct ubi_leb_info *info)
{
	if (!api_device_is_attached(ubi) || !api_vol_id_is_public(vol_id) ||
	    NULL == info) {
		LOG_ERR("leb info: needs a handle, a volume and an answer");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	const int ret = ubi_impl_leb_get_info(ubi, vol_id, lnum, info);

	k_mutex_unlock(&ubi->lock);

	return ret;
}

int ubi_maintenance(struct ubi_device *ubi, enum ubi_maintenance_op operation,
		    uint32_t budget, struct ubi_maintenance_result *result)
{
	if (!api_device_is_attached(ubi) || NULL == result) {
		LOG_ERR("maintenance: needs a handle and somewhere to report");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->lock, K_FOREVER);

	int ret = ubi_impl_state_guard(ubi);

	if (0 == ret)
		ret = ubi_impl_maintenance(ubi, operation, budget, result);

	k_mutex_unlock(&ubi->lock);

	return ret;
}
