// Hooks for default.xbe. A strong definition replaces the recompiled function at that address.
#include "xdk/hle.h"
#include "xdk/d3d8.h"
#include "core/log.h"
#include "core/diag.h"

#include <set>
#include <utility>

namespace {

// World alpha/add materials include NYC's asphalt and roadway. Frame captures only
// see submitted GPU draws; log the game's callbacks to locate an earlier omission.
bool TraceWorldMaterial(const char* pass, uint32_t material, uint32_t mesh, bool instance) {
    if (!diag::GpuTrace()) return false;
    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
    static std::set<std::pair<const char*, uint32_t>> seen;
    if (!material || !mesh || seen.size() >= 64 || !seen.emplace(pass, material).second) return false;
    uint32_t texture = kernel::R32(material + 0x18);
    uint32_t header = texture ? kernel::R32(texture + 0x0C) : 0;
    uint32_t data = header ? kernel::R32(header + 4) : 0;
    uint32_t strips = kernel::R16(mesh + (instance ? 0x14 : 0x68));
    uint32_t triangles = kernel::R16(mesh + (instance ? 0x16 : 0x6A));
    LOGR("world-material: %s material %08X flags %08X texture %08X header %08X data %08X mesh %08X strip %u list %u scene-flags %08X",
         pass, material, kernel::R32(material + 0x10), texture, header, data, mesh, strips, triangles,
         kernel::R32(0x00528530));
    return true;
}

void TraceWorldResult(const char* pass, uint32_t result) {
    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
    const auto& s = d3d::State();
    uint32_t header = s.textures[0];
    LOGR("world-material: %s returned %08X bound-texture %08X data %08X vs %08X ps %08X color-write %08X",
         pass, result, header, header ? kernel::R32(header + 4) : 0, s.vs_handle,
         s.ps ? s.ps->handle : 0, s.simple[gpu::kMColorWriteEnable >> 2]);
}

}  // namespace

X86_FUNC(__imp__sub_001DDF60);
X86_FUNC(__imp__sub_001DE0D0);
X86_FUNC(__imp__sub_001DA510);
X86_FUNC(__imp__sub_001DA6F0);
X86_FUNC(__imp__sub_001CE690);

// The world material callback reaches this shared submission routine. Inspect its
// actual mesh argument, including the embedded push buffer selected by flag 0x2000.
X86_FUNC(sub_001CE690) {
    if (diag::GpuTrace()) {
        uint32_t mesh = kernel::R32(ctx.esp + 4);
        std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
        static std::set<uint32_t> seen;
        if (mesh && seen.size() < 64 && seen.emplace(mesh).second) {
            uint32_t flags = kernel::R32(mesh + 0x10);
            uint32_t pb = mesh + 0x40;
            uint32_t texture = d3d::State().textures[0];
            LOGR("world-submit: mesh %08X flags %08X path %s strip %u list %u index-base %08X index-offset %u texture-data %08X pb-common %08X pb-data %08X pb-lock %08X pb-size %u pb-allocation %u pb-run %08X",
                 mesh, flags, (flags & 0x2000) ? "pushbuffer" : "indexed",
                 kernel::R16(mesh + 0x68), kernel::R16(mesh + 0x6A),
                 kernel::R32(0x003A3304), kernel::R32(mesh + 0x5C),
                 texture ? kernel::R32(texture + 4) : 0,
                 kernel::R32(pb), kernel::R32(pb + 4), kernel::R32(pb + 8),
                 kernel::R32(pb + 12), kernel::R32(pb + 16), kernel::R32(pb + 20));
        }
    }
    __imp__sub_001CE690(ctx, base);
}

X86_FUNC(sub_001DDF60) {
    bool trace = diag::GpuTrace() && TraceWorldMaterial("alpha-world", ctx.ecx, kernel::R32(ctx.esp + 4), false);
    __imp__sub_001DDF60(ctx, base);
    if (trace) TraceWorldResult("alpha-world", ctx.eax);
}

X86_FUNC(sub_001DE0D0) {
    bool trace = diag::GpuTrace() && TraceWorldMaterial("alpha-instance", ctx.ecx, kernel::R32(ctx.esp + 4), true);
    __imp__sub_001DE0D0(ctx, base);
    if (trace) TraceWorldResult("alpha-instance", ctx.eax);
}

