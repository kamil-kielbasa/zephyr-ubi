# zephyr-ubi

A volume manager for raw NOR flash, built as a Zephyr module.

It maps logical erase blocks onto physical ones, spreads wear across the
partition, survives power loss, and authenticates its own metadata with
AES-CMAC so that a changed header is noticed rather than obeyed.

The on-flash headers follow Linux UBI field for field, with the MAC placed in
space Linux reserves as padding. An erase counter header written here passes
validation in unmodified Linux UBI.

**It does not encrypt your data.** Only metadata is protected. See
[doc/security.md](doc/security.md) for what that covers and what it does not.

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

```conf
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
	return UBI_STATE_TRUSTED;	/* read doc/security.md before shipping */
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

## Maintenance is yours to call

There is no background thread. Erasing, wear levelling and repair happen when
the application asks, with a budget it chooses:

```c
struct ubi_maintenance_result result = { 0 };

ubi_maintenance(ubi, UBI_MAINTENANCE_RECLAIM, 4, &result);
```

A budget of zero only counts the work waiting. Keeping reclaim topped up is
what makes a write cost no erase at all.

## Documentation

Three short documents. The full contract for every call lives in
[include/ubi/ubi.h](include/ubi/ubi.h).

| | |
|---|---|
| [doc/how-it-works.md](doc/how-it-works.md) | blocks, attach, pools, maintenance, and what came from Linux |
| [doc/on-flash-format.md](doc/on-flash-format.md) | byte layouts, what the MAC covers, key derivation |
| [doc/security.md](doc/security.md) | threat model, the boundary, and the contract for your own encryption |

## Building and testing

Standalone, using the manifest in this repository:

```sh
west init -m https://github.com/kamil-kielbasa/zephyr-ubi workspace
cd workspace
west update
west twister -T zephyr-ubi/tests -p native_sim
```

That builds and runs every configuration CI does: three flash geometries, the
unit suite, and the two switches that change what the library does.

## Licence

MIT. See [LICENSE](LICENSE).
