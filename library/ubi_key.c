/**
 * \file    ubi_key.c
 * \author  Kamil Kielbasa
 * \brief   Derivation of the keys UBI authenticates its metadata with.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

/* PSA headers: */
#include <psa/crypto.h>

/* UBI headers: */
#include "ubi_key.h"

/* Module defines ---------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Module variables and constants ------------------------------------------ */

/** Domain separator for the block header key. */
static const uint8_t label_header[] = "zephyr-ubi/header/v1";

/** Domain separator for the volume table record key. */
static const uint8_t label_volume_table[] = "zephyr-ubi/volume-table/v1";

/**
 * Fixed HKDF salt.
 *
 * A salt does not have to be secret or varying; separating the two keys is
 * the labels' job. What matters here is that it is a compile-time constant,
 * because anything read from the flash would have to be trusted before it
 * could be verified.
 */
static const uint8_t derivation_salt[] = "zephyr-ubi/v1";

/* Static function declarations -------------------------------------------- */

/**
 * \brief Run one HKDF-SHA256 derivation into a fresh CMAC key.
 *
 *        Reports through \p psa_status what the backend said, so that the
 *        caller can log it; a static helper stays silent itself.
 *
 * \param ikm_key_id                    Key to derive from.
 * \param[in] label                     Domain separator.
 * \param label_length                  Bytes of \p label to use.
 * \param[out] key_id                   Receives the derived key.
 * \param[out] psa_status               Last status from the backend.
 *
 * \retval 0
 *         Success.
 * \retval -EACCES
 *         \p ikm_key_id does not exist, or its policy forbids this use.
 * \retval -EIO
 *         The crypto backend failed.
 */
static int key_derive_one(psa_key_id_t ikm_key_id, const uint8_t *label,
			  size_t label_length, psa_key_id_t *key_id,
			  psa_status_t *psa_status);

/* Static function definitions --------------------------------------------- */

static int key_derive_one(psa_key_id_t ikm_key_id, const uint8_t *label,
			  size_t label_length, psa_key_id_t *key_id,
			  psa_status_t *psa_status)
{
	psa_key_derivation_operation_t operation =
		PSA_KEY_DERIVATION_OPERATION_INIT;
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_status_t status = PSA_ERROR_GENERIC_ERROR;
	int ret = -EIO;

	psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attributes, UBI_KEY_BITS);
	psa_set_key_algorithm(&attributes, PSA_ALG_CMAC);
	psa_set_key_usage_flags(&attributes,
				PSA_KEY_USAGE_SIGN_MESSAGE |
					PSA_KEY_USAGE_VERIFY_MESSAGE);
	psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);

	status = psa_key_derivation_setup(&operation,
					  PSA_ALG_HKDF(PSA_ALG_SHA_256));

	if (PSA_SUCCESS != status) {
		goto exit;
	}

	/* HKDF wants the salt before the secret. */
	status = psa_key_derivation_input_bytes(&operation,
						PSA_KEY_DERIVATION_INPUT_SALT,
						derivation_salt,
						sizeof(derivation_salt) - 1);

	if (PSA_SUCCESS != status) {
		goto exit;
	}

	status = psa_key_derivation_input_key(
		&operation, PSA_KEY_DERIVATION_INPUT_SECRET, ikm_key_id);

	if (PSA_SUCCESS != status) {
		if (PSA_ERROR_NOT_PERMITTED == status ||
		    PSA_ERROR_INVALID_HANDLE == status ||
		    PSA_ERROR_DOES_NOT_EXIST == status) {
			ret = -EACCES;
		}
		goto exit;
	}

	status = psa_key_derivation_input_bytes(
		&operation, PSA_KEY_DERIVATION_INPUT_INFO, label, label_length);

	if (PSA_SUCCESS != status) {
		goto exit;
	}

	status = psa_key_derivation_output_key(&attributes, &operation, key_id);

	if (PSA_SUCCESS != status) {
		goto exit;
	}

	ret = 0;

exit:
	*psa_status = status;

	psa_key_derivation_abort(&operation);
	psa_reset_key_attributes(&attributes);

	return ret;
}

/* Module interface function definitions ----------------------------------- */

int ubi_impl_key_derive(psa_key_id_t ikm_key_id, psa_key_id_t *key_header,
			psa_key_id_t *key_volume_table)
{
	psa_status_t status = PSA_ERROR_GENERIC_ERROR;
	int ret = 0;

	if (NULL == key_header || NULL == key_volume_table) {
		LOG_ERR("key derivation needs both output handles");
		return -EINVAL;
	}

	if (PSA_KEY_ID_NULL == ikm_key_id) {
		LOG_ERR("no input keying material was supplied");
		return -EINVAL;
	}

	*key_header = PSA_KEY_ID_NULL;
	*key_volume_table = PSA_KEY_ID_NULL;

	/* Labels are separators, so the terminating NUL carries no meaning. */
	ret = key_derive_one(ikm_key_id, label_header, sizeof(label_header) - 1,
			     key_header, &status);

	if (0 != ret) {
		LOG_ERR("deriving the header key failed (%d), psa_status=%d",
			ret, (int)status);
		return ret;
	}

	ret = key_derive_one(ikm_key_id, label_volume_table,
			     sizeof(label_volume_table) - 1, key_volume_table,
			     &status);

	if (0 != ret) {
		LOG_ERR("deriving the volume table key failed (%d), "
			"psa_status=%d",
			ret, (int)status);
		ubi_impl_key_destroy(key_header);
		return ret;
	}

	return 0;
}

void ubi_impl_key_destroy(psa_key_id_t *key_id)
{
	psa_status_t status = PSA_SUCCESS;

	if (NULL == key_id) {
		LOG_ERR("no key handle to destroy");
		return;
	}

	if (PSA_KEY_ID_NULL == *key_id) {
		return;
	}

	status = psa_destroy_key(*key_id);

	if (PSA_SUCCESS != status) {
		LOG_ERR("destroying key %u failed, psa_status=%d",
			(unsigned int)*key_id, (int)status);
	}

	*key_id = PSA_KEY_ID_NULL;
}
