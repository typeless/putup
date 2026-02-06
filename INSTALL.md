# Installation

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
./bootstrap-mingw.sh    # Windows (MSYS2/MinGW)
```

### Regenerating Bootstrap Scripts

After making changes to the build, regenerate the scripts with:

```bash
putup show script -B build > bootstrap-linux.sh
CONFIG=macosx putup show script -B build > bootstrap-macos.sh
CONFIG=mingw putup show script -B build > bootstrap-mingw.sh
```
