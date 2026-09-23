# zephyr-ubi

A volume manager for raw NOR flash, built as a Zephyr module. It divides one
flash partition into volumes, spreads wear across it, keeps block updates safe
from power loss, and authenticates its own metadata with AES-CMAC. Application
data is not encrypted.

## Documentation

<https://kamil-kielbasa.github.io/zephyr-ubi/>

## License

MIT. See [LICENSE](LICENSE).

## Contact

email: kamkie1996@gmail.com
