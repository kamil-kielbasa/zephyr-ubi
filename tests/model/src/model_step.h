/**
 * \file    model_step.h
 * \author  Kamil Kielbasa
 * \brief   Picking one operation, carrying it out, and holding the device to
 *          the model afterwards.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef MODEL_STEP_H
#define MODEL_STEP_H

/* Function declarations --------------------------------------------------- */

/**
 * \brief Pick one operation and carry it out, cut short now and then on the
 *        flash simulator.
 */
void step_run(void);

#endif /* MODEL_STEP_H */
