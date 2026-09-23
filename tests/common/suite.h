/**
 * \file    suite.h
 * \author  Kamil Kielbasa
 * \brief   The device handle, the callbacks and the suites the tests declare.
 *
 *          Everything here is set up before a test and torn down after it; a
 *          test reads it but does not own it.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef SUITE_H
#define SUITE_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

/* Zephyr headers: */
#include <zephyr/ztest.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Defines ----------------------------------------------------------------- */

/**
 * \brief Declare a suite with the shared fixture.
 *
 *        A suite that never runs fails the project under
 *        \c CONFIG_ZTEST_VERIFY_RUN_ALL, so tests that hold only on some
 *        builds are selected with \c #if rather than left in an empty suite.
 */
#define UBI_TEST_SUITE(name) \
	ZTEST_SUITE(name, NULL, suite_setup, suite_before, suite_after, NULL)

/** Bytes a test writes to a block; no more than count_data_matching() reads. */
#define UBI_TEST_PAYLOAD_SIZE (256)

/** Logical blocks of the volume a test works in when their number does not
 *  matter. */
#define UBI_TEST_VOLUME_LEBS (4)

/** Logical blocks a loaded volume leaves unclaimed. */
#define UBI_TEST_LOAD_FREE_LEBS (4)

/** Seed the blocks of a loaded volume are filled from. */
#define UBI_TEST_LOAD_SEED (0x00)

/* Variable declarations --------------------------------------------------- */

/** Handle under test, allocated fresh before each test. */
extern struct ubi_device *ubi;

/** Configuration carrying the key the partition is formatted with. */
extern struct ubi_config config;

/** The same configuration with a key that will not verify. */
extern struct ubi_config config_wrong_key;

/** Events reported during the current test, of every kind. */
extern uint32_t events_total;

/** Events reported during the current test, by \ref ubi_event_type. */
extern uint32_t event_count[UBI_EVENT_PEB_BAD + 1];

/** The last event of each kind. */
extern struct ubi_event event_last[UBI_EVENT_PEB_BAD + 1];

/** How many times the library asked for a verdict. */
extern uint32_t state_check_count;

/** What it showed the application the last time it asked. */
extern struct ubi_device_info last_state;

/* Function declarations --------------------------------------------------- */

/**
 * \brief Bring up the crypto backend and the keys, once for every suite.
 */
void *suite_setup(void);

/**
 * \brief Hand the test a zeroed handle and a blank partition.
 */
void suite_before(void *fixture);

/**
 * \brief Give the handle back.
 */
void suite_after(void *fixture);

/**
 * \brief Forget every event recorded so far.
 */
void events_forget(void);

/**
 * \brief Trust callback that accepts whatever it is shown.
 */
enum ubi_state_verdict trust_everything(const struct ubi_device_info *info,
					void *user_context);

/**
 * \brief Trust callback that refuses whatever it is shown.
 */
enum ubi_state_verdict trust_nothing(const struct ubi_device_info *info,
				     void *user_context);

/**
 * \brief Trust callback that accepts the attach and refuses everything after.
 */
enum ubi_state_verdict trust_once(const struct ubi_device_info *info,
				  void *user_context);

/**
 * \brief Format, attach and create one volume.
 *
 * \return Identifier the volume was given.
 */
uint32_t volume_ready(uint32_t leb_count);

/**
 * \brief Physical blocks currently backing a logical one.
 */
uint32_t mapped_pebs(const struct ubi_device_info *info);

/**
 * \brief Erase count of the block behind a logical block.
 */
uint32_t leb_wear(uint32_t vol_id, uint32_t lnum);

/**
 * \brief Fill \p buffer with a pattern that changes from byte to byte.
 */
void pattern_fill(uint8_t *buffer, size_t length, uint8_t seed);

/**
 * \brief Fill \p buffer with what block \p lnum of a volume written from
 *        \p seed holds, so no two blocks of it read alike.
 */
void leb_payload(uint8_t seed, uint32_t lnum, uint8_t *buffer, size_t length);

/**
 * \brief Format, attach and fill a volume that leaves only
 *        \ref UBI_TEST_LOAD_FREE_LEBS logical blocks unclaimed.
 *
 * \param[out] vol_id                   Identifier the volume was given.
 *
 * \return How many logical blocks it holds.
 */
uint32_t volume_under_load(uint32_t *vol_id);

/**
 * \brief The same, with every other block erased and stamped.
 *
 * \param[out] vol_id                   Identifier the volume was given.
 *
 * \return How many logical blocks it holds.
 */
uint32_t pool_ready(uint32_t *vol_id);

#endif /* SUITE_H */
