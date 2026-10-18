#include "video_decoder.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <new>
#include <atomic>
#include <sys/stat.h>

#if defined(__ORBIS__) || defined(__PS4__)
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#endif

// OpenOrbis's bundled Videodec.h currently contains untyped placeholders.
// These ABI structures match the OpenOrbis Videodec definitions added by
// Backporter's "add VideoDecX structures" change (4d2ed72).
#if defined(__ORBIS__) || defined(__PS4__)
namespace {
struct VideoDecCtrl { size_t size; uintptr_t handle; uint64_t version; };
struct VideoDecConfig {
    size_t size; uint32_t codecType, profile, maxLevel;
    int32_t maxWidth, maxHeight, maxDpbFrames; uint64_t flags;
};
struct VideoDecResources {
    size_t size, cpuSize; void* cpuMemory; size_t sharedSize; void* sharedMemory;
    size_t maxFrameSize; uint32_t frameAlignment;
};
struct VideoDecInput {
    size_t size; void* accessUnit; size_t accessUnitSize;
    uint64_t pts, dts, attached;
};
struct VideoDecAvcInfo {
    uint32_t numUnitsInTick, timeScale; uint8_t fixedFrameRate, aspectRatio;
    uint16_t sarWidth, sarHeight; uint8_t primaries, transfer, matrix, fullRange;
    uint32_t cropLeft, cropRight, cropTop, cropBottom;
};
union VideoDecCodecInfo { uint8_t reserved[64]; VideoDecAvcInfo avc; };
struct VideoDecPicture {
    size_t size; uint32_t valid, codecType, width, pitch, height, errorPicture;
    uint64_t pts, attached; VideoDecCodecInfo codec;
};
struct VideoDecFrameBuffer { size_t size; void* data; size_t bytes; };

extern "C" int32_t sceVideodecQueryResourceInfo(const VideoDecConfig*, VideoDecResources*);
extern "C" int32_t sceVideodecCreateDecoder(const VideoDecConfig*, const VideoDecResources*, VideoDecCtrl*);
extern "C" int32_t sceVideodecDecode(VideoDecCtrl*, const VideoDecInput*, VideoDecFrameBuffer*, VideoDecPicture*);
extern "C" int32_t sceVideodecDeleteDecoder(VideoDecCtrl*);

void saveDecoderStage(const char* stage) {
    mkdir("/data/ps4-xcloud", 0777);
    FILE* file = std::fopen("/data/ps4-xcloud/last_stream_stage.txt", "w");
    if (!file) return;
    std::fprintf(file, "%s\n", stage);
    std::fclose(file);
}
}
#endif

namespace xcloud {

PS4AvcDecoder::PS4AvcDecoder() {}

PS4AvcDecoder::~PS4AvcDecoder() {
    shutdown();
}

bool PS4AvcDecoder::init(uint32_t width, uint32_t height) {
#if defined(__ORBIS__) || defined(__PS4__)
    shutdown();
    saveDecoderStage("PS4 AVC startup: validating decoder dimensions");
    if (width == 0 || height == 0 || width > 3840 || height > 2160) {
        m_lastError = "Invalid decoder dimensions.";
        return false;
    }

    saveDecoderStage("PS4 AVC startup: loading legacy Videodec module");
    int rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_VIDEODEC);
    if (rc < 0) {
        char message[128];
        snprintf(message, sizeof(message), "SceVideodec module load failed (0x%08X).", rc);
        m_lastError = message;
        fprintf(stderr, "[Decoder] %s\n", message);
        return false;
    }
    m_videoDecoderModuleLoaded = true;
    saveDecoderStage("PS4 AVC startup: legacy Videodec module loaded");

    VideoDecConfig config{};
    config.size = sizeof(config);
    config.codecType = 0; // AVC/H.264
    config.profile = 100; // High profile
    config.maxLevel = 51;
    config.maxWidth = static_cast<int32_t>(width);
    config.maxHeight = static_cast<int32_t>(height);
    config.maxDpbFrames = 16;
    config.flags = 0x5A0000ULL; // disable one-frame decoder delay

