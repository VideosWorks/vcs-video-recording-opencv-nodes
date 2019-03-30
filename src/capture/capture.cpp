/*
 * 2018 Tarpeeksi Hyvae Soft /
 * VCS capture
 *
 * Handles interactions with the capture hardware.
 *
 */

#include <QDebug>
#include <QFile>
#include <thread>
#include <mutex>
#include "../common/command_line.h"
#include "../common/propagate.h"
#include "../display/display.h"
#include "../common/globals.h"
#include "../main.h"
#include "../common/csv.h"
#include "capture.h"

// All local RGBEASY API callbacks lock this for their duration.
std::mutex INPUT_OUTPUT_MUTEX;

// Set to true if the current input resolution is an alias for another resolution.
static bool IS_ALIASED_INPUT_RESOLUTION = false;

// Set to true when receiving the first frame after 'no signal'.
static bool SIGNAL_WOKE_UP = false;

// If set to true, the scaler should skip the next frame we send.
static u32 SKIP_NEXT_NUM_FRAMES = false;

static std::vector<mode_params_s> KNOWN_MODES;

// The capture's current color format.
static PIXELFORMAT CAPTURE_PIXEL_FORMAT = RGB_PIXELFORMAT_888;
static u32 CAPTURE_COLOR_DEPTH = 32;

// Used to keep track of whether we have new frames to be processed (i.e. if the
// current count of captured frames doesn't equal the number of processed frames).
// Doesn't matter if these counters wrap around.
static std::atomic<unsigned int> CNT_FRAMES_PROCESSED(0);
static std::atomic<unsigned int> CNT_FRAMES_CAPTURED(0);

// The number of frames the capture card has sent which VCS was too busy to
// receive and had to skip. Call kc_reset_missed_frames_count() to reset it.
static std::atomic<unsigned int> CNT_FRAMES_SKIPPED(0);

// Whether the capture card is receiving a signal from its input.
static std::atomic<bool> RECEIVING_A_SIGNAL(true);

// Will be set to true by the capture card callback if the card experiences an
// unrecoverable error.
static bool UNRECOVERABLE_CAPTURE_ERROR = false;

// Set to true if the capture signal is invalid.
static std::atomic<bool> SIGNAL_IS_INVALID(false);

// Will be set to true when the input signal is lost, and back to false once the
// events processor has acknowledged the loss of signal.
static bool SIGNAL_WAS_LOST = false;

// Set to true if the capture card reports the current signal as invalid. Will be
// automatically set back to false once the events processor has acknowledged the
// invalidity of the signal.
static bool SIGNAL_BECAME_INVALID = false;

// Frames sent by the capture card will be stored here. Note that only one frame
// will fit at a time - if the capture card sends in a new frame before the
// previous one has been processed, the new frame will be ignored.
static captured_frame_s FRAME_BUFFER;

// Set to true if the capture card's input mode changes.
static bool RECEIVED_NEW_VIDEO_MODE = false;

// The maximum image depth that the capturer can handle.
static const u32 MAX_BIT_DEPTH = 32;

// Set to 1 if we've acquired access to the RGBEASY API.
static bool RGBEASY_IS_LOADED = 0;

static HRGB CAPTURE_HANDLE = 0;
static HRGBDLL RGBAPI_HANDLE = 0;

// Set to 1 if we're currently capturing.
static bool CAPTURE_IS_ACTIVE = false;

// Set to 1 if the input channel we were requested to use was invalid.
static bool INPUT_CHANNEL_IS_INVALID = 0;

// Aliases are resolutions that stand in for others; i.e. if 640 x 480 is an alias
// for 1024 x 768, VCS will ask the capture card to switch to 640 x 480 every time
// the card sets 1024 x 768.
static std::vector<mode_alias_s> ALIASES;

static capture_hardware_s CAPTURE_HARDWARE;

// Returns true if the given RGBEase API call return value indicates an error.
bool apicall_succeeds(long callReturnValue)
{
    if (callReturnValue != RGBERROR_NO_ERROR)
    {
        NBENE(("A call to the RGBEasy API returned with error code (0x%x).", callReturnValue));
        return false;
    }

    return true;
}

const capture_hardware_s& kc_hardware(void)
{
    return CAPTURE_HARDWARE;
}

void update_known_mode_params(const resolution_s r,
                              const input_color_settings_s *const c,
                              const input_video_settings_s *const v)
{
    uint idx;
    for (idx = 0; idx < KNOWN_MODES.size(); idx++)
    {
        if (KNOWN_MODES[idx].r.w == r.w &&
            KNOWN_MODES[idx].r.h == r.h)
        {
            goto mode_exists;
        }
    }

    // If the mode doesn't already exist, add it.
    KNOWN_MODES.push_back({r,
                           CAPTURE_HARDWARE.meta.default_color_settings(),
                           CAPTURE_HARDWARE.meta.default_video_settings()});

    kd_signal_new_known_mode(r);

    mode_exists:
    // Update the existing mode with the new parameters.
    if (c != nullptr) KNOWN_MODES[idx].color = *c;
    if (v != nullptr) KNOWN_MODES[idx].video = *v;

    return;
}

namespace rgbeasy_callbacks_n
{
#if !USE_RGBEASY_API
    void frame_captured(void){}
    void video_mode_changed(void){}
    void invalid_signal(void){}
    void no_signal(void){}
    void error(void){}
#else
    // Called by the capture card when a new frame has been captured. The
    // captured RGBA data is in frameData.
    //
    void RGBCBKAPI frame_captured(HWND, HRGB, LPBITMAPINFOHEADER frameInfo, void *frameData, ULONG_PTR)
    {
        if (CNT_FRAMES_CAPTURED != CNT_FRAMES_PROCESSED)
        {
            //ERRORI(("The capture card sent in a frame before VCS had finished processing "
            //        "the previous one. Skipping this new one. (Captured: %u, processed: %u.)",
            //        CNT_FRAMES_CAPTURED.load(), CNT_FRAMES_PROCESSED.load()));
            CNT_FRAMES_SKIPPED++;
            return;
        }

        std::lock_guard<std::mutex> lock(INPUT_OUTPUT_MUTEX);

        // Ignore new callback events if the user has signaled to quit the program.
        if (PROGRAM_EXIT_REQUESTED)
        {
            goto done;
        }

        // This could happen e.g. if direct DMA transfer is enabled.
        if (frameData == nullptr ||
            frameInfo == nullptr)
        {
            goto done;
        }

        if (FRAME_BUFFER.pixels.is_null())
        {
            //ERRORI(("The capture card sent in a frame but the frame buffer was uninitialized. "
            //        "Ignoring the frame."));
            goto done;
        }

        if (frameInfo->biBitCount > MAX_BIT_DEPTH)
        {
            //ERRORI(("The capture card sent in a frame that had an illegal bit depth (%u). "
            //        "The maximum allowed bit depth is %u.", frameInfo->biBitCount, MAX_BIT_DEPTH));
            goto done;
        }

        FRAME_BUFFER.r.w = frameInfo->biWidth;
        FRAME_BUFFER.r.h = abs(frameInfo->biHeight);
        FRAME_BUFFER.r.bpp = frameInfo->biBitCount;

        // Copy the frame's data into our local buffer so we can work on it.
        memcpy(FRAME_BUFFER.pixels.ptr(), (u8*)frameData,
               FRAME_BUFFER.pixels.up_to(FRAME_BUFFER.r.w * FRAME_BUFFER.r.h * (FRAME_BUFFER.r.bpp / 8)));

        done:
        CNT_FRAMES_CAPTURED++;
        return;
    }

