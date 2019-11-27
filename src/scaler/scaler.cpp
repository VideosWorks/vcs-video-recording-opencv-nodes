/*
 * 2018 Tarpeeksi Hyvae Soft /
 * VCS scaler
 *
 * Scales captured frames to match a desired output resolution (for e.g. displaying on screen).
 *
 */

#include <algorithm>
#include <cstring>
#include <vector>
#include <cmath>
#include "filter/anti_tear.h"
#include "common/propagate.h"
#include "capture/capture.h"
#include "display/display.h"
#include "common/globals.h"
#include "common/memory.h"
#include "filter/filter.h"
#include "record/record.h"
#include "scaler/scaler.h"

#ifdef USE_OPENCV
    #include <opencv2/imgproc/imgproc.hpp>
    #include <opencv2/core/core.hpp>
#endif

void s_scaler_nearest(SCALER_FUNC_PARAMS);
void s_scaler_linear(SCALER_FUNC_PARAMS);
void s_scaler_area(SCALER_FUNC_PARAMS);
void s_scaler_cubic(SCALER_FUNC_PARAMS);
void s_scaler_lanczos(SCALER_FUNC_PARAMS);

static const scaling_filter_s *UPSCALE_FILTER = nullptr;
static const scaling_filter_s *DOWNSCALE_FILTER = nullptr;
static const std::vector<scaling_filter_s> SCALING_FILTERS =    // User-facing scaling filters. Note that these names will be shown in the GUI.
#ifdef USE_OPENCV
                {{"Nearest", &s_scaler_nearest},
                 {"Linear",  &s_scaler_linear},
                 {"Area",    &s_scaler_area},
                 {"Cubic",   &s_scaler_cubic},
                 {"Lanczos", &s_scaler_lanczos}};
#else
                {{"Nearest", &s_scaler_nearest}};
#endif

// The pixel buffer where scaled frames are to be placed.
static heap_bytes_s<u8> OUTPUT_BUFFER;

// Scratch buffers.
static heap_bytes_s<u8> COLORCONV_BUFFER;
static heap_bytes_s<u8> TMP_BUFFER;

static aspect_mode_e ASPECT_MODE = aspect_mode_e::native;
static bool FORCE_ASPECT = true;

static resolution_s LATEST_OUTPUT_SIZE = {0};       // The size of the image currently in the scaler's output buffer.

static const u32 OUTPUT_BIT_DEPTH = 32;             // The bit depth we're currently scaling to.

static resolution_s BASE_RESOLUTION = {640, 480, 0};// The size of the capture window, before any other scaling.
static bool FORCE_BASE_RESOLUTION = false;          // If false, the base resolution will track the capture card's output resolution.

// The multiplier by which to up/downscale the base output resolution.
static real OUTPUT_SCALING = 1;
static bool FORCE_SCALING = false;

void ks_set_aspect_mode(const aspect_mode_e mode)
{
    ASPECT_MODE = mode;

    return;
}

aspect_mode_e ks_aspect_mode(void)
{
    return ASPECT_MODE;
}

resolution_s ks_resolution_to_aspect(const resolution_s &r)
{
    resolution_s a;
    const int gcd = std::__gcd(r.w, r.h);

    a.w = (r.w / gcd);
    a.h = (r.h / gcd);

    return a;
}

resolution_s ks_output_base_resolution(void)
{
    return BASE_RESOLUTION;
}

