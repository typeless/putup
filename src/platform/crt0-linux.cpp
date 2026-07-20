// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

// Freestanding C runtime for the -nostdlib -static Linux release binary.
// Provides the program entry point, the mem/str primitives the compiler
// emits calls to, C++ static init/atexit, and the stack-protector TLS.
//
// Compiled with -fno-stack-protector (its own functions run before %fs is
// set up) and -fno-builtin (so the naive mem/str bodies below are not
// pattern-matched back into calls to themselves). Linked ONLY into the
// shipped binary; the test binary keeps libc.

#if defined(__linux__) && defined(__x86_64__)

#    include <cstddef>
#    include <cstdint>

#    pragma GCC diagnostic ignored "-Wmain"
#    pragma GCC diagnostic ignored "-Wpedantic"

extern "C" auto main(int argc, char** argv) -> int;
extern "C" void __cxa_finalize(void* dso); // NOLINT

namespace {

auto sys3(long n, long a, long b, long c) -> long
{
    long ret = 0; // NOLINT
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return ret;
}

auto sys_write(int fd, void const* buf, std::size_t len) -> void
{
    sys3(1 /*write*/, fd, reinterpret_cast<long>(buf), static_cast<long>(len));
}

[[noreturn]]
auto sys_exit_group(int code) -> void
{
    sys3(231 /*exit_group*/, code, 0, 0);
    __builtin_unreachable();
}

// Minimal thread-control block: the stack-protector reads its canary from
// %fs:0x28, so %fs must point at a block with a valid canary. tcb[0] is the
// conventional self-pointer, tcb[5] (offset 0x28) is the guard.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
alignas(16) std::uint64_t g_tcb[16];

struct AtexitEntry {
    void (*fn)(void*);
    void* arg;
};
AtexitEntry g_atexit[128];
int g_atexit_count = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

extern "C" {
extern void (*__init_array_start[])(); // NOLINT — linker-provided bounds of .init_array
extern void (*__init_array_end[])();   // NOLINT
}

auto setup_tls(char** envp) -> void
{
    auto** p = envp;
    while (*p != nullptr) {
        ++p;
    }
    ++p; // step past the NULL terminating envp; auxv follows
    auto* auxv = reinterpret_cast<std::uint64_t*>(p);

    auto canary = std::uint64_t { 0 };
    for (auto* a = auxv; a[0] != 0 /*AT_NULL*/; a += 2) {
        if (a[0] == 25 /*AT_RANDOM*/) {
            auto const* rnd = reinterpret_cast<unsigned char const*>(a[1]);
            for (auto i = 0; i < 8; ++i) {
                canary = (canary << 8) | rnd[i];
            }
        }
    }
    canary &= ~std::uint64_t { 0xff }; // null low byte stops string-based overwrite

    g_tcb[0] = reinterpret_cast<std::uint64_t>(&g_tcb[0]);
    g_tcb[5] = canary; // 5 * 8 == 0x28
    sys3(158 /*arch_prctl*/, 0x1002 /*ARCH_SET_FS*/, reinterpret_cast<long>(&g_tcb[0]), 0);
}

auto run_init_array() -> void
{
    auto const n = __init_array_end - __init_array_start;
    for (auto i = decltype(n) { 0 }; i < n; ++i) {
        __init_array_start[i]();
    }
}

} // namespace