    // Called by the capture card when the input video mode changes.
    //
    void RGBCBKAPI video_mode_changed(HWND, HRGB, PRGBMODECHANGEDINFO info, ULONG_PTR)
    {
        std::lock_guard<std::mutex> lock(INPUT_OUTPUT_MUTEX);

        // Ignore new callback events if the user has signaled to quit the program.
        if (PROGRAM_EXIT_REQUESTED)
        {
            goto done;
        }

        SIGNAL_WOKE_UP = !RECEIVING_A_SIGNAL;
        RECEIVED_NEW_VIDEO_MODE = true;
        SIGNAL_IS_INVALID = false;

        done:
        return;
    }

    // Called by the capture card when it's given a signal it can't handle.
    //
    void RGBCBKAPI invalid_signal(HWND, HRGB, unsigned long horClock, unsigned long verClock, ULONG_PTR captureHandle)
    {
        std::lock_guard<std::mutex> lock(INPUT_OUTPUT_MUTEX);

        // Ignore new callback events if the user has signaled to quit the program.
        if (PROGRAM_EXIT_REQUESTED)
        {
            goto done;
        }

        // Let the card apply its own no signal handler as well, just in case.
        RGBInvalidSignal(captureHandle, horClock, verClock);

        SIGNAL_IS_INVALID = true;

        done:
        return;
    }

    // Called by the capture card when no input signal is present.
    //
    void RGBCBKAPI no_signal(HWND, HRGB, ULONG_PTR captureHandle)
    {
        std::lock_guard<std::mutex> lock(INPUT_OUTPUT_MUTEX);

        // Let the card apply its own no signal handler as well, just in case.
        RGBNoSignal(captureHandle);

        SIGNAL_WAS_LOST = true;

        return;
    }

    void RGBCBKAPI error(HWND, HRGB, unsigned long error, ULONG_PTR, unsigned long*)
    {
        std::lock_guard<std::mutex> lock(INPUT_OUTPUT_MUTEX);

        UNRECOVERABLE_CAPTURE_ERROR = true;

        return;
    }
#endif
}

// Returns true if the capture card has been offering frames while the previous
// frame was still being processed for display.
//
bool kc_has_capturer_missed_frames(void)
{
    return bool(CNT_FRAMES_SKIPPED > 0);
}

uint kc_missed_input_frames_count(void)
{
    return CNT_FRAMES_SKIPPED;
}

void kc_reset_missed_frames_count(void)
{
    CNT_FRAMES_SKIPPED = 0;

    return;
}

// Creates a test pattern into the frame buffer.
//
void kc_insert_test_image(void)
{
    // For making the test pattern move with successive calls.
    static uint offset = 0;
    offset++;

    for (uint y = 0; y < FRAME_BUFFER.r.h; y++)
    {
        for (uint x = 0; x < FRAME_BUFFER.r.w; x++)
        {
            const uint idx = ((x + y * FRAME_BUFFER.r.w) * 4);
            FRAME_BUFFER.pixels[idx + 0] = (offset+x)%256;
            FRAME_BUFFER.pixels[idx + 1] = (offset+y)%256;
            FRAME_BUFFER.pixels[idx + 2] = 150;
            FRAME_BUFFER.pixels[idx + 3] = 255;
        }
    }

    return;
}

captured_frame_s& kc_latest_captured_frame(void)
{
    return FRAME_BUFFER;
}

void kc_initialize_capturer(void)
{
    INFO(("Initializing the capturer."));

    FRAME_BUFFER.pixels.alloc(MAX_FRAME_SIZE);

    #ifndef USE_RGBEASY_API
        FRAME_BUFFER.r = {640, 480, 32};

        INFO(("The RGBEASY API is disabled by code. Skipping capture initialization."));
        goto done;
    #endif

    // Open an input on the capture card, and have it start sending in frames.
    {
        if (!kc_initialize_capture_card() ||
            !kc_start_capture())
        {
            NBENE(("Failed to initialize capture."));

            PROGRAM_EXIT_REQUESTED = 1;
            goto done;
        }
    }

    // Load previously-saved settings, if any.
    {
        if (!kc_load_aliases(kcom_alias_file_name(), true))
        {
            NBENE(("Failed loading mode aliases from disk.\n"));
            PROGRAM_EXIT_REQUESTED = 1;
            goto done;
        }

        if (!kc_load_mode_params(kcom_params_file_name(), true))
        {
            NBENE(("Failed loading mode parameters from disk.\n"));
            PROGRAM_EXIT_REQUESTED = 1;
            goto done;
        }
    }

    done:
    kpropagate_new_input_video_mode();
    return;
}

bool kc_adjust_capture_vertical_offset(const int delta)
{
    if (!delta) return true;

    const long newPos = (CAPTURE_HARDWARE.status.video_settings().verPos + delta);
    if (newPos < std::max(2, (int)CAPTURE_HARDWARE.meta.minimum_video_settings().verPos) ||
        newPos > CAPTURE_HARDWARE.meta.maximum_video_settings().verPos)
    {
        // ^ Testing for < 2 along with < MIN_VIDEO_PARAMS.verPos, since on my
        // VisionRGB-PRO2, MIN_VIDEO_PARAMS.verPos gives a value less than 2,
        // but setting any such value corrupts the capture.
        return false;
    }

    if (apicall_succeeds(RGBSetVerPosition(CAPTURE_HANDLE, newPos)))
    {
        kd_update_gui_video_params();
    }

    return true;
}

bool kc_adjust_capture_horizontal_offset(const int delta)
{
    if (!delta) return true;

    const long newPos = (CAPTURE_HARDWARE.status.video_settings().horPos + delta);
    if (newPos < CAPTURE_HARDWARE.meta.minimum_video_settings().horPos ||
        newPos > CAPTURE_HARDWARE.meta.maximum_video_settings().horPos)
    {
        return false;
    }

    if (apicall_succeeds(RGBSetHorPosition(CAPTURE_HANDLE, newPos)))
    {
        kd_update_gui_video_params();
    }

    return true;
}

static bool shutdown_capture(void)
{
    if (!RGBEASY_IS_LOADED) return true;

    if (!apicall_succeeds(RGBCloseInput(CAPTURE_HANDLE)) ||
        !apicall_succeeds(RGBFree(RGBAPI_HANDLE)))
    {
        return false;
    }

    RGBEASY_IS_LOADED = false;
    return true;
}

