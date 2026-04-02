#!/bin/sh
# Install script for putup — a build system using Tupfile syntax.
# Usage: curl -fsSL https://raw.githubusercontent.com/typeless/putup/main/install.sh | sh
#
# Options (environment variables):
#   PUTUP_VERSION=v0.1.0          Install a specific version (default: latest)
#   PUTUP_INSTALL_DIR=/usr/local/bin  Install to a custom directory (default: ~/.local/bin)

set -eu

GITHUB_REPO="typeless/putup"
BASE_URL="https://github.com/${GITHUB_REPO}"

# --- Output helpers ---

if test -t 1; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    CYAN='\033[0;36m'
    BOLD='\033[1m'
    RESET='\033[0m'
else
    RED='' GREEN='' YELLOW='' CYAN='' BOLD='' RESET=''
fi

info()    { printf "${CYAN}info${RESET}  %s\n" "$*"; }
warn()    { printf "${YELLOW}warn${RESET}  %s\n" "$*" >&2; }
err()     { printf "${RED}error${RESET} %s\n" "$*" >&2; }
success() { printf "${GREEN}done${RESET}  %s\n" "$*"; }

# --- Root check ---

check_root() {
    if test "$(id -u)" -eq 0; then
        warn "Running as root. putup installs to ~/.local/bin by default (no root needed)."
        warn "Set PUTUP_INSTALL_DIR=/usr/local/bin if you intend a system-wide install."
    fi
}

# --- Platform detection ---

detect_platform() {
    OS=$(uname -s)
    ARCH=$(uname -m)

    case "${OS}" in
        Linux)
            case "${ARCH}" in
                x86_64)  BINARY_NAME="putup-linux-x86_64" ;;
                aarch64) err "Linux arm64/aarch64 binaries are not yet available."
                         err "Build from source: ${BASE_URL}/blob/main/INSTALL.md"
                         exit 1 ;;
                *)       err "Unsupported Linux architecture: ${ARCH}"
                         exit 1 ;;
            esac
            ;;
        Darwin)
            case "${ARCH}" in
                arm64)   BINARY_NAME="putup-macos-arm64" ;;
                x86_64)  err "Intel Mac (x86_64) binaries are not available."
                         err "Only Apple Silicon (arm64) is supported."
                         exit 1 ;;
                *)       err "Unsupported macOS architecture: ${ARCH}"
                         exit 1 ;;
            esac
            ;;
        MINGW*|MSYS*|CYGWIN*)
            case "${ARCH}" in
                x86_64)  BINARY_NAME="putup-windows-x86_64.exe" ;;
                *)       err "Unsupported Windows architecture: ${ARCH}"
                         exit 1 ;;
            esac
            ;;
        *)
            err "Unsupported operating system: ${OS}"
            err "Build from source: ${BASE_URL}/blob/main/INSTALL.md"
            exit 1
            ;;
    esac

    info "Detected platform: ${OS} ${ARCH}"
}

# --- Version resolution ---

resolve_version() {
    if test -n "${PUTUP_VERSION:-}"; then
        VERSION="${PUTUP_VERSION}"
        case "${VERSION}" in
            v*) ;;
            *)  VERSION="v${VERSION}" ;;
        esac
        info "Using specified version: ${VERSION}"
        return
    fi

    info "Resolving latest version..."
    tmpversion=$(mktemp)
    if ! download "https://api.github.com/repos/${GITHUB_REPO}/releases/latest" "${tmpversion}" 2>/dev/null; then
        rm -f "${tmpversion}"
        err "Failed to query GitHub API for latest release."
        err "Set PUTUP_VERSION=vX.Y.Z to skip the API query."
        err "Or build from source: ${BASE_URL}/blob/main/INSTALL.md"
        exit 1
    fi

    VERSION=$(sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' "${tmpversion}" | head -1)
    rm -f "${tmpversion}"

    if test -z "${VERSION}"; then
        err "No releases found. The project may not have published binaries yet."
        err "Build from source: ${BASE_URL}/blob/main/INSTALL.md"
        exit 1
    fi

    info "Latest version: ${VERSION}"
}

# --- Download abstraction ---

download() {
    url="$1"
    output="$2"

    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 --retry-delay 2 -o "${output}" "${url}"
    elif command -v wget >/dev/null 2>&1; then
        wget -q --tries=3 -O "${output}" "${url}"
    else
        err "Neither curl nor wget found. Install one and try again."
        exit 1
    fi
}

# --- Checksum abstraction ---

compute_sha256() {
    file="$1"

    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "${file}" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "${file}" | cut -d' ' -f1
    else
        echo ""
    fi
}

# --- Main ---