extern "C" {

char** environ = nullptr; // NOLINT

// The kernel enters here with rsp pointing at argc; below it lie argv, a NULL,
// envp, a NULL, then the auxiliary vector. Capture rsp and hand it to C.
asm(
    ".global _start\n"
    ".type _start,@function\n"
    "_start:\n"
    "  xor %ebp, %ebp\n" // outermost frame per ABI
    "  mov %rsp, %rdi\n" // rdi = &argc
    "  and $-16, %rsp\n" // 16-byte align before the call
    "  call _start_main\n"
    "  hlt\n"
);

[[noreturn]]
void _start_main(std::uint64_t* sp)
{
    auto argc = static_cast<int>(sp[0]);
    auto** argv = reinterpret_cast<char**>(sp + 1);
    environ = argv + argc + 1;

    setup_tls(environ);
    run_init_array();

    auto code = main(argc, argv);
    __cxa_finalize(nullptr);
    sys_exit_group(code);
}

// --- process exit / abort ---

int __cxa_atexit(void (*fn)(void*), void* arg, void* /*dso*/) // NOLINT
{
    if (g_atexit_count < 128) {
        g_atexit[g_atexit_count++] = { fn, arg };
    }
    return 0;
}

int atexit(void (*fn)()) // NOLINT
{
    return __cxa_atexit(reinterpret_cast<void (*)(void*)>(fn), nullptr, nullptr);
}

void __cxa_finalize(void* /*dso*/) // NOLINT
{
    while (g_atexit_count > 0) {
        auto const& e = g_atexit[--g_atexit_count];
        e.fn(e.arg);
    }
}

void* __dso_handle = nullptr; // NOLINT

[[noreturn]]
void exit(int code) // NOLINT
{
    __cxa_finalize(nullptr);
    sys_exit_group(code);
}

[[noreturn]]
void abort() // NOLINT
{
    sys_exit_group(134); // 128 + SIGABRT
}

[[noreturn]]
void __stack_chk_fail() // NOLINT
{
    static constexpr char msg[] = "fatal: stack smashing detected\n";
    sys_write(2, msg, sizeof(msg) - 1);
    sys_exit_group(134);
}

// --- compiler-emitted primitives ---

void* memcpy(void* dest, void const* src, std::size_t n) // NOLINT
{
    void* d = dest;
    asm volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dest;
}

void* memmove(void* dest, void const* src, std::size_t n) // NOLINT
{
    void* d = dest;
    if (reinterpret_cast<std::uintptr_t>(dest) - reinterpret_cast<std::uintptr_t>(src) >= n) {
        asm volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    } else {
        d = static_cast<char*>(dest) + n - 1;
        src = static_cast<char const*>(src) + n - 1;
        asm volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(src), "+c"(n) : : "memory", "cc");
    }
    return dest;
}

void* memset(void* dest, int c, std::size_t n) // NOLINT
{
    void* d = dest;
    asm volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(static_cast<unsigned char>(c)) : "memory");
    return dest;
}

int memcmp(void const* a, void const* b, std::size_t n) // NOLINT
{
    auto const* pa = static_cast<unsigned char const*>(a);
    auto const* pb = static_cast<unsigned char const*>(b);
    for (std::size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) {
            return static_cast<int>(pa[i]) - static_cast<int>(pb[i]);
        }
    }
    return 0;
}

void* memchr(void const* s, int c, std::size_t n) // NOLINT
{
    auto const* p = static_cast<unsigned char const*>(s);
    auto uc = static_cast<unsigned char>(c);
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] == uc) {
            return const_cast<unsigned char*>(p + i); // NOLINT
        }
    }
    return nullptr;
}

std::size_t strlen(char const* s) // NOLINT
{
    auto const* p = s;
    while (*p != '\0') {
        ++p;
    }
    return static_cast<std::size_t>(p - s);
}

int strcmp(char const* a, char const* b) // NOLINT
{
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return static_cast<int>(static_cast<unsigned char>(*a)) - static_cast<int>(static_cast<unsigned char>(*b));
}

int strncmp(char const* a, char const* b, std::size_t n) // NOLINT
{
    for (std::size_t i = 0; i < n; ++i) {
        auto ca = static_cast<unsigned char>(a[i]);
        auto cb = static_cast<unsigned char>(b[i]);
        if (ca != cb || ca == '\0') {
            return static_cast<int>(ca) - static_cast<int>(cb);
        }
    }
    return 0;
}

void* __memcpy_chk(void* dest, void const* src, std::size_t n, std::size_t /*destlen*/) // NOLINT
{
    return memcpy(dest, src, n);
}

void* __memmove_chk(void* dest, void const* src, std::size_t n, std::size_t /*destlen*/) // NOLINT
{
    return memmove(dest, src, n);
}

int __popcountdi2(long long a) // NOLINT
{
    auto x = static_cast<std::uint64_t>(a);
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return static_cast<int>((x * 0x0101010101010101ULL) >> 56);
}

} // extern "C"

#endif // __linux__ && __x86_64__
