#include "crashlog.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <dbghelp.h>
#endif

namespace xr {
namespace crashlog {

namespace {
Sink g_sink = nullptr;

const char* codeName(unsigned long code) {
#ifdef _WIN32
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "access violation";
        case EXCEPTION_IN_PAGE_ERROR:         return "in-page error";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION:      return "privileged instruction";
        case EXCEPTION_STACK_OVERFLOW:        return "stack overflow";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "integer divide by zero";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "float divide by zero";
        case EXCEPTION_FLT_INVALID_OPERATION: return "float invalid operation";
        case 0xE06D7363:                      return "C++ exception";
        default: break;
    }
#endif
    (void)code;
    return "exception";
}

// The codes worth a line: the process is in trouble, or about to be.
bool fatal(unsigned long code) {
#ifdef _WIN32
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_DATATYPE_MISALIGNMENT:
            return true;
        default: return false;
    }
#else
    (void)code;
    return false;
#endif
}

// Fixed buffers only: the heap may be what broke.
void formatAddress(const void* addr, char* out, size_t n) {
#ifdef _WIN32
    HMODULE h = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &h) && h) {
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(h, path, sizeof path);
        const char* base = path;
        for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
        snprintf(out, n, "%s+0x%llx", *base ? base : "?", (unsigned long long)((const char*)addr - (const char*)h));
        return;
    }
#endif
    snprintf(out, n, "0x%llx", (unsigned long long)(uintptr_t)addr);
}

#ifdef _WIN32
// dbghelp, resolved once at install: the handler must not load libraries.
typedef BOOL (WINAPI *StackWalk64Fn)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
                                     PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
HMODULE                         g_dbghelp = nullptr;
StackWalk64Fn                   g_stackWalk = nullptr;
PVOID                           g_vectored = nullptr;

// What StackWalk64 asks for on the way up: the unwind information of a function and the base of its module.
// dbghelp's own SymFunctionTableAccess64 answers only once its symbol handler is initialised (which loads every
// module's symbols, not something to do inside the sim); the system's RtlLookupFunctionEntry answers for any
// x64 / ARM64 code at once, and that is how optimised code without frame pointers is unwound.
PVOID WINAPI functionEntry(HANDLE, DWORD64 addr) {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_ARM64) || defined(__aarch64__)
    DWORD64 base = 0;
    return (PVOID)RtlLookupFunctionEntry((DWORD64)addr, &base, nullptr);
#else
    (void)addr;
    return nullptr;
#endif
}
DWORD64 WINAPI moduleBase(HANDLE, DWORD64 addr) {
    HMODULE h = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)addr, &h);
    return (DWORD64)(uintptr_t)h;
}
LPTOP_LEVEL_EXCEPTION_FILTER    g_prevFilter = nullptr;
bool                            g_filterSet = false;
std::atomic<int>                g_written{0};
constexpr int                   kMaxReports = 3;

void emit(const char* line) { if (g_sink) g_sink(line); }

void writeReport(EXCEPTION_POINTERS* ep, const char* how) {
    if (!ep || !ep->ExceptionRecord) return;
    const EXCEPTION_RECORD& r = *ep->ExceptionRecord;
    char where[300], line[600];
    formatAddress(r.ExceptionAddress, where, sizeof where);
    snprintf(line, sizeof line, "XRadio crash: %s (0x%08lx) at %s%s\n", codeName(r.ExceptionCode), (unsigned long)r.ExceptionCode, where, how);
    emit(line);
    if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2) {
        snprintf(line, sizeof line, "XRadio crash: %s address 0x%llx\n", r.ExceptionInformation[0] == 1 ? "writing" : r.ExceptionInformation[0] == 8 ? "executing" : "reading",
                 (unsigned long long)r.ExceptionInformation[1]);
        emit(line);
    }
    if (!g_stackWalk || !ep->ContextRecord) return;
    CONTEXT ctx = *ep->ContextRecord;        // StackWalk64 changes it
    STACKFRAME64 f;
    memset(&f, 0, sizeof f);
    DWORD machine;
