#pragma once 

#include <stdint.h>
#include <stddef.h>

// ====================================================================
// === AUDIO DAC Device (/dev/dac0) - Speaker Output ===
// ====================================================================

// IOCTL Commands for /dev/dac0
#define AUDIO_IOCTL_RESET         0x10    // Reset hardware buffer pointers
#define AUDIO_IOCTL_SET_CONFIG    0x11    // Apply sample rate, channels, bit depth
#define AUDIO_IOCTL_GET_STATUS    0x12    // Get write position / buffer health
#define AUDIO_IOCTL_FLUSH         0x13    // Block until buffer drains

// Legacy DAC IOCTL aliases (for backward compatibility)
#define DAC_IOCTL_SET_CONFIG      AUDIO_IOCTL_SET_CONFIG
#define DAC_IOCTL_GET_CONFIG      AUDIO_IOCTL_GET_STATUS
#define DAC_IOCTL_RESET           AUDIO_IOCTL_RESET
#define DAC_IOCTL_FLUSH           AUDIO_IOCTL_FLUSH

// ====================================================================
// === AUDIO ADC Device (/dev/adc0) - Microphone Input ===
// ====================================================================

// IOCTL Commands for /dev/adc0 (same base as DAC for consistency)
#define ADC_IOCTL_RESET           AUDIO_IOCTL_RESET
#define ADC_IOCTL_SET_CONFIG      AUDIO_IOCTL_SET_CONFIG
#define ADC_IOCTL_GET_STATUS      AUDIO_IOCTL_GET_STATUS
#define ADC_IOCTL_FLUSH           AUDIO_IOCTL_FLUSH

// ADC-specific IOCTL commands
#define ADC_IOCTL_SET_GAIN        0x14    // Set microphone gain (0-7)
#define ADC_IOCTL_GET_GAIN        0x15    // Get current microphone gain
#define ADC_IOCTL_START_CAPTURE   0x16    // Start audio capture
#define ADC_IOCTL_STOP_CAPTURE    0x17    // Stop audio capture

// Microphone Gain Levels (ES8311 codec: 0dB to 42dB in 6dB steps)
#define MIC_GAIN_0DB              0
#define MIC_GAIN_6DB              1
#define MIC_GAIN_12DB             2
#define MIC_GAIN_18DB             3
#define MIC_GAIN_24DB             4
#define MIC_GAIN_30DB             5
#define MIC_GAIN_36DB             6
#define MIC_GAIN_42DB             7
#define MIC_GAIN_MAX              MIC_GAIN_42DB
#define MIC_GAIN_DEFAULT          MIC_GAIN_18DB

// ====================================================================
// === Shared Audio Structures (DAC and ADC) ===
// ====================================================================

// Configuration Structure (used by both DAC and ADC)
struct pcm_config {
    uint32_t sample_rate;       // e.g. 44100, 16000, 48000
    uint8_t  channels;          // 1 (mono) or 2 (stereo)
    uint8_t  bits_per_sample;   // usually 16
    uint8_t  flags;             // Reserved flags
    uint8_t  padding;           // alignment
};

// Status Structure (used by both DAC and ADC)
struct pcm_status {
    uint32_t buffer_size;       // Total hardware buffer size in bytes
    uint32_t buffer_free;       // Bytes available to write (DAC) or read (ADC)
    uint32_t sample_rate;       // Current rate
};

// ADC-specific extended status
struct adc_status {
    uint32_t buffer_size;       // Total buffer size in bytes
    uint32_t samples_available; // Samples ready to read
    uint32_t sample_rate;       // Current sample rate
    uint8_t  gain;              // Current gain setting (0-7)
    uint8_t  is_recording;      // 1 if recording, 0 if stopped
    uint8_t  overflow;          // 1 if buffer overflow occurred
    uint8_t  reserved;
};

// ====================================================================
// === MEDIA Device (/dev/media0) - Combined Audio+Video Streaming ===
// ====================================================================

// IOCTL Commands for /dev/media0
#define MEDIA_IOCTL_RESET         0x20    // Reset all buffers
#define MEDIA_IOCTL_SET_CONFIG    0x21    // Configure media settings
#define MEDIA_IOCTL_GET_STATUS    0x22    // Get buffer status
#define MEDIA_IOCTL_FLUSH         0x23    // Flush pending data
#define MEDIA_IOCTL_GET_PACKET    0x24    // Read next packet header info
#define MEDIA_IOCTL_GET_CONFIG    0x25    // Get current media configuration

