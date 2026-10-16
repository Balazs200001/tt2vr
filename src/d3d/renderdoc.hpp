#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace d3d::renderdoc {

// True when the game was launched under RenderDoc (renderdoc.dll loaded) and its in-app API answered.
bool Available();

// Captures the next frame to <[capture] renderdoc_dir>/<name>_frame<N>.rdc through the in-application API.
// `onDone` gets "capture ok <path>" or a failure line once RenderDoc reports the capture. Present thread.
void Request(const std::string& name, std::function<void(const std::string&)> onDone = {});

// Present hook, once per frame after Present returns: completes pending requests.
void Tick(std::uint64_t frame);

// True while a capture is pending, so the overlay can hide itself from the captured frame.
bool Busy();

} // namespace d3d::renderdoc