#if defined(_M_X64) || defined(__x86_64__)
    machine = IMAGE_FILE_MACHINE_AMD64;
    f.AddrPC.Offset = ctx.Rip; f.AddrFrame.Offset = ctx.Rbp; f.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64) || defined(__aarch64__)
    machine = IMAGE_FILE_MACHINE_ARM64;
    f.AddrPC.Offset = ctx.Pc; f.AddrFrame.Offset = ctx.Fp; f.AddrStack.Offset = ctx.Sp;
#else
    machine = IMAGE_FILE_MACHINE_I386;
    f.AddrPC.Offset = ctx.Eip; f.AddrFrame.Offset = ctx.Ebp; f.AddrStack.Offset = ctx.Esp;
#endif
    f.AddrPC.Mode = f.AddrFrame.Mode = f.AddrStack.Mode = AddrModeFlat;
    const HANDLE proc = GetCurrentProcess(), thread = GetCurrentThread();
    for (int i = 0; i < 16; ++i) {
        if (!g_stackWalk(machine, proc, thread, &f, &ctx, nullptr, functionEntry, moduleBase, nullptr) || !f.AddrPC.Offset) break;
        formatAddress((const void*)(uintptr_t)f.AddrPC.Offset, where, sizeof where);
        snprintf(line, sizeof line, "XRadio crash:   #%d %s\n", i, where);
        emit(line);
    }
}

// Which module we are: a first-chance exception is only ours to mention when it happened in our code (other
// plugins and the runtime raise and catch their own).
HMODULE ownModule() {
    HMODULE h = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&ownModule, &h);
    return h;
}

LONG WINAPI vectoredHandler(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord && fatal(ep->ExceptionRecord->ExceptionCode)) {
        HMODULE h = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &h);
        if (h && h == ownModule() && g_written.fetch_add(1) < kMaxReports) writeReport(ep, " (in the plugin's code)");
    }
    return EXCEPTION_CONTINUE_SEARCH;          // never handled here: whoever handled it before still does
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
    // the last chance: nobody handled it, the process ends -- say where, wherever it was
    if (g_written.fetch_add(1) < kMaxReports + 1) writeReport(ep, " (unhandled: X-Plane is going down)");
    if (g_prevFilter) return g_prevFilter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
}  // namespace

bool install(Sink sink) {
    g_sink = sink;
#ifdef _WIN32
    if (g_vectored) return true;
    g_dbghelp = LoadLibraryA("dbghelp.dll");
    if (g_dbghelp) g_stackWalk = (StackWalk64Fn)(void*)GetProcAddress(g_dbghelp, "StackWalk64");
    g_vectored = AddVectoredExceptionHandler(1, vectoredHandler);
    g_prevFilter = SetUnhandledExceptionFilter(unhandledFilter);
    g_filterSet = true;
    return g_vectored != nullptr;
#else
    return false;
#endif
}

void uninstall() {
#ifdef _WIN32
    if (g_vectored) { RemoveVectoredExceptionHandler(g_vectored); g_vectored = nullptr; }
    if (g_filterSet) { SetUnhandledExceptionFilter(g_prevFilter); g_prevFilter = nullptr; g_filterSet = false; }
    if (g_dbghelp) { FreeLibrary(g_dbghelp); g_dbghelp = nullptr; g_stackWalk = nullptr; }
#endif
    g_sink = nullptr;
}

std::string describeAddress(const void* addr) {
    char where[300];
    formatAddress(addr, where, sizeof where);
    return where;
}

std::string describeFault(unsigned long code, const void* addr) {
    char line[600];
    snprintf(line, sizeof line, "XRadio crash: %s (0x%08lx) at %s", codeName(code), code, describeAddress(addr).c_str());
    return line;
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
void crashNow() {
    volatile int* p = (volatile int*)(uintptr_t)0x10;   // nothing is mapped there
    *p = 1;
}

}  // namespace crashlog
}  // namespace xr