    VideoDecResources resources{};
    resources.size = sizeof(resources);
    saveDecoderStage("PS4 AVC startup: querying legacy decoder memory requirements");
    rc = sceVideodecQueryResourceInfo(&config, &resources);
    if (rc < 0 || resources.maxFrameSize == 0) {
        char message[160];
        snprintf(message, sizeof(message), "AVC resource query failed (0x%08X, frame buffer %zu).",
                 rc, resources.maxFrameSize);
        m_lastError = message;
        fprintf(stderr, "[Decoder] %s\n", message);
        shutdown();
        return false;
    }

    if (resources.cpuSize > 0) {
        saveDecoderStage("PS4 AVC startup: allocating decoder CPU memory");
        const size_t alignment = 0x1000;
        if (posix_memalign(&m_decoderMemory, alignment,
                           (resources.cpuSize + alignment - 1) & ~(alignment - 1)) != 0) {
            m_lastError = "Could not allocate AVC CPU memory.";
            shutdown();
            return false;
        }
        m_memorySize = resources.cpuSize;
        resources.cpuMemory = m_decoderMemory;
    }

    if (resources.sharedSize > 0) {
        saveDecoderStage("PS4 AVC startup: allocating decoder shared direct memory");
        const size_t alignment = 0x200000;
        m_sharedMemorySize = (resources.sharedSize + alignment - 1) & ~(alignment - 1);
        rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), m_sharedMemorySize,
                                           alignment, 3, &m_sharedMemoryOffset);
        const int mapRc = rc < 0 ? rc : sceKernelMapDirectMemory(&m_sharedMemory, m_sharedMemorySize,
                0x33, 0, m_sharedMemoryOffset, alignment);
        if (rc < 0 || mapRc < 0) {
            char message[160];
            snprintf(message, sizeof(message), "AVC shared memory setup failed (alloc 0x%08X, map 0x%08X).",
                     rc, mapRc);
            m_lastError = message;
            fprintf(stderr, "[Decoder] %s\n", message);
            shutdown();
            return false;
        }
        resources.sharedMemory = m_sharedMemory;
        saveDecoderStage("PS4 AVC startup: shared direct memory mapped");
    }

    // Direct memory allocations require the PS4's 2 MiB granularity. The
    // decoder's own frame alignment is returned separately in resources.
    constexpr size_t directMemoryAlignment = 0x200000;
    m_frameBufferSize = (resources.maxFrameSize + directMemoryAlignment - 1) /
        directMemoryAlignment * directMemoryAlignment;
    if (m_frameBufferSize == 0) {
        m_lastError = "AVC frame buffer size was zero.";
        shutdown();
        return false;
    }
    saveDecoderStage("PS4 AVC startup: allocating frame direct memory");
    rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), m_frameBufferSize,
                                       directMemoryAlignment, 3, &m_frameBufferOffset);
    const int frameMapRc = rc < 0 ? rc : sceKernelMapDirectMemory(&m_frameBuffer, m_frameBufferSize,
            0x33, 0, m_frameBufferOffset, directMemoryAlignment);
    if (rc < 0 || frameMapRc < 0) {
        char message[160];
        snprintf(message, sizeof(message), "AVC frame buffer setup failed (alloc 0x%08X, map 0x%08X).",
                 rc, frameMapRc);
        m_lastError = message;
        fprintf(stderr, "[Decoder] %s\n", message);
        shutdown();
        return false;
    }
    saveDecoderStage("PS4 AVC startup: frame direct memory mapped");
    auto* control = new (std::nothrow) VideoDecCtrl{};
    if (!control) { m_lastError = "Could not allocate AVC decoder control block."; shutdown(); return false; }
    control->size = sizeof(*control);
    saveDecoderStage("PS4 AVC startup: creating legacy AVC decoder");
    rc = sceVideodecCreateDecoder(&config, &resources, control);
    if (rc < 0) {
        char message[128];
        snprintf(message, sizeof(message), "AVC decoder creation failed (0x%08X).", rc);
        m_lastError = message;
        fprintf(stderr, "[Decoder] %s\n", message);
        delete control;
        shutdown();
        return false;
    }
    m_decoderHandle = control;
    m_maxWidth = width;
    m_maxHeight = height;
    m_initialized = true;
    m_lastError.clear();
    saveDecoderStage("PS4 AVC startup: legacy AVC decoder ready");
    fprintf(stderr, "[Decoder] PS4 hardware AVC decoder ready (%ux%u).\n", width, height);
    return true;
