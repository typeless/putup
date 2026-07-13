# Installation

## Quick Install

```bash
curl -fsSL https://raw.githubusercontent.com/typeless/putup/main/install.sh | sh
```

Options:
- `PUTUP_VERSION=v0.1.0` — install a specific version
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

## Bootstrapping

Putup is self-hosting (it builds itself with its own Tupfiles). Bootstrap scripts are provided for initial installation on a system without `putup`:

```bash
./bootstrap-linux.sh    # Linux
./bootstrap-macos.sh    # macOS
```

Windows binaries are cross-compiled from Linux (clang-cl + xwin) by the release workflow — download a prebuilt `putup-windows-x86_64.exe` from the Releases page.

### Regenerating Bootstrap Scripts

After making changes to the build, regenerate the scripts with:

```bash
putup show script -B build > bootstrap-linux.sh
CONFIG=macosx putup show script -B build > bootstrap-macos.sh
```