bool stop_capture(void)
{
    INFO(("Stopping capture on input channel %d.", (INPUT_CHANNEL_IDX + 1)));

    if (CAPTURE_IS_ACTIVE)
    {
        if (!apicall_succeeds(RGBStopCapture(CAPTURE_HANDLE)))
        {
            NBENE(("Failed to stop capture on input channel %d.", (INPUT_CHANNEL_IDX + 1)));
            goto fail;
        }

        CAPTURE_IS_ACTIVE = false;
    }
    else
    {
        CAPTURE_IS_ACTIVE = false;

#ifdef USE_RGBEASY_API
        DEBUG(("Was asked to stop the capture even though it hadn't been started. Ignoring this request."));
#endif
    }

    INFO(("Restoring default callback handlers."));
    RGBSetFrameCapturedFn(CAPTURE_HANDLE, NULL, 0);
    RGBSetModeChangedFn(CAPTURE_HANDLE, NULL, 0);
    RGBSetInvalidSignalFn(CAPTURE_HANDLE, NULL, 0);
    RGBSetNoSignalFn(CAPTURE_HANDLE, NULL, 0);
    RGBSetErrorFn(CAPTURE_HANDLE, NULL, 0);

    return true;

    fail:
    return false;
}

void kc_release_capturer(void)
{
    INFO(("Releasing the capturer."));

    if (FRAME_BUFFER.pixels.is_null())
    {
        DEBUG(("Was asked to release the capturer, but the framebuffer was null. "
               "maybe the capturer hadn't been initialized? Ignoring this request."));

        return;
    }

    if (stop_capture() &&
        shutdown_capture())
    {
        INFO(("The capture card has been released."));
    }
    else
    {
        NBENE(("Failed to release the capture card."));
    }

    FRAME_BUFFER.pixels.release_memory();

    return;
}

u32 kc_input_channel_idx(void)
{
    return (u32)INPUT_CHANNEL_IDX;
}

bool kc_start_capture(void)
{
    INFO(("Starting capture on input channel %d.", (INPUT_CHANNEL_IDX + 1)));

    if (RGBStartCapture(CAPTURE_HANDLE) != RGBERROR_NO_ERROR)
    {
        NBENE(("Failed to start capture on input channel %u.", (INPUT_CHANNEL_IDX + 1)));
        goto fail;
    }
    else
    {
        CAPTURE_IS_ACTIVE = true;
    }

    return true;

    fail:
    return false;
}

bool kc_pause_capture(void)
{
    INFO(("Pausing the capture."));
    return apicall_succeeds(RGBPauseCapture(CAPTURE_HANDLE));
}

bool kc_resume_capture(void)
{
    INFO(("Resuming the capture."));
    return apicall_succeeds(RGBResumeCapture(CAPTURE_HANDLE));
}

bool kc_is_capture_active(void)
{
    return CAPTURE_IS_ACTIVE;
}

bool kc_force_capture_input_resolution(const resolution_s r)
{
    unsigned long wd = 0, hd = 0;

    const auto currentInputRes = kc_hardware().status.capture_resolution();
    if (r.w == currentInputRes.w &&
        r.h == currentInputRes.h)
    {
        DEBUG(("Was asked to force a capture resolution that had already been set. Ignoring the request."));
        goto fail;
    }

    // Test whether the capture card can handle the given resolution.
    if (!apicall_succeeds(RGBTestCaptureWidth(CAPTURE_HANDLE, r.w)))
    {
        NBENE(("Failed to force the new input resolution (%u x %u). The capture card says the width "
               "is illegal.", r.w, r.h));
        goto fail;
    }

    // Set the new resolution.
    if (!apicall_succeeds(RGBSetCaptureWidth(CAPTURE_HANDLE, (unsigned long)r.w)) ||
        !apicall_succeeds(RGBSetCaptureHeight(CAPTURE_HANDLE, (unsigned long)r.h)) ||
        !apicall_succeeds(RGBSetOutputSize(CAPTURE_HANDLE, (unsigned long)r.w, (unsigned long)r.h)))
    {
        NBENE(("The capture card could not properly initialize the new input resolution (%u x %u).",
                r.w, r.h));
        goto fail;
    }

    /// Temp hack.
    RGBGetOutputSize(CAPTURE_HANDLE, &wd, &hd);
    if (wd != r.w ||
        hd != r.h)
    {
        NBENE(("The capture card failed to set the desired resolution."));
        goto fail;
    }

    SKIP_NEXT_NUM_FRAMES += 2;  // Avoid garbage on screen while the mode changes.

    return true;

    fail:
    return false;
}

int kc_alias_resolution_index(const resolution_s r)
{
    for (size_t i = 0; i < ALIASES.size(); i++)
    {
        if (ALIASES[i].from.w == r.w &&
            ALIASES[i].from.h == r.h)
        {
            return i;
        }
    }

    // No alias found.
    return -1;
}

mode_params_s kc_mode_params_for_resolution(const resolution_s r)
{
    for (const auto &m: KNOWN_MODES)
    {
        if (m.r.w == r.w &&
            m.r.h == r.h)
        {
            return m;
        }
    }

    INFO(("Unknown video mode; returning default parameters."));
    return {r,
            CAPTURE_HARDWARE.meta.default_color_settings(),
            CAPTURE_HARDWARE.meta.default_video_settings()};
}

bool kc_apply_mode_parameters(const resolution_s r)
{
    INFO(("Applying mode parameters for %u x %u.", r.w, r.h));

    mode_params_s p = kc_mode_params_for_resolution(r);

    // Apply the set of mode parameters for the current input resolution.
    /// TODO. Add error-checking.
    RGBSetPhase(CAPTURE_HANDLE,         p.video.phase);
    RGBSetBlackLevel(CAPTURE_HANDLE,    p.video.blackLevel);
    RGBSetHorScale(CAPTURE_HANDLE,      p.video.horScale);
    RGBSetHorPosition(CAPTURE_HANDLE,   p.video.horPos);
    RGBSetVerPosition(CAPTURE_HANDLE,   p.video.verPos);
    RGBSetBrightness(CAPTURE_HANDLE,    p.color.bright);
    RGBSetContrast(CAPTURE_HANDLE,      p.color.contr);
    RGBSetColourBalance(CAPTURE_HANDLE, p.color.redBright,
                                        p.color.greenBright,
                                        p.color.blueBright,
                                        p.color.redContr,
                                        p.color.greenContr,
                                        p.color.blueContr);

    return true;
}

bool kc_is_aliased_resolution(void)
{
    return IS_ALIASED_INPUT_RESOLUTION;
}