// Media Packet Flags (used in packet headers)
#define MEDIA_FLAG_HAS_VIDEO      0x01
#define MEDIA_FLAG_HAS_AUDIO      0x02
#define MEDIA_FLAG_AUDIO_MASK     0x30
#define MEDIA_FLAG_AUDIO_PCM      0x00    // Audio format: Raw PCM
#define MEDIA_FLAG_AUDIO_MP3      0x10    // Audio format: MP3 compressed
#define MEDIA_FLAG_AUDIO_NYU      0x20    // Audio format: NYU compressed
#define MEDIA_FLAG_AUDIO_AAC      0x30    // Audio format: AAC compressed (LC from MP4)
#define MEDIA_FLAG_VIDEO_RGB565   0x00    // Video format: RGB565
#define MEDIA_FLAG_VIDEO_JPEG     0x40    // Video format: JPEG compressed

// Media Packet Header (written before each data chunk)
struct media_packet_header {
    uint32_t magic;             // 0x4D504B54 "MPKT"
    uint32_t flags;             // MEDIA_FLAG_* combination
    uint32_t audio_size;        // Size of audio data following header
    uint32_t video_size;        // Size of video data following header
    uint32_t audio_sample_rate; // Sample rate if audio present
    uint8_t  audio_channels;    // Channel count if audio present
    uint8_t  reserved[3];       // Padding
};

#define MEDIA_PACKET_MAGIC 0x4D504B54

// Media Configuration Structure
struct media_config {
    // Video settings
    uint16_t video_width;
    uint16_t video_height;
    uint8_t  video_format;      // MEDIA_FLAG_VIDEO_* value
    
    // Audio settings
    uint32_t audio_sample_rate;
    uint8_t  audio_channels;
    uint8_t  audio_format;      // MEDIA_FLAG_AUDIO_* value
    uint8_t  padding[2];
};

// Media Status Structure
struct media_status {
    uint32_t total_buffer_size;     // Total gram size
    uint32_t audio_buffer_used;     // Bytes of audio data in buffer
    uint32_t video_buffer_used;     // Bytes of video data in buffer
    uint32_t packets_pending;       // Number of complete packets ready to read
    uint32_t flags;                 // Current configuration flags
};

// ====================================================================
// === GPU Offload Device (/dev/gpu0) ===
// ====================================================================

#define GPU_IOCTL_GET_INFO       0x40
#define GPU_IOCTL_SUBMIT_NOOP    0x41
#define GPU_IOCTL_COMPUTE_RGBA8_TO_RGB565 0x42
#define GPU_IOCTL_COMPUTE_RGBA8_COPY 0x43
/* Textured, nearest-sampled, arbitrarily rescaled RGBA8 -> RGBA8 rectangle
 * blit. Unlike the two compute jobs above -- which are linear runs of pixels --
 * this one is two-dimensional and takes independent source and destination
 * extents, which is what lets a software rasteriser hand a whole sprite to the
 * GPU instead of stepping it a texel at a time. Offered only when the device
 * reports GPU_CAP_SCALED_BLIT. */
#define GPU_IOCTL_BLIT_RGBA8_SCALED 0x44
/* Indexed list of pre-transformed, textured triangles. This is the seam a
 * software rasteriser hands its primitives over at: everything above it --
 * vertex shading, clipping, the vertex cache -- has already run on the CPU, and
 * what arrives is screen-space positions with texel coordinates. Offered only
 * when the device reports GPU_CAP_TRIANGLE_LIST, and the caller must keep the
 * rasteriser it would otherwise have run, because every job may be refused. */
#define GPU_IOCTL_DRAW_TRIANGLES 0x45

#define GPU_ACCELERATOR_ABI_VERSION 1

#define GPU_CAP_PRESENT          0x00000001u
#define GPU_CAP_NVIDIA           0x00000002u
#define GPU_CAP_BAR0_MAPPED      0x00000004u
#define GPU_CAP_BUS_MASTER       0x00000008u
#define GPU_CAP_COMPUTE_ABI      0x00000010u
#define GPU_CAP_SUBMIT_NOOP      0x00000020u
#define GPU_CAP_INTEL            0x00000040u
#define GPU_CAP_COMPUTE_RGBA8_TO_RGB565 0x00000080u
#define GPU_CAP_CPU_VECTOR_FALLBACK 0x00000100u
#define GPU_CAP_COMPUTE_RGBA8_COPY 0x00000200u
#define GPU_CAP_MEDIATEK         0x00000400u
#define GPU_CAP_ZERO_COPY_RGBA8  0x00000800u
#define GPU_CAP_SHADER_PIPELINE   0x00001000u
#define GPU_CAP_SCALED_BLIT       0x00002000u
/* An indexed triangle list of pre-transformed vertices can be drawn. */
#define GPU_CAP_TRIANGLE_LIST     0x00004000u
/* ...and pixels of the target rectangle that no triangle covers come back
 * unchanged. Without this bit the caller must be redrawing the whole rectangle,
 * because the tile buffer is initialised by a clear before the batch runs. */