// Returns the resolution at which the scaler will output after performing all the actions
// (e.g. relative scaling or aspect ratio correction) that it has been asked to.
//
resolution_s ks_output_resolution(void)
{
    // While recording video, the output resolution is required to stay locked
    // to the video resolution.
    if (krecord_is_recording())
    {
        const auto r = krecord_video_resolution();
        return {r.w, r.h, OUTPUT_BIT_DEPTH};
    }

    resolution_s inRes = kc_hardware().status.capture_resolution();
    resolution_s outRes = inRes;

    // Base resolution.
    if (FORCE_BASE_RESOLUTION)
    {
        outRes = BASE_RESOLUTION;
    }

    // Magnification.
    if (FORCE_SCALING)
    {
        outRes.w = round(outRes.w * OUTPUT_SCALING);
        outRes.h = round(outRes.h * OUTPUT_SCALING);
    }

    // Bounds-check.
    {
        if (outRes.w > MAX_OUTPUT_WIDTH)
        {
            outRes.w = MAX_OUTPUT_HEIGHT;
        }
        else if (outRes.w < MIN_OUTPUT_WIDTH)
        {
            outRes.w = MIN_OUTPUT_WIDTH;
        }

        if (outRes.h > MAX_OUTPUT_HEIGHT)
        {
            outRes.h = MAX_OUTPUT_HEIGHT;
        }
        else if (outRes.h < MIN_OUTPUT_HEIGHT)
        {
            outRes.h = MIN_OUTPUT_HEIGHT;
        }
    }

    outRes.bpp = OUTPUT_BIT_DEPTH;

    return outRes;
}

bool ks_is_forced_aspect_enabled(void)
{
    return FORCE_ASPECT;
}

#if USE_OPENCV
// Returns a resolution corresponding to sourceRes scaled up to targetRes but
// maintaining sourceRes's aspect ratio according to the scaler's current aspect
// mode.
//
static resolution_s padded_resolution(const resolution_s &sourceRes, const resolution_s &targetRes)
{
    const resolution_s aspect = [sourceRes]()->resolution_s
    {
        switch (ASPECT_MODE)
        {
            case aspect_mode_e::native: return ks_resolution_to_aspect(sourceRes);
            case aspect_mode_e::always_4_3: return {4, 3, 0};
            case aspect_mode_e::traditional_4_3:
            {
                if ((sourceRes.w == 720 && sourceRes.h == 400) ||
                    (sourceRes.w == 640 && sourceRes.h == 400) ||
                    (sourceRes.w == 320 && sourceRes.h == 200))
                {
                    return {4, 3, 0};
                }
                else
                {
                    return ks_resolution_to_aspect(sourceRes);
                }
            }
            default: k_assert(0, "Unknown aspect mode."); return ks_resolution_to_aspect(sourceRes);
        }
    }();
    const real aspectRatio = (aspect.w / (real)aspect.h);
    uint w = std::round(targetRes.h * aspectRatio);
    uint h = targetRes.h;
    if (w > targetRes.w)
    {
        const real aspectRatio = (aspect.h / (real)aspect.w);
        w = targetRes.w;
        h = std::round(targetRes.w * aspectRatio);
    }

    return {w, h, OUTPUT_BIT_DEPTH};
}

// Returns border padding sizes for cv::copyMakeBorder()
//
static cv::Vec4i border_padding(const resolution_s &paddedRes, const resolution_s &targetRes)
{
    cv::Vec4i p;

    p[0] = ((targetRes.h - paddedRes.h) / 2);     // Top.
    p[1] = ((targetRes.h - paddedRes.h + 1) / 2); // Bottom.
    p[2] = ((targetRes.w - paddedRes.w) / 2);     // Left.
    p[3] = ((targetRes.w - paddedRes.w + 1) / 2); // Right.

    return p;
}

// Copies src into dsts and adds a border of the given size.
//
void copy_with_border(const cv::Mat &src, cv::Mat &dst, const cv::Vec4i &borderSides)
{
    cv::copyMakeBorder(src, dst, borderSides[0], borderSides[1], borderSides[2], borderSides[3], cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));

    return;
}

// Scales the given pixel data using OpenCV.
//
void opencv_scale(u8 *const pixelData,
                  u8 *const outputBuffer,
                  const resolution_s &sourceRes,
                  const resolution_s &targetRes,
                  const cv::InterpolationFlags interpolator)
{
    cv::Mat scratch = cv::Mat(sourceRes.h, sourceRes.w, CV_8UC4, pixelData);
    cv::Mat output = cv::Mat(targetRes.h, targetRes.w, CV_8UC4, outputBuffer);

    if (ks_is_forced_aspect_enabled())
    {
        const resolution_s paddedRes = padded_resolution(sourceRes, targetRes);
        cv::Mat tmp = cv::Mat(paddedRes.h, paddedRes.w, CV_8UC4, TMP_BUFFER.ptr());

        if ((paddedRes.h == targetRes.h) &&
            (paddedRes.w == targetRes.w))
        {
            // No padding is needed, so we can resize directly into the output buffer.
            cv::resize(scratch, output, output.size(), 0, 0, interpolator);
        }
        else
        {
            cv::resize(scratch, tmp, tmp.size(), 0, 0, interpolator);
            copy_with_border(tmp, output, border_padding(paddedRes, targetRes));
        }
    }
    else
    {
        cv::resize(scratch, output, output.size(), 0, 0, interpolator);
    }

    return;
}

