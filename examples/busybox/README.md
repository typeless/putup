# Busybox

Build [Busybox](https://busybox.net/) using putup.

## Quick Start

```bash
# 1. Download and extract busybox
wget https://busybox.net/downloads/busybox-1.38.0.tar.bz2
tar xjf busybox-1.38.0.tar.bz2

# 2. Build (generates .config automatically from defconfig)
putup -S busybox-1.38.0 -B build
```

Or with a specific configuration:

```bash
# Android config
CONFIG=android_defconfig putup configure -S busybox-1.38.0 -B build-android
putup -S busybox-1.38.0 -B build-android
```

## Multi-Variant Builds

Build multiple configurations in parallel:

```bash
CONFIG=android_defconfig putup configure -S busybox-1.38.0 -B build-android
CONFIG=freebsd_defconfig putup configure -S busybox-1.38.0 -B build-freebsd
putup build-android build-freebsd -j$(nproc)
```

## Files

| File | Purpose |
|------|---------|
| `Makefile.pup` | Make wrapper for putup commands |
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

- `build/busybox` - Main binary (~1.2MB, 403 applets)
- `build/**/*.o` - Object files

## Notes

- The source list in `Tupfile` matches `make defconfig` configuration
- For different configs, regenerate the source list from a make build
- Requires: gcc, bzip2, od (for embedded scripts)
- If the build fails with TC errors (missing kernel headers), use `notc_defconfig`:
  ```bash
  CONFIG=notc_defconfig putup configure -S busybox-1.38.0 -B build
  putup -S busybox-1.38.0 -B build
  ```
- **With `PUP_IMPLICIT_DEPS=1`**: Pass CONFIG in both configure and build phases.
  Implicit deps re-parses Tupfiles after discovering dependencies, and CONFIG must
  be set during both phases to select the correct defconfig.
