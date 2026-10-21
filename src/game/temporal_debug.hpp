// game::TemporalDebug (PLAN Phase 6f): per-eye history views and the VRAM of the per-eye history, for the overlay,
// and the [debug] freeze of eye R's TAA history.
//
// Sources, per camera index k (docs/re/temporal.md 2, 10): TAA history RenderLoop+0xce8+k*0x10, 'Last Frame Depth'
// +0x10e0+k*8, 'Last Frame Buffer' +0x10f0+k*8 (RenderTarget* -> Texture2DDX11+0x68), froxel volumes
// DeferredLighting+0x760+(pp*2+k)*8 (Texture3DDX11+0x68), previous luminance tm+0x138+k*8. The motion-vector RT
// (RenderLoop+0xca8) is one transient rewritten by every view, so each eye's vectors are copied (CopyResource, on the
// recording context) right before that view's 'TAA resolve' draw, which is found by its history SRV (t2) like the leak
// test; the draw also gets t3 = the motion vectors. Views are drawn into small RGBA8 thumbnails at Present
// (d3d::Blitter::Visualize): TAA history raw, motion vectors as colour, last-frame depth on a log scale.
// Freeze ([debug], default off, never the fix for anything): eye R's TAA history is copied once, then restored from
// that copy before every view-1 resolve, so eye R blends against a frozen image while eye L must stay unaffected (T2).
#pragma once

#include <d3d11.h>

#include <functional>
#include <string>

namespace game {

struct TemporalVram {
    bool valid = false;
    double taaMB[2] = {}; // TAA history pair per camera index
    double lastFrameMB[2] = {}; // last-frame depth + colour (SSR)
    double froxelMB[2] = {}; // two froxel volumes
    double luminanceMB[2] = {};
    double totalMB[2] = {};
    double debugMB = 0; // this module's own copies and thumbnails
};

enum class TemporalView { History, Motion, PreviousDepth };

// Thread: game main thread, DeferredRenderLoop::Render pre-hook (main render only).
void OnTemporalDebugLoopRender(void* renderLoop);
// Thread: game main thread (Present, before the overlay builds its frame). Draws the thumbnails while shown.
void UpdateTemporalViews(ID3D11Device* device, ID3D11DeviceContext* context);
// Thread: game main thread (overlay, remote commands).
void SetTemporalViewsShown(bool shown);
bool TemporalViewsShown();
void SetFreezeRightHistory(bool freeze);
bool FreezeRightHistory();
// Thumbnail of eye `eye` (0 = left), or nullptr; width / height of the thumbnails.
ID3D11ShaderResourceView* TemporalThumbnail(int eye, TemporalView view);
void TemporalThumbnailSize(unsigned& width, unsigned& height);
TemporalVram TemporalVramUsage();
// Remote "mvsave": with the views shown, reads back both eyes' motion vectors at the next Present, reports per eye the
// mean / max length in pixels and the share above 1 px, and writes screens\motion_vectors.png (left | right at half
// size; red / green = 128 + 16 x the x / y motion in pixels). Thread: game main thread.
void RequestMotionSave(std::function<void(const std::string&)> done);

} // namespace game