// See if there isn't an alias resolution for the given resolution.
// If there is, will return that. Otherwise, returns the resolution
// that was passed in.
//
resolution_s aliased(const resolution_s &r)
{
    resolution_s newRes = r;
    const int aliasIdx = kc_alias_resolution_index(r);

    if (aliasIdx >= 0)
    {
        newRes = ALIASES[aliasIdx].to;

        // Try to switch to the alias resolution.
        if (!kc_force_capture_input_resolution(r))
        {
            NBENE(("Failed to apply an alias."));

            IS_ALIASED_INPUT_RESOLUTION = false;
            newRes = r;
        }
        else
        {
            IS_ALIASED_INPUT_RESOLUTION = true;
        }
    }
    else
    {
        IS_ALIASED_INPUT_RESOLUTION = false;
    }

    return newRes;
}

void kc_apply_new_capture_resolution(void)
{
    resolution_s r = aliased(kc_hardware().status.capture_resolution());

    kc_apply_mode_parameters(r);

    RECEIVED_NEW_VIDEO_MODE = false;

    INFO(("Capturer reports new input mode: %u x %u.", r.w, r.h));

    return;
}

bool kc_should_skip_next_frame(void)
{
    return bool(SKIP_NEXT_NUM_FRAMES > 0);
}

void kc_mark_frame_buffer_as_processed(void)
{
    CNT_FRAMES_PROCESSED = CNT_FRAMES_CAPTURED.load();

    if (SKIP_NEXT_NUM_FRAMES > 0)
    {
        SKIP_NEXT_NUM_FRAMES--;
    }

    return;
}

bool kc_is_invalid_signal(void)
{
    return SIGNAL_IS_INVALID;
}

bool kc_no_signal(void)
{
    return !RECEIVING_A_SIGNAL;
}

// Examine the state of the capture system and decide which has been the most recent
// capture event. Note that the order in which these conditionals occur is meaningful.
//
/// FIXME: This is a bit of an ugly way to handle things. For instance, the function
/// is a getter, but also modifies the unit's state.
capture_event_e kc_get_next_capture_event(void)
{
    if (UNRECOVERABLE_CAPTURE_ERROR)
    {
        return CEVENT_UNRECOVERABLE_ERROR;
    }
    else if (RECEIVED_NEW_VIDEO_MODE)
    {
        RECEIVING_A_SIGNAL = true;
        SIGNAL_IS_INVALID = false;

        return CEVENT_NEW_VIDEO_MODE;
    }
    else if (SIGNAL_WAS_LOST)
    {
        RECEIVING_A_SIGNAL = false;
        SIGNAL_WAS_LOST = false;

        return CEVENT_NO_SIGNAL;
    }
    else if (!RECEIVING_A_SIGNAL)
    {
        return CEVENT_SLEEP;
    }
    else if (SIGNAL_BECAME_INVALID)
    {
        RECEIVING_A_SIGNAL = false;
        SIGNAL_IS_INVALID = true;
        SIGNAL_BECAME_INVALID = false;

        return CEVENT_INVALID_SIGNAL;
    }
    else if (SIGNAL_IS_INVALID)
    {
        return CEVENT_SLEEP;
    }
    else if (CNT_FRAMES_CAPTURED != CNT_FRAMES_PROCESSED)
    {
        return CEVENT_NEW_FRAME;
    }

    // If there were no events.
    return CEVENT_NONE;
}

bool kc_set_capture_frame_dropping(const u32 drop)
{
    // Sanity check.
    k_assert(drop < 100, "Odd frame drop number.");

    if (apicall_succeeds(RGBSetFrameDropping(CAPTURE_HANDLE, drop)))
    {
        INFO(("Setting frame drop to %u.", drop));

        FRAME_SKIP = drop;

        goto success;
    }
    else
    {
        NBENE(("Failed to set frame drop to %u.", drop));
        goto fail;
    }

    success:
    return true;

    fail:
    return false;
}

bool kc_set_capture_input_channel(const u32 channel)
{
    if (channel >= MAX_INPUT_CHANNELS)
    {
        goto fail;
    }

    if (apicall_succeeds(RGBSetInput(CAPTURE_HANDLE, channel)))
    {
        INFO(("Setting capture input channel to %u.", (channel + 1)));

        INPUT_CHANNEL_IDX = channel;
    }
    else
    {
        NBENE(("Failed to set capture input channel to %u.", (channel + 1)));

        goto fail;
    }

    return true;

    fail:
    return false;
}

u32 kc_capture_color_depth(void)
{
    return CAPTURE_COLOR_DEPTH;
}

PIXELFORMAT kc_output_pixel_format(void)
{
    return CAPTURE_PIXEL_FORMAT;
}

u32 kc_output_bit_depth(void)
{
    u32 bpp = 0;

    switch (CAPTURE_PIXEL_FORMAT)
    {
        case RGB_PIXELFORMAT_888: bpp = 24; break;
        case RGB_PIXELFORMAT_565: bpp = 16; break;
        case RGB_PIXELFORMAT_555: bpp = 15; break;
        default: k_assert(0, "Found an unknown pixel format while being queried for it.");
    }

    return bpp;
}

bool kc_set_output_bit_depth(const u32 bpp)
{
    const PIXELFORMAT previousFormat = CAPTURE_PIXEL_FORMAT;
    const uint previousColorDepth = CAPTURE_COLOR_DEPTH;

    switch (bpp)
    {
        case 24: CAPTURE_PIXEL_FORMAT = RGB_PIXELFORMAT_888; CAPTURE_COLOR_DEPTH = 32; break;
        case 16: CAPTURE_PIXEL_FORMAT = RGB_PIXELFORMAT_565; CAPTURE_COLOR_DEPTH = 16; break;
        case 15: CAPTURE_PIXEL_FORMAT = RGB_PIXELFORMAT_555; CAPTURE_COLOR_DEPTH = 16;break;
        default: k_assert(0, "Was asked to set an unknown pixel format."); break;
    }

    if (!apicall_succeeds(RGBSetPixelFormat(CAPTURE_HANDLE, CAPTURE_PIXEL_FORMAT)))
    {
        CAPTURE_PIXEL_FORMAT = previousFormat;
        CAPTURE_COLOR_DEPTH = previousColorDepth;

        goto fail;
    }

    // Ignore the next frame to avoid displaying some visual corruption from
    // switching the bit depth.
    SKIP_NEXT_NUM_FRAMES += 1;

    return true;

    fail:
    return false;
}

