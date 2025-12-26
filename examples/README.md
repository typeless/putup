# Pup Examples

## Busybox

Build [Busybox](https://busybox.net/) using pup. Files in `busybox/`.

## Quick Start

```bash
# 1. Download and extract busybox
wget https://busybox.net/downloads/busybox-1.38.0.tar.bz2
tar xjf busybox-1.38.0.tar.bz2
cd busybox-1.38.0

# 2. Copy pup build files
rsync -av /path/to/pup/examples/busybox/ ./

# 3. Build (generates .config automatically from defconfig)
make -f Makefile.pup
```

Or with a specific configuration:

```bash
# Android config
CONFIG=android_defconfig make -f Makefile.pup

# Or use pup directly
CONFIG=android_defconfig pup configure -B build-android
pup -B build-android
```

## Multi-Variant Builds

Build multiple configurations in parallel:

```bash
CONFIG=android_defconfig make -f Makefile.pup BUILD=build-android configure
CONFIG=freebsd_defconfig make -f Makefile.pup BUILD=build-freebsd configure
pup build-android build-freebsd -j$(nproc)
```

## Files (in `busybox/`)

| File | Purpose |
|------|---------|
| `Makefile.pup` | Make wrapper for pup commands |
| `Tupfile.ini` | Project root marker |
| `Tuprules.tup` | Compiler flags and macros |
| `Tupfile` | Config generation + source compilation rules |
| `scripts/kconfig/Tupfile` | Builds kconfig conf tool |
| `include/Tupfile` | Generated header rules |

## Generated Headers

The build generates these headers from `.config`:

- `applets.h`, `usage.h` - Applet definitions from source comments
- `autoconf.h` - Config macros from kconfig
- `applet_tables.h`, `NUM_APPLETS.h` - Applet lookup tables
- `usage_compressed.h` - Compressed help text
- `bbconfigopts.h` - Config option strings
- `embedded_scripts.h` - Embedded shell scripts (mim, nologin)

## Build Output

- `build/build/busybox` - Main binary (~1.2MB, 403 applets)
- `build/build/**/*.o` - Object files

## Notes

- The source list in `Tupfile` matches `make defconfig` configuration
- For different configs, regenerate the source list from a make build
- Requires: gcc, bzip2, od (for embedded scripts)
- If the build fails with TC errors (missing kernel headers), use `notc_defconfig`:
  ```bash
  CONFIG=notc_defconfig pup configure -B build
  CONFIG=notc_defconfig pup -B build
  ```
- **With `PUP_IMPLICIT_DEPS=1`**: Pass CONFIG in both configure and build phases.
  Implicit deps re-parses Tupfiles after discovering dependencies, and CONFIG must
  be set during both phases to select the correct defconfig.
