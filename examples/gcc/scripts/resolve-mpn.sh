#!/bin/sh
# resolve-mpn.sh — Resolve GMP mpn sources for a given CPU target
#
# Usage: resolve-mpn.sh <cpu> <mpn_srcdir>
#
# Outputs tup.config entries to stdout for gmp/mpn/tup.config.
# Generic mode emits all C sources with template functions enabled.
# Assembly mode walks the priority chain, emitting per-function y/n
# toggles for the foo-y conditional compilation pattern.

set -e

cpu="$1"
mpn_src="$2"

if [ -z "$cpu" ] || [ -z "$mpn_src" ]; then
    echo "Usage: resolve-mpn.sh <cpu> <mpn_srcdir>" >&2
    exit 1
fi

# --- Multi-function file → output function mapping ---
# Static across all architectures. Only the directory changes.

multi_funcs() {
    case "$1" in
        aors_n)      echo "add_n sub_n" ;;
        aorsmul_1)   echo "addmul_1 submul_1" ;;
        aors_err1_n) echo "add_err1_n sub_err1_n" ;;
        aors_err2_n) echo "add_err2_n sub_err2_n" ;;
        aors_err3_n) echo "add_err3_n sub_err3_n" ;;
        logops_n)    echo "and_n andn_n ior_n iorn_n nand_n nior_n xnor_n xor_n" ;;
        popham)      echo "popcount hamdist" ;;
        sec_aors_1)  echo "sec_add_1 sec_sub_1" ;;
    esac
}

is_multi_asm() {
    case "$1" in
        aors_n|aorsmul_1|aors_err1_n|aors_err2_n|aors_err3_n) return 0 ;;
        logops_n|popham|sec_aors_1) return 0 ;;
        *) return 1 ;;
    esac
}

is_template_file() {
    case "$1" in
        logops_n|popham|sec_aors_1|sec_div|sec_pi1_div) return 0 ;;
        *) return 1 ;;
    esac
}

# All template output functions (16 total)
all_template_funcs="and_n andn_n ior_n iorn_n nand_n nior_n xnor_n xor_n
popcount hamdist sec_add_1 sec_sub_1 sec_div_qr sec_div_r sec_pi1_div_qr sec_pi1_div_r"

# --- Generic mode ---

if [ "$cpu" = "generic" ]; then
    c_files=""
    for f in "$mpn_src"/generic/*.c; do
        [ -f "$f" ] || continue
        func=$(basename "$f" .c)
        is_template_file "$func" && continue
        c_files="${c_files:+$c_files }generic/$(basename "$f")"
    done

    echo "CONFIG_MPN_ARCH=generic"
    echo "CONFIG_MPN_SUBDIR=generic"
    echo "CONFIG_C_SOURCES=$c_files"
    echo "CONFIG_ASM_SOURCES="
    for func in $all_template_funcs; do
        echo "CONFIG_C_$func=y"
    done
    exit 0
fi

# --- Assembly mode ---

arch="${cpu%%/*}"
if [ "$cpu" = "$arch" ]; then
    cpu_subdir=""
else
    cpu_subdir="${cpu#*/}"
fi

# Walk priority chain: cpu-specific dir → arch dir.
# Higher priority wins; once a function is resolved, lower levels skip it.
asm_funcs=""           # all functions with asm (for C exclusion)
single_asm_files=""    # single-function .asm paths (for ASM_SOURCES)
multi_asm_funcs=""     # functions from multi-function .asm (for CONFIG_ASM_ entries)

collect_asm() {
    dir="$1"
    full="$mpn_src/$dir"
    [ -d "$full" ] || return 0
    for f in "$full"/*.asm; do
        [ -f "$f" ] || continue
        base=$(basename "$f" .asm)

        if is_multi_asm "$base"; then
            funcs=$(multi_funcs "$base")
            any_new=false
            for func in $funcs; do
                case " $asm_funcs " in
                    *" $func "*) ;;
                    *) any_new=true; break ;;
                esac
            done
            $any_new || continue

            for func in $funcs; do
                case " $asm_funcs " in
                    *" $func "*) ;;
                    *) asm_funcs="$asm_funcs $func"
                       multi_asm_funcs="$multi_asm_funcs $func" ;;
                esac
            done
        else
            case " $asm_funcs " in
                *" $base "*) continue ;;
            esac
            asm_funcs="$asm_funcs $base"
            single_asm_files="${single_asm_files:+$single_asm_files }$dir/$(basename "$f")"
        fi
    done
}

if [ -n "$cpu_subdir" ]; then
    collect_asm "$arch/$cpu_subdir"
fi
collect_asm "$arch"

# Collect generic C sources, excluding:
# - Functions replaced by asm (single or multi-function)
# - Template source files (handled by foo-y rules)
c_files=""
for f in "$mpn_src"/generic/*.c; do
    [ -f "$f" ] || continue
    func=$(basename "$f" .c)
    is_template_file "$func" && continue
    skip=false
    for af in $asm_funcs; do
        if [ "$func" = "$af" ]; then
            skip=true
            break
        fi
    done
    $skip || c_files="${c_files:+$c_files }generic/$(basename "$f")"
done

# --- Output tup.config entries ---

echo "CONFIG_MPN_ARCH=$arch"
echo "CONFIG_MPN_SUBDIR=$cpu_subdir"
echo "CONFIG_C_SOURCES=$c_files"
echo "CONFIG_ASM_SOURCES=$single_asm_files"

# Per-function asm toggles (multi-function asm outputs only)
for func in $multi_asm_funcs; do
    echo "CONFIG_ASM_$func=y"
done

# Template C toggles: y when no asm, n when asm replaces it
for func in $all_template_funcs; do
    case " $asm_funcs " in
        *" $func "*) echo "CONFIG_C_$func=n" ;;
        *) echo "CONFIG_C_$func=y" ;;
    esac
done