X86_FUNC(sub_001DA510) {
    bool trace = diag::GpuTrace() && TraceWorldMaterial("add-world", ctx.ecx, kernel::R32(ctx.esp + 4), false);
    __imp__sub_001DA510(ctx, base);
    if (trace) TraceWorldResult("add-world", ctx.eax);
}

X86_FUNC(sub_001DA6F0) {
    bool trace = diag::GpuTrace() && TraceWorldMaterial("add-instance", ctx.ecx, kernel::R32(ctx.esp + 4), true);
    __imp__sub_001DA6F0(ctx, base);
    if (trace) TraceWorldResult("add-instance", ctx.eax);
}

// sub_001147D6(HANDLE h) -> cluster size in bytes. XAPI's volume cluster-size helper: it turns the handle into a
// FILE_OBJECT (ObReferenceObjectByHandle), calls the kernel-mode IoQueryVolumeInformation (ordinal 76, class 5,
// the file system name) and then reads the cluster size out of the volume's device extension ("FATX": [ext+0x1C],
// "GDFX": [ext]). The runtime has neither IoQueryVolumeInformation nor a device extension behind its file objects,
// so the helper traps at boot. Return the FATX cluster size the runtime's NtQueryVolumeInformationFile already
// reports for size queries: 32 sectors of 512 bytes = 16 KiB.
X86_FUNC(sub_001147D6) {
    (void)base;
    kernel::Return(ctx, 16384, 1);  // stdcall, one argument (ret 4)
}

// The runtime's DirectSound objects belong to the host, so an export of the library that it does not replace runs
// console code on an object with another layout and reads null ([this+0xC] is 0). These two are not covered by the
// runtime's DSOUND layer and the title calls them on its voices. Both are stdcall, two arguments (ret 8), S_OK.

// 0x3F7F9E, .XTLID 0xB0067 (between SetCurrentPosition and SetFilter): IDirectSoundBuffer_SetEG(this, DSENVELOPEDESC*).
// The inner function copies the envelope's mode, delay, attack, hold, decay, release and sustain into the voice's
// registers. The host mixer has no envelope generator, so the call is accepted and ignored.
X86_FUNC(sub_003F7F9E) {
    (void)base;
    kernel::Return(ctx, 0, 2);
}

// 0x3F8052, .XTLID 0xB0057 (between Lock and Play): IDirectSoundBuffer_Pause(this, dwPause). Accepted and ignored, so a
// paused sound keeps playing until the title stops it.
X86_FUNC(sub_003F8052) {
    (void)base;
    kernel::Return(ctx, 0, 2);
}

// 0x39D5F0: D3DDevice_RunPushBuffer(pushbuffer, fixup), stdcall, ret 8.
// The original XDK implementation requires GPU device internals. Submit the recorded
// geometry through the host API with the title's currently bound material and streams.
X86_FUNC(sub_0039D5F0) {
    (void)base;
    uint32_t pb = kernel::R32(ctx.esp + 4), fx = kernel::R32(ctx.esp + 8);
    d3d::RunPushBuffer(pb, fx);
    kernel::Return(ctx, 0, 2);
}

// Main scene: objects submitted, dynamic shadows and the deferred effect list still pending.
// This is a mid-function callback: leave every guest register/flag/stack slot untouched.
X86_HOST_API void MarvelAmbientOcclusion(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!(ctx.ebx & 4)) d3d::AmbientOcclusion();
}

// The supported XBE creates its named 256x256 ShadowBuffer in this initializer.
// Register its actual D3D texture header, not a session-specific data address.
X86_FUNC(__imp__sub_00020F20);
X86_FUNC(sub_00020F20) {
    d3d::RegisterShadowBuffer(0);
    __imp__sub_00020F20(ctx, base);
    const uint32_t owner = kernel::R32(0x004A38A8);
    if (owner && kernel::R32(owner) == 0x0043F3CC)
        d3d::RegisterShadowBuffer(kernel::R32(owner + 0x0C));
    else LOGR("shadows: expected ShadowBuffer owner not found; following original target scale");
}

// Scene shutdown releases ShadowBuffer. Forget its role before guest memory is reused.
X86_FUNC(__imp__sub_00020860);
X86_FUNC(sub_00020860) {
    d3d::RegisterShadowBuffer(0);
    __imp__sub_00020860(ctx, base);
}