bool kc_initialize_capture_card(void)
{
    INFO(("Initializing the capture card."));

    if (INPUT_CHANNEL_IDX >= MAX_INPUT_CHANNELS)
    {
        NBENE(("The requested input channel %u is out of bounds.", INPUT_CHANNEL_IDX));

        INPUT_CHANNEL_IS_INVALID = true;

        goto fail;
    }

    if (!apicall_succeeds(RGBLoad(&RGBAPI_HANDLE)))
    {
        goto fail;
    }
    else
    {
        RGBEASY_IS_LOADED = true;
    }

    if (!apicall_succeeds(RGBOpenInput(INPUT_CHANNEL_IDX, &CAPTURE_HANDLE)) ||
        !apicall_succeeds(RGBSetFrameDropping(CAPTURE_HANDLE, FRAME_SKIP)) ||
        !apicall_succeeds(RGBSetDMADirect(CAPTURE_HANDLE, FALSE)) ||
        !apicall_succeeds(RGBSetPixelFormat(CAPTURE_HANDLE, CAPTURE_PIXEL_FORMAT)) ||
        !apicall_succeeds(RGBUseOutputBuffers(CAPTURE_HANDLE, FALSE)) ||
        !apicall_succeeds(RGBSetFrameCapturedFn(CAPTURE_HANDLE, rgbeasy_callbacks_n::frame_captured, 0)) ||
        !apicall_succeeds(RGBSetModeChangedFn(CAPTURE_HANDLE, rgbeasy_callbacks_n::video_mode_changed, 0)) ||
        !apicall_succeeds(RGBSetInvalidSignalFn(CAPTURE_HANDLE, rgbeasy_callbacks_n::invalid_signal, (ULONG_PTR)&CAPTURE_HANDLE)) ||
        !apicall_succeeds(RGBSetErrorFn(CAPTURE_HANDLE, rgbeasy_callbacks_n::error, (ULONG_PTR)&CAPTURE_HANDLE)) ||
        !apicall_succeeds(RGBSetNoSignalFn(CAPTURE_HANDLE, rgbeasy_callbacks_n::no_signal, (ULONG_PTR)&CAPTURE_HANDLE)))
    {
        NBENE(("Failed to initialize the capture card."));
        goto fail;
    }

    /// Temp hack. We've only allocated enough room in the input frame buffer to
    /// hold at most the maximum output size.
    {
        const resolution_s maxCaptureRes = kc_hardware().meta.maximum_capture_resolution();
        k_assert((maxCaptureRes.w <= MAX_OUTPUT_WIDTH &&
                  maxCaptureRes.h <= MAX_OUTPUT_HEIGHT),
                 "The capture device is not compatible with this version of VCS.");
    }

    return true;

    fail:
    return false;
}

// Lets the gui know which aliases we've got loaded.
void kc_broadcast_aliases_to_gui(void)
{
    DEBUG(("Broadcasting %u alias set(s) to the GUI.", ALIASES.size()));

    kd_clear_known_aliases();
    for (const auto &a: ALIASES)
    {
        kd_signal_new_known_alias(a);
    }

    return;
}

void kc_update_alias_resolutions(const std::vector<mode_alias_s> &aliases)
{
    ALIASES = aliases;

    if (!kc_no_signal())
    {
        // If one of the aliases matches the current input resolution, change the
        // resolution accordingly.
        const resolution_s currentRes = kc_hardware().status.capture_resolution();
        for (const auto &alias: ALIASES)
        {
            if (alias.from.w == currentRes.w &&
                alias.from.h == currentRes.h)
            {
                kmain_change_capture_input_resolution(alias.to);
                break;
            }
        }
    }

    return;
}

bool kc_save_aliases(const QString filename)
{
    const QString tempFilename = filename + ".tmp";  // Use a temporary file at first, until we're reasonably sure there were no errors while saving.
    QFile file(tempFilename);
    QTextStream f(&file);

    // Write the aliases into the file.
    {
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            NBENE(("Unable to open the alias file for saving."));
            goto fail;
        }

        for (const auto &a: ALIASES)
        {
            f << a.from.w << ',' << a.from.h << ',' << a.to.w << ',' << a.to.h << ",\n";
        }

        file.close();
    }

    // Replace the existing save file with our new data.
    if (QFile(filename).exists())
    {
        if (!QFile(filename).remove())
        {
            NBENE(("Failed to remove old mode params file."));
            goto fail;
        }
    }
    if (!QFile(tempFilename).rename(filename))
    {
        NBENE(("Failed to write mode params to file."));
        goto fail;
    }

    INFO(("Saved %u aliases to disk.", ALIASES.size()));

    return true;

    fail:
    kd_show_headless_error_message("Data was not saved",
                                   "An error was encountered while preparing the alias "
                                   "resolutions for saving. As a result, no data was saved. \n\nMore "
                                   "information about this error may be found in the terminal.");
    return false;
}

// Loads alias definitions from the given file. Will expect automaticCall to be
// set to false if this function was called directly by a request by the user
// through the GUI to load the aliases (as opposed to being called automatically
// on startup or so).
//
bool kc_load_aliases(const QString filename, const bool automaticCall)
{
    if (filename.isEmpty())
    {
        DEBUG(("No alias file defined, skipping."));
        return true;
    }

    QList<QStringList> rowData = csv_parse_c(filename).contents();
    std::vector<mode_alias_s> aliasesFromDisk;
    for (const auto &row: rowData)
    {
        if (row.count() != 4)
        {
            NBENE(("Expected a 4-parameter row in the alias file."));
            goto fail;
        }

        mode_alias_s a;
        a.from.w = row.at(0).toUInt();
        a.from.h = row.at(1).toUInt();
        a.to.w = row.at(2).toUInt();
        a.to.h = row.at(3).toUInt();

        aliasesFromDisk.push_back(a);
    }

    kc_update_alias_resolutions(aliasesFromDisk);

    // Sort the parameters so they display more nicely in the GUI.
    std::sort(ALIASES.begin(), ALIASES.end(), [](const mode_alias_s &a, const mode_alias_s &b)
                                              { return (a.to.w * a.to.h) < (b.to.w * b.to.h); });

    kc_broadcast_aliases_to_gui();

    INFO(("Loaded %u alias set(s) from disk.", ALIASES.size()));

    if (!automaticCall)
    {
        // Signal a new input mode to force the program to re-evaluate the mode
        // parameters, in case one of the newly-loaded aliases applies to the
        // current mode.
        kpropagate_new_input_video_mode();
    }

    return true;

    fail:
    kd_show_headless_error_message("Data was not loaded",
                                   "An error was encountered while loading the alias "
                                   "file. No data was loaded.\n\nMore "
                                   "information about the error may be found in the terminal.");
    return false;
}