main() {
    printf "\n${BOLD}putup installer${RESET}\n\n"

    check_root
    detect_platform
    resolve_version

    INSTALL_DIR="${PUTUP_INSTALL_DIR:-${HOME:?HOME is not set. Use PUTUP_INSTALL_DIR to specify an install location.}/.local/bin}"
    mkdir -p "${INSTALL_DIR}"

    RELEASE_URL="${BASE_URL}/releases/download/${VERSION}"
    tmpdir=$(mktemp -d 2>/dev/null || { t="/tmp/putup-install-$$"; mkdir -p "$t"; echo "$t"; })
    trap 'rm -rf "${tmpdir}"' EXIT INT TERM

    # Download checksums (non-fatal if missing)
    SKIP_CHECKSUM=0
    if ! download "${RELEASE_URL}/sha256sums.txt" "${tmpdir}/sha256sums.txt" 2>/dev/null; then
        warn "Checksums not available for this release. Skipping verification."
        SKIP_CHECKSUM=1
    fi

    # Download binary
    info "Downloading ${BINARY_NAME} ${VERSION}..."
    if ! download "${RELEASE_URL}/${BINARY_NAME}" "${tmpdir}/${BINARY_NAME}"; then
        err "Failed to download binary from:"
        err "  ${RELEASE_URL}/${BINARY_NAME}"
        err ""
        err "Check that version ${VERSION} exists and has binaries attached."
        err "Available releases: ${BASE_URL}/releases"
        exit 1
    fi

    # Verify checksum
    if test "${SKIP_CHECKSUM}" -eq 0; then
        actual=$(compute_sha256 "${tmpdir}/${BINARY_NAME}")
        if test -z "${actual}"; then
            warn "No sha256 tool found. Skipping checksum verification."
        else
            expected=$(grep " ${BINARY_NAME}\$" "${tmpdir}/sha256sums.txt" | cut -d' ' -f1)
            if test -z "${expected}"; then
                warn "Binary not found in checksums file. Skipping verification."
            elif test "${actual}" != "${expected}"; then
                err "Checksum verification FAILED!"
                err "  Expected: ${expected}"
                err "  Actual:   ${actual}"
                err "The download may be corrupted or tampered with."
                exit 1
            else
                info "Checksum verified."
            fi
        fi
    fi

    # Install binary (use .exe on Windows)
    case "${OS}" in
        MINGW*|MSYS*|CYGWIN*)
            EXE_NAME="putup.exe"
            ALIAS_NAME="pup.exe"
            ;;
        *)
            EXE_NAME="putup"
            ALIAS_NAME="pup"
            ;;
    esac

    install -m 755 "${tmpdir}/${BINARY_NAME}" "${INSTALL_DIR}/${EXE_NAME}"

    # Create pup symlink/copy
    ln -sf "${EXE_NAME}" "${INSTALL_DIR}/${ALIAS_NAME}" 2>/dev/null \
        || cp "${INSTALL_DIR}/${EXE_NAME}" "${INSTALL_DIR}/${ALIAS_NAME}"

    # Verify
    if "${INSTALL_DIR}/${EXE_NAME}" --version >/dev/null 2>&1; then
        installed_version=$("${INSTALL_DIR}/${EXE_NAME}" --version | head -1)
        success "Installed ${installed_version} to ${INSTALL_DIR}/${EXE_NAME}"
    else
        warn "Binary installed but failed to execute. Check platform compatibility."
    fi

    # PATH check
    case ":${PATH}:" in
        *":${INSTALL_DIR}:"*)
            ;;
        *)
            printf "\n"
            warn "${INSTALL_DIR} is not in your PATH."
            shell_name=$(basename "${SHELL:-/bin/sh}")
            case "${shell_name}" in
                zsh)  printf "  Add to ~/.zshrc:   ${BOLD}export PATH=\"%s:\$PATH\"${RESET}\n" "${INSTALL_DIR}" ;;
                bash) printf "  Add to ~/.bashrc:  ${BOLD}export PATH=\"%s:\$PATH\"${RESET}\n" "${INSTALL_DIR}" ;;
                fish) printf "  Run:               ${BOLD}fish_add_path %s${RESET}\n" "${INSTALL_DIR}" ;;
                *)    printf "  Add to your shell profile: ${BOLD}export PATH=\"%s:\$PATH\"${RESET}\n" "${INSTALL_DIR}" ;;
            esac
            ;;
    esac

    printf "\n${BOLD}Get started:${RESET}\n"
    printf "  putup --help\n"
    printf "  ${CYAN}${BASE_URL}/blob/main/docs/reference.md${RESET}\n\n"
}

main