#endif

void s_scaler_nearest(SCALER_FUNC_PARAMS)
{
    k_assert((sourceRes.bpp == 32) && (targetRes.bpp == 32),
             "This filter requires 32-bit source and target color.")
    if (pixelData == nullptr)
    {
        return;
    }

    #if USE_OPENCV
        opencv_scale(pixelData, OUTPUT_BUFFER.ptr(), sourceRes, targetRes, cv::INTER_NEAREST);
    #else
        /// TODO. Implement a non-OpenCV nearest scaler so there's a basic fallback.
        k_assert(0, "Attempted to use a scaling filter that hasn't been implemented for non-OpenCV builds.");
    #endif

    return;
}

void s_scaler_linear(SCALER_FUNC_PARAMS)
{
    k_assert((sourceRes.bpp == 32) && (targetRes.bpp == 32),
             "This filter requires 32-bit source and target color.")
    if (pixelData == nullptr)
    {
        return;
    }

    #if USE_OPENCV
        opencv_scale(pixelData, OUTPUT_BUFFER.ptr(), sourceRes, targetRes, cv::INTER_LINEAR);
    #else
        k_assert(0, "Attempted to use a scaling filter that hasn't been implemented for non-OpenCV builds.");
    #endif

    return;
}

void s_scaler_area(SCALER_FUNC_PARAMS)
{
    k_assert((sourceRes.bpp == 32) && (targetRes.bpp == 32),
             "This filter requires 32-bit source and target color.")
    if (pixelData == nullptr)
    {
        return;
    }

    #if USE_OPENCV
        opencv_scale(pixelData, OUTPUT_BUFFER.ptr(), sourceRes, targetRes, cv::INTER_AREA);
    #else
        k_assert(0, "Attempted to use a scaling filter that hasn't been implemented for non-OpenCV builds.");
    #endif

    return;
}

void s_scaler_cubic(SCALER_FUNC_PARAMS)
{
    k_assert((sourceRes.bpp == 32) && (targetRes.bpp == 32),
             "This filter requires 32-bit source and target color.")
    if (pixelData == nullptr)
    {
        return;
    }

    #if USE_OPENCV
        opencv_scale(pixelData, OUTPUT_BUFFER.ptr(), sourceRes, targetRes, cv::INTER_CUBIC);
    #else
        k_assert(0, "Attempted to use a scaling filter that hasn't been implemented for non-OpenCV builds.");
    #endif

    return;
}

void s_scaler_lanczos(SCALER_FUNC_PARAMS)
{
    k_assert((sourceRes.bpp == 32) && (targetRes.bpp == 32),
             "This filter requires 32-bit source and target color.")
    if (pixelData == nullptr)
    {
        return;
    }

    #if USE_OPENCV
        opencv_scale(pixelData, OUTPUT_BUFFER.ptr(), sourceRes, targetRes, cv::INTER_LANCZOS4);
    #else
        k_assert(0, "Attempted to use a scaling filter that hasn't been implemented for non-OpenCV builds.");
    #endif

    return;
}

uint ks_max_output_bit_depth(void)
{
    return MAX_OUTPUT_BPP;
}

// Replaces OpenCV's default error handler.
//
int cv_error_handler(int status, const char* func_name,
                     const char* err_msg, const char* file_name, int line, void* userdata)
{
    NBENE(("OpenCV reports an error: '%s'.", err_msg));
    k_assert(0, "OpenCV reported an error.");

    (void)func_name;
    (void)file_name;
    (void)userdata;
    (void)err_msg;
    (void)status;
    (void)line;

    return 1;
}