#define GPU_CAP_TRIANGLE_PRESERVE 0x00008000u

#ifndef MVII_GPU_DEVICE_INFO_DEFINED
#define MVII_GPU_DEVICE_INFO_DEFINED
struct gpu_device_info {
    uint32_t abi_version;
    uint32_t present;
    uint32_t vendor;
    uint32_t device;
    uint32_t bdf;
    uint32_t class_code;
    uint32_t subclass;
    uint32_t prog_if;
    uint32_t revision;
    uint32_t bar0;
    uint32_t caps;
    uint32_t queue_ready;
    char driver_name[32];
    char status[96];
};
#endif

#ifndef MVII_GPU_RGBA8_TO_RGB565_JOB_DEFINED
#define MVII_GPU_RGBA8_TO_RGB565_JOB_DEFINED
struct gpu_rgba8_to_rgb565_job {
    const uint8_t* src_rgba8;
    uint16_t* dst_rgb565;
    uint32_t pixel_count;
    uint32_t flags;
};
#endif

#ifndef MVII_GPU_RGBA8_COPY_JOB_DEFINED
#define MVII_GPU_RGBA8_COPY_JOB_DEFINED
struct gpu_rgba8_copy_job {
    const uint8_t* src_rgba8;
    uint8_t* dst_rgba8;
    uint32_t pixel_count;
    uint32_t flags;
};
#endif

#ifndef MVII_GPU_RGBA8_BLIT_JOB_DEFINED
#define MVII_GPU_RGBA8_BLIT_JOB_DEFINED
/* Source and destination are both tightly-describable RGBA8 surfaces addressed
 * by a byte pitch, so the destination may be a sub-rectangle of a larger
 * framebuffer: point dst_rgba8 at its first pixel and leave dst_pitch_bytes at
 * the surface's full pitch. The driver enforces its own alignment rules and
 * returns -1/ENOTSUP when it cannot take the job, so every caller must keep the
 * CPU path it would otherwise have run. */
struct gpu_rgba8_blit_job {
    const uint8_t* src_rgba8;
    uint8_t* dst_rgba8;
    uint32_t src_width;
    uint32_t src_height;
    uint32_t src_pitch_bytes;
    uint32_t dst_width;
    uint32_t dst_height;
    uint32_t dst_pitch_bytes;
    uint32_t flags;
};
#endif

#ifndef MVII_GPU_TRIANGLE_JOB_DEFINED
#define MVII_GPU_TRIANGLE_JOB_DEFINED
/* Pre-transformed screen-space vertex. Positions are destination pixels and
 * texture coordinates are texels rather than the normalised range, which is
 * what a software rasteriser's varyings already hold and what saves a
 * normalise/denormalise round trip on the way in. */
struct gpu_triangle_vertex {
    float x;
    float y;
    float u;
    float v;
};

/* Uncovered pixels of the destination rectangle may be lost. Set this when the
 * caller is redrawing the whole rectangle anyway; leave it clear to require
 * that they survive, which a device without GPU_CAP_TRIANGLE_PRESERVE will
 * refuse rather than silently break. */
#define GPU_TRIANGLE_FLAG_OVERWRITE_TARGET 0x00000001u

struct gpu_triangle_job {
    const struct gpu_triangle_vertex* vertices;
    uint32_t                          vertex_count;
    const uint16_t*                   indices;
    uint32_t                          index_count;
    const uint8_t*                    texture_rgba8;
    uint32_t                          texture_width;
    uint32_t                          texture_height;
    uint32_t                          texture_pitch_bytes;
    void*                             dst;
    uint32_t                          dst_width;
    uint32_t                          dst_height;
    uint32_t                          dst_pitch_bytes;
    /* Zero selects RGB565 and three RGBA8, matching the writeback pixel-format
     * codes the rest of this API already passes around. */
    uint32_t dst_format;
    /* Destination rectangle in pixels, half-open. Nothing outside it is read or
     * written. */
    uint32_t scissor_min_x;
    uint32_t scissor_min_y;
    uint32_t scissor_max_x;
    uint32_t scissor_max_y;
    uint32_t flags;
};
#endif

// ====================================================================
// === Thermal Device (/dev/thermal0) ===
// ====================================================================

#define THERMAL_IOCTL_GET_STATUS 0x50
#define THERMAL_IOCTL_SET_POLICY 0x51

#define THERMAL_DEVICE_ABI_VERSION 1

#define THERMAL_MODE_STANDARD    0u
#define THERMAL_MODE_PERFORMANCE 1u

