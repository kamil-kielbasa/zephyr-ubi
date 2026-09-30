/**
 * \file    model.h
 * \author  Kamil Kielbasa
 * \brief   What the device has to hold, and checking that it does.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Header guard ------------------------------------------------------------ */
#ifndef MODEL_H
#define MODEL_H

/* Include files ----------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stdint.h>

/* UBI headers: */
#include <ubi/ubi.h>

/* Defines ----------------------------------------------------------------- */

/** Volumes the model keeps, as many as the build allows. */
#define MODEL_VOLUMES (CONFIG_UBI_MAX_NR_OF_VOLUMES)

/** Logical blocks of the largest volume, enough for the volumes to fill the
 *  device between them. */
#define MODEL_LEBS (16)

/** Writes a logical block takes before it has to be changed or erased. */
#define MODEL_SEGMENTS (3)

/** Longest write, in bytes. */
#define MODEL_WRITE_MAX (512)

/** Room for a volume name: "v" and a slot of up to ten digits. */
#define MODEL_NAME_SIZE (12)

/* Types and type definitions ---------------------------------------------- */

/** Bytes written in one go, generated from a seed. */
struct model_segment {
	/** What the bytes are generated from. */
	uint32_t seed;
	/** How many. */
	uint32_t length;
};

/** What a logical block has to hold. */
struct model_leb {
	/** A physical block backs it. */
	bool mapped;
	/** Unmapped, but the copy the unmap let go of may come back at the
	 *  next attach, holding the segments below. */
	bool ghost;
	/** A write into it stopped part way, so nothing may be appended until
	 *  it is changed or erased. */
	bool closed;
	/** Segments written back to back from the start of the block. */
	uint32_t segments;
	/** The segments. */
	struct model_segment segment[MODEL_SEGMENTS];
};

/** What a volume has to be. */
struct model_volume {
	/** The volume exists. */
	bool exists;
	/** Its identifier, or #UBI_VOL_ID_INVALID until the device says. */
	uint32_t vol_id;
	/** Logical blocks it has. */
	uint32_t leb_count;
	/** What each has to hold. */
	struct model_leb leb[MODEL_LEBS];
};

/** Everything the device has to hold. */
struct model {
	/** Volumes, by the slot their name is made from. */
	struct model_volume volume[MODEL_VOLUMES];
};

/** Operations the generator picks from. */
enum model_op {
	OP_CHANGE,
	OP_APPEND,
	OP_READ,
	OP_MAP,
	OP_UNMAP,
	OP_ERASE,
	OP_CREATE,
	OP_REMOVE,
	OP_RESIZE,
	OP_MAINTAIN,
	OP_REATTACH,
	OP_COUNT,
};

/** One operation, and what it leaves if it goes through. */
struct model_step {
	/** What to do. */
	enum model_op op;
	/** Volume slot concerned. */
	uint32_t slot;
	/** Logical block concerned, for operations on one. */
	uint32_t lnum;
	/** Bytes to write. */
	uint32_t length;
	/** What they are generated from. */
	uint32_t seed;
	/** Where they go. */
	uint32_t offset;
	/** New size, for a resize. */
	uint32_t leb_count;
	/** Housekeeping, for a maintenance step. */
	enum ubi_maintenance_op maintenance;
	/** Its budget. */
	uint32_t budget;
	/** What the device answers if the operation goes through. */
	int expected;
	/** The block written may be left with its last segment cut short. */
	bool partial;
	/** The model once the operation has gone through. */
	struct model after;
};

/** What a run did, for the report at the end. */
struct model_stats {
	/** Operations carried out, by kind. */
	uint32_t done[OP_COUNT];
	/** Refused as the model expected. */
	uint32_t refused;
	/** Refused for want of space the model could not see. */
	uint32_t no_space;
	/** Cut short by a power loss. */
	uint32_t cuts;
	/** Hit by a failed write. */
	uint32_t failures;
	/** Left with a write stopped part way. */
	uint32_t partial;
	/** Blocks relocation moved. */
	uint32_t relocated;
	/** Milliseconds the last attach took. */
	uint32_t attach_last;
	/** And the longest. */
	uint32_t attach_max;
};

/* Variable declarations --------------------------------------------------- */

/** What the device has to hold. */
extern struct model model;

/** The model before the operation under way. */
extern struct model before;

/** Operation under way, for the messages. */
extern uint32_t step;

/** Bytes a logical block holds. */
extern uint32_t leb_size;

/** Write granularity of the flash. */
extern uint32_t write_block;

/** Logical blocks the volumes may share out, all of them unclaimed. */
extern uint32_t lebs_shared;

/** What the run did. */
extern struct model_stats stats;

/** Names of the operations, for the messages. */
extern const char *const op_name[OP_COUNT];

/* Function declarations --------------------------------------------------- */

/**
 * \brief Next number of the generator.
 */
uint32_t random_next(void);

/**
 * \brief A number below \p bound.
 */
uint32_t random_below(uint32_t bound);

/**
 * \brief Byte \p at of a segment generated from \p seed.
 */
uint8_t segment_byte(uint32_t seed, uint32_t at);

/**
 * \brief Bytes a block holds, from its start.
 */
uint32_t leb_written(const struct model_leb *leb);

/**
 * \brief Name of the volume in \p slot.
 */
void volume_name(uint32_t slot, char name[MODEL_NAME_SIZE]);

/**
 * \brief Report whether a block is mapped as \p leb says and reads as it
 *        says.
 */
bool leb_reads(const struct model_volume *volume, uint32_t lnum,
	       const struct model_leb *leb);

/**
 * \brief Report whether a block reads as \p leb with its last segment cut
 *        short, and say how in \p found.
 */
bool leb_reads_cut(const struct model_volume *volume, uint32_t lnum,
		   const struct model_leb *leb, struct model_leb *found);

/**
 * \brief Report whether the volumes on the device are the ones \p expected
 *        holds, and take their identifiers into it.
 */
bool volumes_are(struct model *expected);

/**
 * \brief Fail unless the device holds exactly what the model says.
 */
void model_verify(void);

/**
 * \brief Attach, and time it.
 */
void attach_timed(void);

/**
 * \brief Settle every block after an attach.
 */
void model_settle(struct model *settled, const struct model_step *s,
		  const struct model *old);

/**
 * \brief Settle the block a failed write hit, while the device carries on.
 */
void leb_settle_failed(const struct model_step *s);

/**
 * \brief Detach, attach again and settle what an unmap left.
 */
void reattach(void);

#endif /* MODEL_H */