void ks_initialize_scaler(void)
{
    INFO(("Initializing the scaler."));

    #if USE_OPENCV
        cv::redirectError(cv_error_handler);
    #endif

    OUTPUT_BUFFER.alloc(MAX_FRAME_SIZE, "Scaler output buffer");
    COLORCONV_BUFFER.alloc(MAX_FRAME_SIZE, "Scaler color convertion buffer");
    TMP_BUFFER.alloc(MAX_FRAME_SIZE, "Scaler scratch buffer");

    ks_set_upscaling_filter(SCALING_FILTERS.at(0).name);
    ks_set_downscaling_filter(SCALING_FILTERS.at(0).name);

    return;
}

void ks_release_scaler(void)
{
    INFO(("Releasing the scaler."));

    COLORCONV_BUFFER.release_memory();
    OUTPUT_BUFFER.release_memory();
    TMP_BUFFER.release_memory();

    return;
}

// Converts the given frame to BGRA format.
//
void s_convert_frame_to_bgra(const captured_frame_s &frame)
{
    #ifdef USE_OPENCV
        u32 conversionType = 0;
        const u32 numColorChan = (frame.r.bpp / 8);

        cv::Mat input = cv::Mat(frame.r.h, frame.r.w, CV_MAKETYPE(CV_8U,numColorChan), frame.pixels.ptr());
        cv::Mat colorConv = cv::Mat(frame.r.h, frame.r.w, CV_8UC4, COLORCONV_BUFFER.ptr());

        k_assert(!COLORCONV_BUFFER.is_null(),
                 "Was asked to convert a frame's color depth, but the color conversion buffer "
                 "was null.");

        if (kc_pixel_format() == RGB_PIXELFORMAT_565)
        {
            conversionType = CV_BGR5652BGRA;
        }
        else if (kc_pixel_format() == RGB_PIXELFORMAT_555)
        {
            conversionType = CV_BGR5552BGRA;
        }
        else // The third pixel format we recognize is RGB_PIXELFORMAT_888; it should never need this conversion, as it arrives in BGRA.
        {
            // k_assert(0, "Was asked to scale a frame from an unknown pixel format.");
             NBENE(("Detected an unknown output pixel format (depth: %u) while converting a frame to BGRA. Attempting to guess its type...",
                    frame.r.bpp));

            if (frame.r.bpp == 32)
            {
                conversionType = CV_RGBA2BGRA;
            }
            if (frame.r.bpp == 24)
            {
                conversionType = CV_BGR2BGRA;
            }
            else
            {
                conversionType = CV_BGR5652BGRA;
            }
        }

        cv::cvtColor(input, colorConv, conversionType);
    #else
        (void)frame;
        k_assert(0, "Was asked to convert the frame to BGRA, but OpenCV had been disabled in the build. Can't do it.");
    #endif

    return;
}