#define THERMAL_CAP_FAN_CONTROL        0x00000001u
#define THERMAL_CAP_CPU_TEMP           0x00000002u
#define THERMAL_CAP_GPU_TEMP           0x00000004u
#define THERMAL_CAP_PERFORMANCE_POLICY 0x00000008u
#define THERMAL_CAP_ESTIMATED          0x00000010u

struct thermal_device_status {
    uint32_t abi_version;
    uint32_t cpu_avg_c_milli;
    uint32_t gpu_avg_c_milli;
    uint32_t hottest_c_milli;
    uint32_t fan_percent;
    uint32_t mode;
    uint32_t sensor_count_cpu;
    uint32_t sensor_count_gpu;
    uint32_t caps;
    char status[96];
};

struct thermal_policy {
    uint32_t mode;
    uint32_t fan_percent;
    uint32_t flags;
};

// ====================================================================
// === Network Device (/dev/net0) ===
// ====================================================================

#define NET_IOCTL_GET_INFO       0x60

#define NET_DEVICE_ABI_VERSION   1
#define NET_CAP_RAW_ETHERNET     0x00000001u
#define NET_CAP_BSD_SOCKETS      0x00000002u

struct net_device_info {
    uint32_t abi_version;
    uint32_t link_up;
    uint32_t ipv4_addr;
    uint32_t mtu;
    uint32_t caps;
    char driver_name[32];
    char status[96];
};

// ====================================================================
// === Buffer Size Constants ===
// ====================================================================

// DAC (Speaker Output) buffer
#define DAC_GRAM_SIZE               48000   // Max audio samples in DAC buffer

// ADC (Microphone Input) buffer
#define ADC_GRAM_SIZE               48000   // Max audio samples in ADC buffer
#define ADC_GRAM_SIZE_BYTES         (ADC_GRAM_SIZE * sizeof(int16_t))
#define ADC_DEFAULT_SAMPLE_RATE     16000   // Default recording sample rate
#define ADC_MAX_SAMPLE_RATE         48000   // Maximum recording sample rate
#define ADC_MIN_SAMPLE_RATE         8000    // Minimum recording sample rate

// Media buffer sizes
#define MEDIA_AUDIO_SIZE    (48000 * 4)         // Audio portion of media buffer (bytes)
#define MEDIA_VIDEO_SIZE    (320 * 240 * 2)     // Video portion (RGB565)
#define MEDIA_GRAM_SIZE     (MEDIA_AUDIO_SIZE + MEDIA_VIDEO_SIZE + 4096) // Total + headers

// ====================================================================
// === Framebuffer Device (/dev/fb0) ===
// ====================================================================

#define FB_IOCTL_CLEAR           0x2    
#define FB_IOCTL_SWAP_BUFFER     0x3    
#define FB_IOCTL_VFLIP           0x4    
#define FB_IOCTL_HFLIP           0x5    
#define FB_IOCTL_GET_BUFFER      0x6    
#define FB_IOCTL_PRESENT         0x7    
#define FB_IOCTL_SWAP_RGBA8      0x8    
#define FB_IOCTL_MAP_RGBA8       0x9

struct fb_draw {
    uint16_t x, y;      // Top-left position
    uint16_t w, h;      // Dimensions (e.g., 320x240)
    uint16_t *data;     // User pointer to RGB565 buffer (w*h uint16_t's)
};

struct fb_draw_rgba8 {
    uint16_t x, y;       // Top-left position
    uint16_t w, h;       // Dimensions
    const uint8_t *data; // User pointer to RGBA8 buffer (w*h*4 bytes)
};

/* Shared-surface mapping for same-address-space Virtua runtimes. The caller
 * supplies the requested visible width/height; the kernel returns its RGBA8
 * backing store and pitch. Rendering directly into this surface removes the
 * full-frame application-to-kernel copy before FB_IOCTL_PRESENT. */
struct fb_map_rgba8 {
    uint16_t width;
    uint16_t height;
    uint16_t pitch_pixels;
    uint16_t flags;
    uint8_t *data;
};


enum TouchType
{
    TOUCH_NONE = 0,
    TOUCH_BEGIN,
    TOUCH_MOVE,
    TOUCH_END,

    // Keyboard events multiplexed into the same /dev/input0 ring.
    // For KEY_DOWN: TouchEvent.x = Linux input keycode, TouchEvent.y = 1
    //               (initial press) or > 1 (auto-repeat).
    // For KEY_UP:   TouchEvent.x = Linux input keycode, TouchEvent.y = 0.
    KEY_DOWN = 100,
    KEY_UP   = 101
};

struct TouchEvent {
    uint16_t x;
    uint16_t y;
    TouchType type;
} __attribute__((packed, aligned(4)));