bool kc_save_mode_params(const QString filename)
{
    const QString tempFilename = filename + ".tmp";  // Use a temporary file at first, until we're reasonably sure there were no errors while saving.
    QFile file(tempFilename);
    QTextStream f(&file);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        NBENE(("Unable to open the mode parameter file for saving."));
        goto fail;
    }

    // Each mode params block consists of two values specifying the resolution
    // followed by a set of string-value pairs for the different parameters.
    for (const auto &m: KNOWN_MODES)
    {
        // Resolution.
        f << "resolution," << m.r.w << ',' << m.r.h << '\n';
        if (file.error() != QFileDevice::NoError)
        {
            NBENE(("Failed to write mode params to file."));
            goto fail;
        }

        // Video params.
        f << "vPos," << m.video.verPos << '\n'
          << "hPos," << m.video.horPos << '\n'
          << "hScale," << m.video.horScale << '\n'
          << "phase," << m.video.phase << '\n'
          << "bLevel," << m.video.blackLevel << '\n';
        if (file.error() != QFileDevice::NoError)
        {
            NBENE(("Failed to write mode params to file."));
            goto fail;
        }

        // Color params.
        f << "bright," << m.color.bright << '\n'
          << "contr," << m.color.contr << '\n'
          << "redBr," << m.color.redBright << '\n'
          << "redCn," << m.color.redContr << '\n'
          << "greenBr," << m.color.greenBright << '\n'
          << "greenCn," << m.color.greenContr << '\n'
          << "blueBr," << m.color.blueBright << '\n'
          << "blueCn," << m.color.blueContr << '\n';
        if (file.error() != QFileDevice::NoError)
        {
            NBENE(("Failed to write mode params to file."));
            goto fail;
        }

        f << '\n';  // Separate the next block.
    }

    file.close();

    // Replace the existing save file with our new data.
    if (QFile(filename).exists())
    {
        if (!QFile(filename).remove())
        {
            NBENE(("Failed to remove old mode params file."));
            goto fail;
        }
    }
    if (!QFile(tempFilename).rename(filename))
    {
        NBENE(("Failed to write mode params to file."));
        goto fail;
    }

    INFO(("Saved %u set(s) of mode params to disk.", KNOWN_MODES.size()));

    kd_signal_new_mode_settings_source_file(filename);

    return true;

    fail:
    kd_show_headless_error_message("Data was not saved",
                                   "An error was encountered while preparing the mode "
                                   "settings for saving. As a result, no data was saved. \n\nMore "
                                   "information about this error may be found in the terminal.");
    return false;
}

void kc_broadcast_mode_params_to_gui(void)
{
    kd_clear_known_modes();
    for (const auto &m: KNOWN_MODES)
    {
        kd_signal_new_known_mode(m.r);
    }

    return;
}

bool kc_load_mode_params(const QString filename, const bool automaticCall)
{
    std::vector<mode_params_s> modesFromDisk;

    if (filename.isEmpty())
    {
        DEBUG(("No mode settings file defined, skipping."));
        return true;
    }

    QList<QStringList> paramRows = csv_parse_c(filename).contents();

    // Each mode is saved as a block of rows, starting with a 3-element row defining
    // the mode's resolution, followed by several 2-element rows defining the various
    // video and color parameters for the resolution.
    for (int i = 0; i < paramRows.count();)
    {
        if ((paramRows[i].count() != 3) ||
            (paramRows[i].at(0) != "resolution"))
        {
            NBENE(("Expected a 3-parameter 'resolution' statement to begin a mode params block."));
            goto fail;
        }

        mode_params_s p;
        p.r.w = paramRows[i].at(1).toUInt();
        p.r.h = paramRows[i].at(2).toUInt();

        i++;    // Move to the next row to start fetching the params for this resolution.

        auto get_param = [&](const QString &name)->QString
                         {
                             if (paramRows[i].at(0) != name)
                             {
                                 NBENE(("Error while loading the mode params file: expected '%s' but got '%s'.",
                                        name.toLatin1().constData(), paramRows[i].at(0).toLatin1().constData()));
                                 throw 0;
                             }
                             return paramRows[i++].at(1);
                         };
        try
        {
            // Note: the order in which the params are fetched is fixed to the
            // order in which they were saved.
            p.video.verPos = get_param("vPos").toInt();
            p.video.horPos = get_param("hPos").toInt();
            p.video.horScale = get_param("hScale").toInt();
            p.video.phase = get_param("phase").toInt();
            p.video.blackLevel = get_param("bLevel").toInt();
            p.color.bright = get_param("bright").toInt();
            p.color.contr = get_param("contr").toInt();
            p.color.redBright = get_param("redBr").toInt();
            p.color.redContr = get_param("redCn").toInt();
            p.color.greenBright = get_param("greenBr").toInt();
            p.color.greenContr = get_param("greenCn").toInt();
            p.color.blueBright = get_param("blueBr").toInt();
            p.color.blueContr = get_param("blueCn").toInt();
        }
        catch (int)
        {
            NBENE(("Failed to load mode params from disk."));
            goto fail;
        }

        modesFromDisk.push_back(p);
    }

    KNOWN_MODES = modesFromDisk;

    // Sort the modes so they display more nicely in the GUI.
    std::sort(KNOWN_MODES.begin(), KNOWN_MODES.end(), [](const mode_params_s &a, const mode_params_s &b)
                                                      { return (a.r.w * a.r.h) < (b.r.w * b.r.h); });

    // Update the GUI with information related to the new mode params.
    kc_broadcast_mode_params_to_gui();
    kpropagate_new_input_video_mode();  // In case the mode params changed for the current mode, re-initialize it.
    kd_signal_new_mode_settings_source_file(filename);

    INFO(("Loaded %u set(s) of mode params from disk.", KNOWN_MODES.size()));
    if (!automaticCall)
    {
        kd_show_headless_info_message("Data was loaded",
                                      "The mode parameters were successfully loaded.");
    }

    return true;

    fail:
    kd_show_headless_error_message("Data was not loaded",
                                   "An error was encountered while loading the mode "
                                   "parameter file. No data was loaded.\n\nMore "
                                   "information about the error may be found in the terminal.");
    return false;
}

void kc_set_capture_color_params(const input_color_settings_s c)
{
    if (kc_no_signal())
    {
        DEBUG(("Was asked to set capture color params while there was no signal. "
               "Ignoring the request."));
        return;
    }

    RGBSetBrightness(CAPTURE_HANDLE, c.bright);
    RGBSetContrast(CAPTURE_HANDLE, c.contr);
    RGBSetColourBalance(CAPTURE_HANDLE, c.redBright,
                                        c.greenBright,
                                        c.blueBright,
                                        c.redContr,
                                        c.greenContr,
                                        c.blueContr);

    update_known_mode_params(kc_hardware().status.capture_resolution(), &c, nullptr);

    return;
}

void kc_set_capture_video_params(const input_video_settings_s v)
{
    if (kc_no_signal())
    {
        DEBUG(("Was asked to set capture video params while there was no signal. "
               "Ignoring the request."));
        return;
    }

    RGBSetPhase(CAPTURE_HANDLE, v.phase);
    RGBSetBlackLevel(CAPTURE_HANDLE, v.blackLevel);
    RGBSetHorPosition(CAPTURE_HANDLE, v.horPos);
    RGBSetHorScale(CAPTURE_HANDLE, v.horScale);
    RGBSetVerPosition(CAPTURE_HANDLE, v.verPos);

    update_known_mode_params(kc_hardware().status.capture_resolution(), nullptr, &v);

    return;
}

#ifdef VALIDATION_RUN
    void kc_VALIDATION_set_capture_color_depth(const uint bpp)
    {
        CAPTURE_COLOR_DEPTH = bpp;
        return;
    }

    void kc_VALIDATION_set_capture_pixel_format(const PIXELFORMAT pf)
    {
        CAPTURE_PIXEL_FORMAT = pf;
        return;
    }