// Takes the given image and scales it according to the scaler's current internal
// resolution settings. The scaled image is placed in the scaler's internal buffer,
// not in the source buffer.
//
void ks_scale_frame(const captured_frame_s &frame)
{
    u8 *pixelData = frame.pixels.ptr();
    resolution_s frameRes = frame.r; /// Temp hack. May want to modify the .bpp value.
    resolution_s outputRes = ks_output_resolution();

    const resolution_s minres = kc_hardware().meta.minimum_capture_resolution();
    const resolution_s maxres = kc_hardware().meta.maximum_capture_resolution();

    // Verify that we have a workable frame.
    {
        if (kc_should_current_frame_be_skipped())
        {
            DEBUG(("Skipping a frame, as requested."));
            goto done;
        }
        else if (frame.r.bpp != 16 && frame.r.bpp != 24 && frame.r.bpp != 32)
        {
            NBENE(("Was asked to scale a frame with an incompatible bit depth (%u). Ignoring it.",
                    frame.r.bpp));
            goto done;
        }
        else if (outputRes.w > MAX_OUTPUT_WIDTH ||
                 outputRes.h > MAX_OUTPUT_HEIGHT)
        {
            NBENE(("Was asked to scale a frame with an output size (%u x %u) larger than the maximum allowed (%u x %u). Ignoring it.",
                    outputRes.w, outputRes.h, MAX_OUTPUT_WIDTH, MAX_OUTPUT_HEIGHT));
            goto done;
        }
        else if (pixelData == nullptr)
        {
            NBENE(("Was asked to scale a null frame. Ignoring it."));
            goto done;
        }
        else if (frame.r.bpp != kc_output_color_depth())
        {
            NBENE(("Was asked to scale a frame whose bit depth (%u bits) differed from the expected (%u bits). Ignoring it.",
                   frame.r.bpp, kc_output_color_depth()));
            goto done;
        }
        else if (frame.r.bpp > MAX_OUTPUT_BPP)
        {
            NBENE(("Was asked to scale a frame with a color depth (%u bits) higher than that allowed (%u bits). Ignoring it.",
                   frame.r.bpp, MAX_OUTPUT_BPP));
            goto done;
        }
        else if (frame.r.w < minres.w ||
                 frame.r.h < minres.h)
        {
            NBENE(("Was asked to scale a frame with an input size (%u x %u) smaller than the minimum allowed (%u x %u). Ignoring it.",
                   frame.r.w, frame.r.h, minres.w, minres.h));
            goto done;
        }
        else if (frame.r.w > maxres.w ||
                 frame.r.h > maxres.h)
        {
            NBENE(("Was asked to scale a frame with an input size (%u x %u) larger than the maximum allowed (%u x %u). Ignoring it.",
                   frame.r.w, frame.r.h, maxres.w, maxres.h));
            goto done;
        }
        else if (OUTPUT_BUFFER.is_null())
        {
            goto done;
        }
    }

    // If needed, convert the color data to BGRA, which is what the scaling filters
    // expect to receive. Note that this will only happen if the frame's bit depth
    // doesn't match with the expected value - a frame with the same bit depth but
    // different arrangement of the color channels would not get converted to the
    // proper order.
    if (frame.r.bpp != OUTPUT_BIT_DEPTH)
    {
        s_convert_frame_to_bgra(frame);
        frameRes.bpp = 32;

        pixelData = COLORCONV_BUFFER.ptr();
    }

    // While we have access to the color-converted original frame, and if we've
    // been asked to do so, find out whether the frame is out of alignment with
    // the screen; and if it is, adjust the capture properties to align it.
    if (ALIGN_CAPTURE)
    {
        const auto alignment = kf_find_capture_alignment(pixelData, frameRes);

        kpropagate_capture_alignment_adjust(alignment[0], alignment[1]);

        ALIGN_CAPTURE = false;
    }

    // Perform anti-tearing on the (color-converted) frame. If the user has turned
    // anti-tearing off, this will just return without doing anything.
    pixelData = kat_anti_tear(pixelData, frameRes);
    if (pixelData == nullptr)
    {
        goto done;
    }

    // Apply filtering, and scale the frame.
    {
        kf_apply_filter_chain(pixelData, frameRes);

        // If no need to scale, just copy the data over.
        if ((!FORCE_ASPECT || ASPECT_MODE == aspect_mode_e::native) &&
            frameRes.w == outputRes.w &&
            frameRes.h == outputRes.h)
        {
            memcpy(OUTPUT_BUFFER.ptr(), pixelData, OUTPUT_BUFFER.up_to(frameRes.w * frameRes.h * (frameRes.bpp / 8)));
        }
        else
        {
            const scaling_filter_s *scaler;

            if ((frameRes.w < outputRes.w) ||
                (frameRes.h < outputRes.h))
            {
                scaler = UPSCALE_FILTER;
            }
            else
            {
                scaler = DOWNSCALE_FILTER;
            }

            if (!scaler)
            {
                NBENE(("Upscale or downscale filter is null. Refusing to scale."));

                outputRes = frameRes;
                memcpy(OUTPUT_BUFFER.ptr(), pixelData, OUTPUT_BUFFER.up_to(frameRes.w * frameRes.h * (frameRes.bpp / 8)));
            }
            else
            {
                scaler->scale(pixelData, frameRes, outputRes);
            }
        }
    }

    LATEST_OUTPUT_SIZE = outputRes;

    done:
    return;
}