#else
    (void)width;
    (void)height;
    m_initialized = false;
    m_lastError = "PS4 hardware AVC decoding is available only on-console.";
    return false;
#endif
}

bool PS4AvcDecoder::decodeFrame(const uint8_t* h264Data, size_t size, FrameInfo& outFrame) {
    outFrame = {};
#if defined(__ORBIS__) || defined(__PS4__)
    if (!m_initialized || !h264Data || size == 0 || size > 8 * 1024 * 1024 || !m_frameBuffer) return false;
    auto* control = static_cast<VideoDecCtrl*>(m_decoderHandle);
    VideoDecInput input{};
    input.size = sizeof(input);
    input.accessUnit = const_cast<uint8_t*>(h264Data);
    input.accessUnitSize = size;
    input.pts = 0;
    input.dts = 0;
    VideoDecFrameBuffer frameBuffer{sizeof(VideoDecFrameBuffer), m_frameBuffer, m_frameBufferSize};
    VideoDecPicture picture{};
    picture.size = sizeof(picture);
    static std::atomic<bool> firstDecodeLogged{false};
    if (!firstDecodeLogged.exchange(true)) saveDecoderStage("PS4 AVC: entering first frame decode");
    const int rc = sceVideodecDecode(control, &input, &frameBuffer, &picture);
    static std::atomic<bool> firstDecodeReturned{false};
    if (rc >= 0 && !firstDecodeReturned.exchange(true)) saveDecoderStage("PS4 AVC: first frame decode returned");
    if (rc < 0 || !picture.valid || picture.errorPicture || picture.width == 0 || picture.height == 0 ||
        picture.width > m_maxWidth || picture.height > m_maxHeight || picture.pitch < picture.width ||
        static_cast<size_t>(picture.pitch) * picture.height * 3 / 2 > m_frameBufferSize) return false;
    outFrame.width = picture.width;
    outFrame.height = picture.height;
    outFrame.pitchY = picture.pitch;
    outFrame.pitchUV = picture.pitch;
    outFrame.planeY = static_cast<const uint8_t*>(m_frameBuffer);
    outFrame.planeUV = outFrame.planeY + static_cast<size_t>(picture.pitch) * picture.height;
    return true;
#else
    (void)h264Data;
    (void)size;
    return false;
#endif
}

void PS4AvcDecoder::shutdown() {
#if defined(__ORBIS__) || defined(__PS4__)
    if (m_decoderHandle) {
        sceVideodecDeleteDecoder(static_cast<VideoDecCtrl*>(m_decoderHandle));
        delete static_cast<VideoDecCtrl*>(m_decoderHandle);
    }
    if (m_frameBufferOffset >= 0 && m_frameBufferSize > 0) {
        sceKernelReleaseDirectMemory(static_cast<off_t>(m_frameBufferOffset), m_frameBufferSize);
    }
    if (m_sharedMemoryOffset >= 0 && m_sharedMemorySize > 0) {
        sceKernelReleaseDirectMemory(static_cast<off_t>(m_sharedMemoryOffset), m_sharedMemorySize);
    }
    if (m_videoDecoderModuleLoaded) {
        sceSysmoduleUnloadModule(ORBIS_SYSMODULE_VIDEODEC);
    }
#endif
    std::free(m_decoderMemory);
    m_initialized = false;
    m_decoderHandle = nullptr;
    m_decoderMemory = nullptr;
    m_memorySize = 0;
    m_sharedMemory = nullptr;
    m_sharedMemorySize = 0;
    m_frameBuffer = nullptr;
    m_frameBufferSize = 0;
    m_sharedMemoryOffset = -1;
    m_frameBufferOffset = -1;
    m_maxWidth = 0;
    m_maxHeight = 0;
    m_videoDecoderModuleLoaded = false;
}

} // namespace xcloud