#endif

bool capture_hardware_s::features_supported_s::component_capture() const
{
    for (uint i = 0; i < MAX_INPUT_CHANNELS; i++)
    {
        long isSupported = 0;
        if (!apicall_succeeds(RGBInputIsComponentSupported(i, &isSupported))) return false;
        if (isSupported) return true;
    }
    return false;
}

bool capture_hardware_s::features_supported_s::composite_capture() const
{
    long isSupported = 0;
    for (uint i = 0; i < MAX_INPUT_CHANNELS; i++)
    {
        if (!apicall_succeeds(RGBInputIsCompositeSupported(i, &isSupported))) return false;
        if (isSupported) return true;
    }
    return false;
}

bool capture_hardware_s::features_supported_s::deinterlace() const
{
    long isSupported = 0;
    if (!apicall_succeeds(RGBIsDeinterlaceSupported(&isSupported))) return false;
    return isSupported;
}

bool capture_hardware_s::features_supported_s::dma() const
{
    long isSupported = 0;
    if (!apicall_succeeds(RGBIsDirectDMASupported(&isSupported))) return false;
    return isSupported;
}

bool capture_hardware_s::features_supported_s::dvi() const
{
    for (uint i = 0; i < MAX_INPUT_CHANNELS; i++)
    {
        long isSupported = 0;
        if (!apicall_succeeds(RGBInputIsDVISupported(i, &isSupported))) return false;
        if (isSupported) return true;
    }
    return false;
}

bool capture_hardware_s::features_supported_s::svideo() const
{
    for (uint i = 0; i < MAX_INPUT_CHANNELS; i++)
    {
        long isSupported = 0;
        if (!apicall_succeeds(RGBInputIsSVideoSupported(i, &isSupported))) return false;
        if (isSupported) return true;
    }
    return false;
}

bool capture_hardware_s::features_supported_s::vga() const
{
    for (uint i = 0; i < MAX_INPUT_CHANNELS; i++)
    {
        long isSupported = 0;
        if (!apicall_succeeds(RGBInputIsVGASupported(i, &isSupported))) return false;
        if (isSupported) return true;
    }
    return false;
}

bool capture_hardware_s::features_supported_s::yuv() const
{
    long isSupported = 0;
    if (!apicall_succeeds(RGBIsYUVSupported(&isSupported))) return false;
    return isSupported;
}

std::string capture_hardware_s::metainfo_s::model_name() const
{
    const std::string unknownName = "Unknown capture device";

    CAPTURECARD card = RGB_CAPTURECARD_DGC103;
    if (!apicall_succeeds(RGBGetCaptureCard(&card))) return unknownName;

    switch (card)
    {
        case RGB_CAPTURECARD_DGC103: return "Datapath VisionRGB-PRO";
        case RGB_CAPTURECARD_DGC133: return "Datapath DGC133 Series";
        default: break;
    }

    return unknownName;
}

int capture_hardware_s::metainfo_s::minimum_frame_drop() const
{
    unsigned long frameDrop = 0;
    if (!apicall_succeeds(RGBGetFrameDroppingMinimum(CAPTURE_HANDLE, &frameDrop))) return -1;
    return frameDrop;
}

int capture_hardware_s::metainfo_s::maximum_frame_drop() const
{
    unsigned long frameDrop = 0;
    if (!apicall_succeeds(RGBGetFrameDroppingMaximum(CAPTURE_HANDLE, &frameDrop))) return -1;
    return frameDrop;
}

std::string capture_hardware_s::metainfo_s::driver_version() const
{
    const std::string unknownVersion = "Unknown";

    RGBINPUTINFO ii = {0};
    ii.Size = sizeof(ii);

    if (!apicall_succeeds(RGBGetInputInfo(INPUT_CHANNEL_IDX, &ii))) return unknownVersion;

    return std::string(std::to_string(ii.Driver.Major) + "." +
                       std::to_string(ii.Driver.Minor) + "." +
                       std::to_string(ii.Driver.Micro) + "/" +
                       std::to_string(ii.Driver.Revision));
}

std::string capture_hardware_s::metainfo_s::firmware_version() const
{
    const std::string unknownVersion = "Unknown";

    RGBINPUTINFO ii = {0};
    ii.Size = sizeof(ii);

    if (!apicall_succeeds(RGBGetInputInfo(INPUT_CHANNEL_IDX, &ii))) return unknownVersion;

    return std::to_string(ii.FirmWare);
}

input_color_settings_s capture_hardware_s::metainfo_s::default_color_settings() const
{
    input_color_settings_s p = {0};

    if (!apicall_succeeds(RGBGetBrightnessDefault(CAPTURE_HANDLE, &p.bright)) ||
        !apicall_succeeds(RGBGetContrastDefault(CAPTURE_HANDLE, &p.contr)) ||
        !apicall_succeeds(RGBGetColourBalanceDefault(CAPTURE_HANDLE, &p.redBright,
                                                                     &p.greenBright,
                                                                     &p.blueBright,
                                                                     &p.redContr,
                                                                     &p.greenContr,
                                                                     &p.blueContr)))
    {
        return {0};
    }

    return p;
}

input_color_settings_s capture_hardware_s::metainfo_s::minimum_color_settings() const
{
    input_color_settings_s p = {0};

    if (!apicall_succeeds(RGBGetBrightnessMinimum(CAPTURE_HANDLE, &p.bright)) ||
        !apicall_succeeds(RGBGetContrastMinimum(CAPTURE_HANDLE, &p.contr)) ||
        !apicall_succeeds(RGBGetColourBalanceMinimum(CAPTURE_HANDLE, &p.redBright,
                                                                     &p.greenBright,
                                                                     &p.blueBright,
                                                                     &p.redContr,
                                                                     &p.greenContr,
                                                                     &p.blueContr)))
    {
        return {0};
    }

    return p;
}

input_color_settings_s capture_hardware_s::metainfo_s::maximum_color_settings() const
{
    input_color_settings_s p = {0};

    if (!apicall_succeeds(RGBGetBrightnessMaximum(CAPTURE_HANDLE, &p.bright)) ||
        !apicall_succeeds(RGBGetContrastMaximum(CAPTURE_HANDLE, &p.contr)) ||
        !apicall_succeeds(RGBGetColourBalanceMaximum(CAPTURE_HANDLE, &p.redBright,
                                                                     &p.greenBright,
                                                                     &p.blueBright,
                                                                     &p.redContr,
                                                                     &p.greenContr,
                                                                     &p.blueContr)))
    {
        return {0};
    }

    return p;
}

