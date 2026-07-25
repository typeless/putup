# Installation

## Quick Install

```bash
curl -fsSL https://raw.githubusercontent.com/typeless/putup/main/install.sh | sh
```

Options:
- `PUTUP_VERSION=v2.1.0` — install a specific version
- `PUTUP_INSTALL_DIR=/usr/local/bin` — install to a custom directory (default: `~/.local/bin`)

## Requirements

- C++20 compiler: GCC 11+ or Clang 14+

## Building from Source

```bash
git clone <repository-url>
cd putup
make
sudo install build/putup /usr/local/bin/
```

`make` is self-hosting: it invokes an existing putup (the Makefile calls
`$(PUTUP)`, which defaults to `$(PREFIX)/bin/putup` with `PREFIX ?= $(HOME)`, i.e.
`~/bin/putup`). On a fresh system without putup, run `./bootstrap-linux.sh` (or
`./bootstrap-macos.sh`) first to produce the initial binary, or point the Makefile
at an existing one with `make PUTUP=/path/to/putup`.

> **Note:** the quick-install `install.sh` installs to `~/.local/bin` by default,
> whereas the Makefile looks for putup under `~/bin`. If you installed via the
> script, pass `make PREFIX=~/.local` (or copy the binary to `~/bin`) so the
> self-hosting build can find it.

## Bootstrapping

Putup is self-hosting (it builds itself with its own Tupfiles). Bootstrap scripts are provided for initial installation on a system without `putup`:

```bash
./bootstrap-linux.sh    # Linux
./bootstrap-macos.sh    # macOS
```

Windows binaries are cross-compiled from Linux (clang-cl + xwin) by the release workflow — download a prebuilt `putup-windows-x86_64.exe` from the Releases page.

### Regenerating Bootstrap Scripts

`putup show script` emits the compile commands recorded in an already-configured
build directory. Both scripts come from a single Linux host — the macOS script
needs the macOS *config*, not a macOS machine — so regenerate them together:

```bash
make bootstrap
```

Commit the result with the `Tupfile`, `Tuprules.tup`, or `configs/` change that
caused it. CI regenerates and fails on any diff, so a stale script blocks the PR
rather than landing on someone else's.
