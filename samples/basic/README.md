# UBI basic

Attaches UBI to a partition, formats it only when it holds no UBI metadata,
and counts boots in a volume of one block. The state callback compares
`image_seq` and `revision` with an anchor kept in RAM;
[security.md](../../docs/security.md) describes a complete check. The key is
fixed in the source; a product provisions one per device and partition.

## Building and running

On `native_sim` the partition is the upper megabyte of the simulated flash,
kept in `flash.bin` between runs:

```sh
west build -b native_sim samples/basic -t run
```

```
ubi: no device on the partition, formatting
ubi: 256 blocks of 4096 bytes, image 0xcc31db3b, revision 1
boot count: 1
done
```

Each run counts one more boot. On the nRF5340 DK the partition is the first
megabyte of the external MX25R64:

```sh
west build -b nrf5340dk/nrf5340/cpuapp samples/basic
west flash
```

A partition holding UBI metadata the sample cannot use, such as one formatted
under another key, is reported and left alone.