input_video_settings_s capture_hardware_s::metainfo_s::default_video_settings() const
{
    input_video_settings_s p = {0};

    if (!apicall_succeeds(RGBGetPhaseDefault(CAPTURE_HANDLE, &p.phase)) ||
        !apicall_succeeds(RGBGetBlackLevelDefault(CAPTURE_HANDLE, &p.blackLevel)) ||
        !apicall_succeeds(RGBGetHorPositionDefault(CAPTURE_HANDLE, &p.horPos)) ||
        !apicall_succeeds(RGBGetVerPositionDefault(CAPTURE_HANDLE, &p.verPos)) ||
        !apicall_succeeds(RGBGetHorScaleDefault(CAPTURE_HANDLE, &p.horScale)))
    {
        return {0};
    }

    return p;
}

input_video_settings_s capture_hardware_s::metainfo_s::minimum_video_settings() const
{
    input_video_settings_s p = {0};

    if (!apicall_succeeds(RGBGetPhaseMinimum(CAPTURE_HANDLE, &p.phase)) ||
        !apicall_succeeds(RGBGetBlackLevelMinimum(CAPTURE_HANDLE, &p.blackLevel)) ||
        !apicall_succeeds(RGBGetHorPositionMinimum(CAPTURE_HANDLE, &p.horPos)) ||
        !apicall_succeeds(RGBGetVerPositionMinimum(CAPTURE_HANDLE, &p.verPos)) ||
        !apicall_succeeds(RGBGetHorScaleMinimum(CAPTURE_HANDLE, &p.horScale)))
    {
        return {0};
    }

    return p;
}

input_video_settings_s capture_hardware_s::metainfo_s::maximum_video_settings() const
{
    input_video_settings_s p = {0};

    if (!apicall_succeeds(RGBGetPhaseMaximum(CAPTURE_HANDLE, &p.phase)) ||
        !apicall_succeeds(RGBGetBlackLevelMaximum(CAPTURE_HANDLE, &p.blackLevel)) ||
        !apicall_succeeds(RGBGetHorPositionMaximum(CAPTURE_HANDLE, &p.horPos)) ||
        !apicall_succeeds(RGBGetVerPositionMaximum(CAPTURE_HANDLE, &p.verPos)) ||
        !apicall_succeeds(RGBGetHorScaleMaximum(CAPTURE_HANDLE, &p.horScale)))
    {
        return {0};
    }

    return p;
}

resolution_s capture_hardware_s::metainfo_s::minimum_capture_resolution() const
{
    resolution_s r;

#if !USE_RGBEASY_API
    r.w = 1;
    r.h = 1;
#else
    if (!apicall_succeeds(RGBGetCaptureWidthMinimum(CAPTURE_HANDLE, &r.w)) ||
        !apicall_succeeds(RGBGetCaptureHeightMinimum(CAPTURE_HANDLE, &r.h)))
    {
        return {0};
    }
#endif

    // NOTE: It's assumed that 16-bit is the minimum capture color depth.
    r.bpp = 16;

    return r;
}

resolution_s capture_hardware_s::metainfo_s::maximum_capture_resolution() const
{
    resolution_s r;

#if !USE_RGBEASY_API
    r.w = 1920;
    r.h = 1260;
#else
    if (!apicall_succeeds(RGBGetCaptureWidthMaximum(CAPTURE_HANDLE, &r.w)) ||
        !apicall_succeeds(RGBGetCaptureHeightMaximum(CAPTURE_HANDLE, &r.h)))
    {
        return {0};
    }
#endif

    // NOTE: It's assumed that 32-bit is the maximum capture color depth.
    r.bpp = 32;

    return r;
}

int capture_hardware_s::metainfo_s::num_capture_inputs() const
{
    unsigned long numInputs = 0;
    if (!apicall_succeeds(RGBGetNumberOfInputs(&numInputs))) return -1;
    return numInputs;
}

bool capture_hardware_s::metainfo_s::is_dma_enabled() const
{
    long isEnabled = 0;
    if (!apicall_succeeds(RGBGetDMADirect(CAPTURE_HANDLE, &isEnabled))) return false;
    return isEnabled;
}

resolution_s capture_hardware_s::status_s::capture_resolution() const
{
    resolution_s r;

#if USE_RGBEASY_API
    if (!apicall_succeeds(RGBGetCaptureWidth(CAPTURE_HANDLE, &r.w)) ||
        !apicall_succeeds(RGBGetCaptureHeight(CAPTURE_HANDLE, &r.h)))
    {
        k_assert(0, "The capture card failed to report its input resolution.");
    }
#else
    r.w = 640;
    r.h = 480;
#endif

    r.bpp = CAPTURE_COLOR_DEPTH;

    return r;
}

input_color_settings_s capture_hardware_s::status_s::color_settings() const
{
    input_color_settings_s p = {0};

    if (!apicall_succeeds(RGBGetBrightness(CAPTURE_HANDLE, &p.bright)) ||
        !apicall_succeeds(RGBGetContrast(CAPTURE_HANDLE, &p.contr)) ||
        !apicall_succeeds(RGBGetColourBalance(CAPTURE_HANDLE, &p.redBright,
                                                              &p.greenBright,
                                                              &p.blueBright,
                                                              &p.redContr,
                                                              &p.greenContr,
                                                              &p.blueContr)))
    {
        return {0};
    }

    return p;
}

input_video_settings_s capture_hardware_s::status_s::video_settings() const
{
    input_video_settings_s p = {0};

    if (!apicall_succeeds(RGBGetPhase(CAPTURE_HANDLE, &p.phase)) ||
        !apicall_succeeds(RGBGetBlackLevel(CAPTURE_HANDLE, &p.blackLevel)) ||
        !apicall_succeeds(RGBGetHorPosition(CAPTURE_HANDLE, &p.horPos)) ||
        !apicall_succeeds(RGBGetVerPosition(CAPTURE_HANDLE, &p.verPos)) ||
        !apicall_succeeds(RGBGetHorScale(CAPTURE_HANDLE, &p.horScale)))
    {
        return {0};
    }

    return p;
}

input_signal_s capture_hardware_s::status_s::signal() const
{
    if (kc_no_signal())
    {
        NBENE(("Tried to query the capture signal while no signal was being received."));
        return {0};
    }

    input_signal_s s = {0};

#if !USE_RGBEASY_API
    s.r.w = 640;
    s.r.h = 480;
    s.refreshRate = 60;
#else
    RGBMODEINFO mi = {0};
    mi.Size = sizeof(mi);

    s.wokeUp = SIGNAL_WOKE_UP;

    if (apicall_succeeds(RGBGetModeInfo(CAPTURE_HANDLE, &mi)))
    {
        s.isInterlaced = mi.BInterlaced;
        s.isDigital = mi.BDVI;
        s.refreshRate = round(mi.RefreshRate / 1000.0);
    }
    else
    {
        s.isInterlaced = 0;
        s.isDigital = false;
        s.refreshRate = 0;
    }

    s.r = CAPTURE_HARDWARE.status.capture_resolution();
#endif

    return s;
}

int capture_hardware_s::status_s::frame_rate() const
{
    unsigned long rate = 0;
    if (!apicall_succeeds(RGBGetFrameRate(CAPTURE_HANDLE, &rate))) return -1;
    return rate;
}
