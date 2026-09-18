/**
 * \file    suite.h
 * \author  Kamil Kielbasa
 * \brief   The device handle and the callbacks the suite owns.
 *
 *          Everything declared here is set up before a test runs and torn
 *          down after it, so a test may read it but must not take
 *          responsibility for its lifetime.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef SUITE_H
#define SUITE_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stdint.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Variable declarations --------------------------------------------------- */

/** Handle under test, allocated fresh before each test. */
extern struct ubi_device *ubi;

/** Configuration carrying the key the partition was formatted with. */
extern struct ubi_config config;

/** The same configuration, but with a key that will not verify. */
extern struct ubi_config config_wrong_key;

/** How many events the library reported during the current test. */
extern uint32_t event_count;

/** How many of each kind, indexed by \ref ubi_event_type. */
extern uint32_t event_seen[UBI_EVENT_PEB_BAD + 1];

/** The last event of each kind, so a test can check what it named. */
extern struct ubi_event event_last[UBI_EVENT_PEB_BAD + 1];

/** How many times the library asked the application for a verdict. */
extern uint32_t state_check_count;

/** What it showed the application the last time it asked. */
extern struct ubi_device_info last_state;

/* Function declarations --------------------------------------------------- */

/**
 * \brief Forget every event recorded so far.
 *
 *        Lets a test draw a line after a noisy setup and speak only about
 *        what the step under test reported.
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

#endif /* SUITE_H */
