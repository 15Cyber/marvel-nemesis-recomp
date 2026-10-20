#include "cpu/x86_ops.h"
X86_EXTERN_FUNC(sub_00001000);
X86_EXTERN_FUNC(sub_00002000);
X86_FUNC(__imp__sub_00001000) { ctx.eax = 11; }
X86_WEAK_FUNC(sub_00001000, __imp__sub_00001000)
X86_FUNC(__imp__sub_00002000) { ctx.eax = 22; ctx.ebx = uint32_t(x86_rdtsc()); ctx.st[0] = std::remainder(ctx.st[0], ctx.st[1]); }
X86_WEAK_FUNC(sub_00002000, __imp__sub_00002000)
X86_FUNC(TestCallOverride) { sub_00001000(ctx, base); }
X86_FUNC(TestThrow) { throw X86Resume{123, 456}; }
extern "C" const X86FuncMapping X86FuncMappings[] = {{0x1000, sub_00001000}, {0x2000, sub_00002000}, {0, nullptr}};
