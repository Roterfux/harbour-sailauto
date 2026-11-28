// Android Auto Projection protocol constants.
// Values cross-checked against aasdk (f1xpl/aasdk, aasdk_proto/*.proto),
// the library OpenAuto is built on.
#pragma once

#include <cstdint>

namespace aap {

// Channel ids as assigned by aasdk/OpenAuto. A real car may use others; we
// always take the ids from the ServiceDiscoveryResponse.
enum Channel : uint8_t {
    ChControl = 0,
    ChInput = 1,
    ChSensor = 2,
    ChVideo = 3,
    ChMediaAudio = 4,
    ChSpeechAudio = 5,
    ChSystemAudio = 6,
    ChAvInput = 7,
    ChBluetooth = 8,
};

// Frame header byte 1
enum FrameFlags : uint8_t {
    FrameMiddle = 0,
    FrameFirst = 1 << 0,
    FrameLast = 1 << 1,
    FrameBulk = FrameFirst | FrameLast,
    FrameTypeMask = 0x03,
    MsgControl = 1 << 2, // "control" message on a service channel (channel open)
    Encrypted = 1 << 3,
};

constexpr int MaxFramePayload = 0x4000;

namespace ctrl {
enum : uint16_t {
    VersionRequest = 0x0001,
    VersionResponse = 0x0002,
    SslHandshake = 0x0003,
    AuthComplete = 0x0004,
    ServiceDiscoveryRequest = 0x0005,
    ServiceDiscoveryResponse = 0x0006,
    ChannelOpenRequest = 0x0007,
    ChannelOpenResponse = 0x0008,
    PingRequest = 0x000b,
    PingResponse = 0x000c,
    NavFocusRequest = 0x000d,
    NavFocusResponse = 0x000e,
    ShutdownRequest = 0x000f,
    ShutdownResponse = 0x0010,
    VoiceSessionRequest = 0x0011,
    AudioFocusRequest = 0x0012,
    AudioFocusResponse = 0x0013,
};
}

namespace av {
enum : uint16_t {
    MediaWithTimestamp = 0x0000,
    Media = 0x0001,
    SetupRequest = 0x8000,
    StartIndication = 0x8001,
    StopIndication = 0x8002,
    SetupResponse = 0x8003,
    MediaAck = 0x8004,
    InputOpenRequest = 0x8005,
    InputOpenResponse = 0x8006,
    VideoFocusRequest = 0x8007,
    VideoFocusIndication = 0x8008,
};
}

namespace input {
enum : uint16_t {
    EventIndication = 0x8001,
    BindingRequest = 0x8002,
    BindingResponse = 0x8003,
};
}

namespace sensor {
enum : uint16_t {
    StartRequest = 0x8001,
    StartResponse = 0x8002,
    EventIndication = 0x8003,
};
enum Type { Location = 1, Speed = 3, Gear = 8, NightData = 10, DrivingStatus = 13, Gyroscope = 20, GpsSatellites = 21 };
}

enum class TouchAction { Press = 0, Release = 1, Drag = 2, PointerDown = 5, PointerUp = 6 };

// VideoResolution enum -> pixels
inline void resolutionSize(int res, int &w, int &h)
{
    switch (res) {
    case 2: w = 1280; h = 720; break;
    case 3: w = 1920; h = 1080; break;
    default: w = 800; h = 480; break;
    }
}

} // namespace aap