void ks_set_output_resolution_override_enabled(const bool state)
{
    FORCE_BASE_RESOLUTION = state;
    kd_update_output_window_size();

    return;
}

void ks_set_forced_aspect_enabled(const bool state)
{
    FORCE_ASPECT = state;
    kd_update_output_window_size();

    return;
}

void ks_set_output_base_resolution(const resolution_s &r,
                                   const bool originatesFromUser)
{
    if (FORCE_BASE_RESOLUTION &&
        !originatesFromUser)
    {
        return;
    }

    BASE_RESOLUTION = r;
    kd_update_output_window_size();

    return;
}

real ks_output_scaling(void)
{
    return OUTPUT_SCALING;
}

void ks_set_output_scaling(const real s)
{
    OUTPUT_SCALING = s;
    kd_update_output_window_size();

    return;
}

void ks_set_output_scale_override_enabled(const bool state)
{
    FORCE_SCALING = state;

    kd_update_output_window_size();

    return;
}

void ks_indicate_no_signal(void)
{
    ks_clear_scaler_output_buffer();

    return;
}

void ks_indicate_invalid_signal(void)
{
    ks_clear_scaler_output_buffer();

    return;
}

void ks_clear_scaler_output_buffer(void)
{
    k_assert(!OUTPUT_BUFFER.is_null(),
             "Can't access the output buffer: it was unexpectedly null.");

    memset(OUTPUT_BUFFER.ptr(), 0, OUTPUT_BUFFER.up_to(MAX_FRAME_SIZE));

    return;
}

const u8* ks_scaler_output_as_raw_ptr(void)
{
    return OUTPUT_BUFFER.ptr();
}

// Returns a list of GUI-displayable names of the scaling filters that're
// available.
//
std::vector<std::string> ks_list_of_scaling_filter_names(void)
{
    std::vector<std::string> names;

    for (uint i = 0; i < SCALING_FILTERS.size(); i++)
    {
        names.push_back(SCALING_FILTERS[i].name);
    }

    return names;
}

// Returns a scaling filter matching the given name.
//
const scaling_filter_s* ks_scaler_for_name_string(const std::string &name)
{
    const scaling_filter_s *f = nullptr;

    k_assert(!SCALING_FILTERS.empty(),
             "Could find no scaling filters to search.");

    for (size_t i = 0; i < SCALING_FILTERS.size(); i++)
    {
        if (SCALING_FILTERS[i].name == name)
        {
            f = &SCALING_FILTERS[i];
            goto done;
        }
    }

    f = &SCALING_FILTERS.at(0);
    NBENE(("Was unable to find a scaler called '%s'. "
           "Defaulting to the first scaler on the list (%s).",
           name.c_str(), f->name.c_str()));

    done:
    return f;
}

const std::string& ks_upscaling_filter_name(void)
{
    k_assert(UPSCALE_FILTER != nullptr,
             "Tried to get the name of a null upscale filter.");

    return UPSCALE_FILTER->name;
}

const std::string& ks_downscaling_filter_name(void)
{
    k_assert(UPSCALE_FILTER != nullptr,
             "Tried to get the name of a null downscale filter.")

    return DOWNSCALE_FILTER->name;
}

void ks_set_upscaling_filter(const std::string &name)
{
    UPSCALE_FILTER = ks_scaler_for_name_string(name);

    DEBUG(("Assigned '%s' as the upscaling filter.", UPSCALE_FILTER->name.c_str()));

    return;
}

void ks_set_downscaling_filter(const std::string &name)
{
    DOWNSCALE_FILTER = ks_scaler_for_name_string(name);

    DEBUG(("Assigned '%s' as the downscaling filter.", DOWNSCALE_FILTER->name.c_str()));

    return;
}

#ifdef VALIDATION_RUN
    const u8* ks_VALIDATION_raw_output_buffer_ptr(void)
    {
        return OUTPUT_BUFFER.ptr();
    }
#endif
