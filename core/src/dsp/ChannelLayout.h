#pragma once

namespace guitarfx
{
/// Whether a connection in a graph carries one signal on both channels or two. Decided when the
/// graph is built, from the input and the effect types along the path, never from the audio
/// (see SignalGraphExecutor, "Channel layout").
enum class ChannelLayout
{
    Mono,
    Stereo,
};
} // namespace guitarfx
