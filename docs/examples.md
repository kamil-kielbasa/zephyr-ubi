# Examples

The full contract for every call lives in
[include/ubi/ubi.h](https://github.com/kamil-kielbasa/zephyr-ubi/blob/main/include/ubi/ubi.h).

## Adding it to a project

Add the repository to your west manifest:

```yaml
manifest:
  projects:
    - name: zephyr-ubi
      url: https://github.com/kamil-kielbasa/zephyr-ubi
      revision: main
      path: modules/lib/zephyr-ubi
```

Then turn it on. `CONFIG_UBI` pulls in the flash and PSA crypto it needs:

```ini
CONFIG_UBI=y
CONFIG_MBEDTLS=y
CONFIG_MBEDTLS_PSA_CRYPTO_C=y

# The device handle and its per-block bookkeeping are allocated here.
CONFIG_HEAP_MEM_POOL_SIZE=8192
```

UBI manages one fixed partition, named through the devicetree as usual.

## A minimal life

```c
#include <ubi/ubi.h>

static void on_event(const struct ubi_event *event, void *user_context)
{
	LOG_WRN("UBI event %d on PEB %u", event->type, event->pnum);
}

static enum ubi_state_verdict on_state(const struct ubi_device_info *info,
				       void *user_context)
{
	return UBI_STATE_TRUSTED;	/* read the security page before shipping */
}

static const struct ubi_config cfg = {
	.flash_area_id = PARTITION_ID(storage_partition),
	.ikm_key_id    = DEVICE_IKM_KEY_ID,
	.event_cb      = on_event,
	.state_cb      = on_state,
};

struct ubi_device *ubi = malloc(ubi_device_size());
uint32_t vol_id = UBI_VOL_ID_INVALID;

ubi_device_format(&cfg);		/* once, at manufacturing */
ubi_device_init(ubi, &cfg);

const struct ubi_volume_config wanted = { .name = "logs", .leb_count = 8 };

ubi_volume_create(ubi, &wanted, &vol_id);
ubi_leb_change(ubi, vol_id, 0, buffer, sizeof(buffer));
ubi_leb_read(ubi, vol_id, 0, 0, buffer, sizeof(buffer));

ubi_device_deinit(ubi);
```

`ikm_key_id` is a PSA key handle the application owns. It must carry
`PSA_KEY_USAGE_DERIVE` and permit `PSA_ALG_HKDF(PSA_ALG_SHA_256)`. UBI derives
its own keys from it and never reads, copies or stores the material itself.
See [Security](security.md) before trusting the device unconditionally.

## Appending records

`ubi_leb_write_at` places bytes where it is told and remembers nothing, so the
application keeps its own offset. Offset and length are whole write blocks:

```c
struct ubi_device_info info = { 0 };
uint8_t record[64] = { 0 };
uint32_t offset = 0;

ubi_device_get_info(ubi, &info);

while (offset + sizeof(record) <= info.leb_size) {
	ubi_leb_write_at(ubi, vol_id, 0, offset, record, sizeof(record));
	offset += sizeof(record);
}

/* The block is full: start again on a fresh one. */
ubi_leb_erase(ubi, vol_id, 0);
offset = 0;
```

The same region may not be written twice without an `ubi_leb_change`,
`ubi_leb_unmap` or `ubi_leb_erase` in between. UBI stores no length, so after a
reboot the application finds its own place; the bytes past the last record read
back erased. `ubi_leb_unmap` also starts a fresh block and costs no erase, but
if the device reboots before anything new is written, the old contents come
back.

## Maintenance is yours to call

There is no background thread. Erasing, wear levelling and repair happen when
the application asks, with a budget it chooses:

```c
struct ubi_maintenance_result result = { 0 };

ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 4, &result);
```

A budget of zero only counts the work waiting. Keeping reclaim topped up is
what makes a write cost no erase at all.

## Running the tests

Standalone, using the manifest in the repository:

```sh
west init -m https://github.com/kamil-kielbasa/zephyr-ubi workspace
cd workspace
west update
west twister -T zephyr-ubi/tests -p native_sim
```

That builds and runs every configuration CI does: four flash geometries on an
8 MiB partition, the unit suite, and four Kconfig variants.
