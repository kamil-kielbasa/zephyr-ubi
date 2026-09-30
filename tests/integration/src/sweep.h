/**
 * \file    sweep.h
 * \author  Kamil Kielbasa
 * \brief   Running one operation over and over from the same partition, with
 *          a fault moved a little further along each time. Flash simulator
 *          only.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef SWEEP_H
#define SWEEP_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Defines ----------------------------------------------------------------- */

/** Bytes a sweep moves its fault along by. Odd, so that it lands on every
 *  offset within a write block over the course of a sweep. */
#define SWEEP_STRIDE (3)

/* Function declarations --------------------------------------------------- */

/**
 * \brief Attach the device as it stands and fail the test if it will not.
 */
void sweep_attach(void);

/**
 * \brief Give the flash back its power and detach, as a reboot would.
 */
void sweep_reboot(void);

/**
 * \brief Report whether logical block \p lnum reads exactly \p expected.
 */
bool leb_holds(uint32_t vol_id, uint32_t lnum, const uint8_t *expected,
	       size_t length);

/**
 * \brief Fail unless logical block \p lnum reads exactly \p expected.
 */
void leb_check(uint32_t vol_id, uint32_t lnum, const uint8_t *expected,
	       size_t length);

/**
 * \brief Report whether a volume of that name is on the attached device.
 */
bool volume_present(const char *name);

#endif /* SWEEP_H */
