#define NOMINMAX
#include <windows.h>
#include <cassert>
#include "cpu/x86_ops.h"
X86_FUNC(sub_00001000) { ctx.eax = 77; }
X86_EXTERN_FUNC(sub_00002000);
X86_EXTERN_FUNC(__imp__sub_00001000);
X86_HOST_API uint64_t x86_rdtsc() { return 1234; }
#pragma comment(linker, "/EXPORT:__CxxFrameHandler3=vcruntime140.__CxxFrameHandler3")
#pragma comment(linker, "/EXPORT:_CxxThrowException=vcruntime140._CxxThrowException")
#pragma comment(linker, "/EXPORT:remainder=ucrtbase.remainder")
int main() {
    HMODULE dll = LoadLibraryW(L"Fixture.dll");
    assert(dll);
    auto mappings = reinterpret_cast<const X86FuncMapping*>(GetProcAddress(dll, "X86FuncMappings"));
    assert(mappings);
    X86Context ctx{};
    mappings[0].host(ctx, nullptr); assert(ctx.eax == 77);
    ctx.st[0] = 5.0; ctx.st[1] = 2.0;
    mappings[1].host(ctx, nullptr); assert(ctx.eax == 22 && ctx.ebx == 1234 && ctx.st[0] == 1.0);
    __imp__sub_00001000(ctx, nullptr); assert(ctx.eax == 11);
    auto call = reinterpret_cast<X86Func*>(GetProcAddress(dll, "TestCallOverride"));
    assert(call); call(ctx, nullptr); assert(ctx.eax == 77);
    auto raise = reinterpret_cast<X86Func*>(GetProcAddress(dll, "TestThrow"));
    assert(raise);
    bool caught = false;
    try { raise(ctx, nullptr); }
    catch (const X86Resume& r) { caught = r.target == 123 && r.esp == 456; }
    assert(caught);
}
