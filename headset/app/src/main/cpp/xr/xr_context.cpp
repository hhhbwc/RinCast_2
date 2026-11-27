#include "xr_context.h"

#ifdef __ANDROID__
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <ctime>
#include <unistd.h>

#include "rc/platform.h"

#define XR_CHECK(call)                                                                   \
    do {                                                                                 \
        const XrResult r_ = (call);                                                      \
        if (XR_FAILED(r_)) {                                                             \
            RC_LOGE("%s failed: %s (%s:%d)", #call, XrResultString(r_), __FILE__, __LINE__); \
            return false;                                                                \
        }                                                                                \
    } while (0)

namespace rv {

namespace {

// How hot the headset is, as Android rates it: "none", "light", "moderate", "severe"...;
// "unknown" before Android 11 or without the thermal service. Meta's first answer to heat is
// lowering a refresh rate above 72 Hz to 72.
const char* ThermalStatus() {
#ifdef __ANDROID__
    using Acquire = void* (*)();
    using Status = int (*)(void*);
    static void* lib = dlopen("libandroid.so", RTLD_NOW);
    static auto acquire = lib ? reinterpret_cast<Acquire>(dlsym(lib, "AThermal_acquireManager")) : nullptr;
    static auto status = lib ? reinterpret_cast<Status>(dlsym(lib, "AThermal_getCurrentThermalStatus")) : nullptr;
    static void* manager = acquire ? acquire() : nullptr;
    if (!manager || !status) return "unknown";
    static const char* const kNames[] = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
    const int s = status(manager);
    return s >= 0 && s < int(std::size(kNames)) ? kNames[s] : "unknown";
#else
    return "unknown";
#endif
}

// A developer setting (adb, SideQuest, Meta Quest Developer Hub) that sets the display's
// rate whatever an app asks for. Empty when not set.
std::string RefreshRateOverride() {
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.oculus.refreshRate", value);
    return value;
#else
    return {};
#endif
}

enum class ProjectionOmission { NoRender, EyeLayerOff, NotAcquired, ReleaseFailed };

// EndFrame runs on the render thread. Keep expected omissions (a non-rendering frame or an
// eye layer that is off) out of the warning path, but leave one low-rate summary when they happen.
void NoteProjectionOmission(ProjectionOmission reason) {
    struct Counts {
        uint64_t noRender = 0, eyeLayerOff = 0, notAcquired = 0, releaseFailed = 0;
        int64_t lastLogNs = 0;
    };
    static Counts counts;
    switch (reason) {
        case ProjectionOmission::NoRender: counts.noRender++; break;
        case ProjectionOmission::EyeLayerOff: counts.eyeLayerOff++; break;
        case ProjectionOmission::NotAcquired: counts.notAcquired++; break;
        case ProjectionOmission::ReleaseFailed: counts.releaseFailed++; break;
    }
    const int64_t now = rc::NowNs();
    if (counts.lastLogNs && now - counts.lastLogNs < 5'000'000'000LL) return;
    RC_LOGI("Projection omitted: noRender=%llu eyeLayerOff=%llu notAcquired=%llu releaseFailed=%llu",
            static_cast<unsigned long long>(counts.noRender), static_cast<unsigned long long>(counts.eyeLayerOff),
            static_cast<unsigned long long>(counts.notAcquired), static_cast<unsigned long long>(counts.releaseFailed));
    counts.noRender = counts.eyeLayerOff = counts.notAcquired = counts.releaseFailed = 0;
    counts.lastLogNs = now;
}

}  // namespace

const char* XrResultString(XrResult r) {
    switch (r) {
        case XR_SUCCESS: return "XR_SUCCESS";
        case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
        case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
        case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
        case XR_ERROR_CALL_ORDER_INVALID: return "XR_ERROR_CALL_ORDER_INVALID";
        case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
        case XR_SESSION_LOSS_PENDING: return "XR_SESSION_LOSS_PENDING";
        default: {
            static thread_local char buf[32];
            snprintf(buf, sizeof(buf), "XrResult(%d)", int(r));
            return buf;
        }
    }
}

template <typename T>
static T GetProc(XrInstance instance, const char* name) {
    PFN_xrVoidFunction f = nullptr;
    xrGetInstanceProcAddr(instance, name, &f);
    return reinterpret_cast<T>(f);
}

bool XrContext::HasExtension(const char* name) const {
    return std::find(m_extensions.begin(), m_extensions.end(), name) != m_extensions.end();
}

bool XrContext::InitEgl() {
    m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (m_display == EGL_NO_DISPLAY) {
        RC_LOGE("eglGetDisplay failed: 0x%x", eglGetError());
        return false;
    }
    if (!eglInitialize(m_display, nullptr, nullptr)) {
        RC_LOGE("eglInitialize failed: 0x%x", eglGetError());
        return false;
    }
    const EGLint attribsES3[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_SAMPLES, 0,
                                 EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(m_display, attribsES3, &m_config, 1, &count) || count == 0) {
        RC_LOGW("eglChooseConfig with ES3 failed, retrying with ES2 config bit");
        const EGLint attribsES2[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                     EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_SAMPLES, 0,
                                     EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
        if (!eglChooseConfig(m_display, attribsES2, &m_config, 1, &count) || count == 0) {
            RC_LOGE("eglChooseConfig failed for both ES3 and ES2");
            return false;
        }
    }
    const EGLint ctxAttribs31[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, ctxAttribs31);
    if (m_context == EGL_NO_CONTEXT) {
        RC_LOGW("eglCreateContext ES 3.1 failed, falling back to ES 3.0");
        const EGLint ctxAttribs30[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
        m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, ctxAttribs30);
    }
    if (m_context == EGL_NO_CONTEXT) {
        RC_LOGE("eglCreateContext failed: 0x%x", eglGetError());
        return false;
    }
    const EGLint pbAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    m_surface = eglCreatePbufferSurface(m_display, m_config, pbAttribs);
    if (m_surface == EGL_NO_SURFACE) {
        RC_LOGE("eglCreatePbufferSurface failed: 0x%x", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
        RC_LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        return false;
    }
    RC_LOGI("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return true;
}

bool XrContext::CreateInstance(android_app* app, const std::vector<const char*>& extra) {
    const size_t totalCandidates = xrloader::CandidateCount();
    for (size_t candIdx = 0; candIdx < totalCandidates; candIdx++) {
        if (!xrloader::LoadCandidate(candIdx)) {
            continue;
        }
        RC_LOGI("Trying OpenXR candidate [%zu/%zu]: %s", candIdx + 1, totalCandidates, xrloader::Name());
        m_extensions.clear();

#ifdef __ANDROID__
        auto initLoader = GetProc<PFN_xrInitializeLoaderKHR>(XR_NULL_HANDLE, "xrInitializeLoaderKHR");
        if (initLoader) {
            XrLoaderInitInfoAndroidKHR info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
            info.applicationVM = app ? app->activity->vm : nullptr;
            info.applicationContext = app ? app->activity->clazz : nullptr;
            const XrResult r = initLoader(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&info));
            if (XR_FAILED(r)) {
                RC_LOGW("OpenXR candidate %s: xrInitializeLoaderKHR failed: %s", xrloader::Name(), XrResultString(r));
                xrloader::Unload();
                continue;
            }
        }
#endif

        uint32_t count = 0;
        XrResult rEnum = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
        if (XR_FAILED(rEnum)) {
            RC_LOGW("OpenXR candidate %s: xrEnumerateInstanceExtensionProperties failed: %s", xrloader::Name(), XrResultString(rEnum));
            xrloader::Unload();
            continue;
        }
        std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
        if (count > 0) {
            rEnum = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data());
            if (XR_FAILED(rEnum)) {
                RC_LOGW("OpenXR candidate %s: xrEnumerateInstanceExtensionProperties(data) failed: %s", xrloader::Name(), XrResultString(rEnum));
                xrloader::Unload();
                continue;
            }
        }

        const char* wanted[] = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
#ifdef __ANDROID__
                                XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME,
#else
                                XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,
                                XR_MNDX_EGL_ENABLE_EXTENSION_NAME,
#endif
                                XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME,
                                XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
                                XR_FB_PASSTHROUGH_EXTENSION_NAME,
                                XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME,
                                XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME,
                                XR_FB_COMPOSITION_LAYER_ALPHA_BLEND_EXTENSION_NAME,
                                XR_EXT_HAND_TRACKING_EXTENSION_NAME,
                                XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME,
                                XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME,
                                XR_META_DETACHED_CONTROLLERS_EXTENSION_NAME,
                                XR_EXT_LOCAL_FLOOR_EXTENSION_NAME,
                                XR_FB_COLOR_SPACE_EXTENSION_NAME,
                                XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME};
        std::vector<const char*> all(std::begin(wanted), std::end(wanted));
        all.insert(all.end(), extra.begin(), extra.end());
        std::vector<const char*> enabled;
        for (const char* w : all) {
            if (HasExtension(w)) continue;  // named twice
            for (const auto& p : props) {
                if (strcmp(p.extensionName, w) == 0) {
                    enabled.push_back(w);
                    m_extensions.push_back(w);
                    break;
                }
            }
        }
#ifdef __ANDROID__
        if (!HasExtension(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
            RC_LOGW("OpenXR candidate %s lacks XR_KHR_opengl_es_enable", xrloader::Name());
            xrloader::Unload();
            continue;
        }
#else
        if (!HasExtension(XR_MNDX_EGL_ENABLE_EXTENSION_NAME)) {
            RC_LOGW("OpenXR candidate %s lacks XR_MNDX_egl_enable", xrloader::Name());
            xrloader::Unload();
            continue;
        }
        m_glNotEs = !HasExtension(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
        if (m_glNotEs && !HasExtension(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME)) {
            RC_LOGW("OpenXR candidate %s has neither XR_KHR_opengl_es_enable nor XR_KHR_opengl_enable", xrloader::Name());
            xrloader::Unload();
            continue;
        }
        const char* unused = m_glNotEs ? XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME : XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
        std::erase_if(enabled, [&](const char* e) { return !strcmp(e, unused); });
        std::erase_if(m_extensions, [&](const std::string& e) { return e == unused; });
#endif
        if (m_compatPlainOpenXr) {
            auto isVendorExt = [](const char* name) {
                return strncmp(name, "XR_FB_", 6) == 0 ||
                       strncmp(name, "XR_META_", 8) == 0 ||
                       strncmp(name, "XR_BD_", 6) == 0 ||
                       strncmp(name, "XR_HTC_", 7) == 0;
            };
            std::erase_if(enabled, [&](const char* e) { return isVendorExt(e); });
            std::erase_if(m_extensions, [&](const std::string& e) { return isVendorExt(e.c_str()); });
        }
        m_cylinder = HasExtension(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
        m_layerSettings = HasExtension(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
        m_handTracking = HasExtension(XR_EXT_HAND_TRACKING_EXTENSION_NAME);

        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
#ifdef __ANDROID__
        XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        android.applicationVM = app ? app->activity->vm : nullptr;
        android.applicationActivity = app ? app->activity->clazz : nullptr;
        ci.next = &android;
#else
        (void)app;
#endif
        strcpy(ci.applicationInfo.applicationName, "RinCast");
        ci.applicationInfo.applicationVersion = 1;
        strcpy(ci.applicationInfo.engineName, "RinCast");
        ci.applicationInfo.engineVersion = 1;
        ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        ci.enabledExtensionCount = uint32_t(enabled.size());
        ci.enabledExtensionNames = enabled.data();

        XrInstance instance = XR_NULL_HANDLE;
        XrResult rInst = xrCreateInstance(&ci, &instance);
        if (XR_FAILED(rInst) || instance == XR_NULL_HANDLE) {
            RC_LOGW("OpenXR candidate %s: xrCreateInstance failed: %s", xrloader::Name(), XrResultString(rInst));
            xrloader::Unload();
            continue;
        }
        if (!xrloader::LoadInstance(instance)) {
            RC_LOGW("OpenXR candidate %s: LoadInstance failed", xrloader::Name());
            auto destroyFn = xrDestroyInstance ? xrDestroyInstance
                                               : GetProc<PFN_xrDestroyInstance>(instance, "xrDestroyInstance");
            if (destroyFn) destroyFn(instance);
            xrloader::Unload();
            continue;
        }

        XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system = XR_NULL_SYSTEM_ID;
        XrResult rSys = xrGetSystem(instance, &sgi, &system);
        if (XR_FAILED(rSys)) {
            RC_LOGW("OpenXR candidate %s: xrGetSystem failed: %s", xrloader::Name(), XrResultString(rSys));
            xrDestroyInstance(instance);
            xrloader::Unload();
            continue;
        }

        m_instance = instance;
        m_system = system;
        XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &ip))) m_runtimeName = ip.runtimeName;
        RC_LOGI("OpenXR runtime: %s (loader: %s)", m_runtimeName.c_str(), xrloader::Name());

        if (HasExtension(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME)) {
            m_fromTimespec = GetProc<PFN_xrConvertTimespecTimeToTimeKHR>(m_instance, "xrConvertTimespecTimeToTimeKHR");
            m_toTimespec = GetProc<PFN_xrConvertTimeToTimespecTimeKHR>(m_instance, "xrConvertTimeToTimespecTimeKHR");
        }
        RC_LOGI("OpenXR instance created with %zu extensions", enabled.size());
        return true;
    }

    RC_LOGE("OpenXR: all loader candidates failed to initialize");
    return false;
}

bool XrContext::CreateSession(bool preferStage) {
#ifdef __ANDROID__
    auto getReqs = GetProc<PFN_xrGetOpenGLESGraphicsRequirementsKHR>(m_instance, "xrGetOpenGLESGraphicsRequirementsKHR");
    XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_CHECK(getReqs(m_instance, m_system, &reqs));

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = m_display;
    binding.config = m_config;
    binding.context = m_context;
#else
    // The runtime insists on being asked for its graphics requirements first.
    if (m_glNotEs) {
        auto getReqs = GetProc<PFN_xrGetOpenGLGraphicsRequirementsKHR>(m_instance, "xrGetOpenGLGraphicsRequirementsKHR");
        XrGraphicsRequirementsOpenGLKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
        XR_CHECK(getReqs(m_instance, m_system, &reqs));
    } else {
        auto getReqs = GetProc<PFN_xrGetOpenGLESGraphicsRequirementsKHR>(m_instance, "xrGetOpenGLESGraphicsRequirementsKHR");
        XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        XR_CHECK(getReqs(m_instance, m_system, &reqs));
    }
    XrGraphicsBindingEGLMNDX binding{XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};
    binding.getProcAddress = reinterpret_cast<PFN_xrEglGetProcAddressMNDX>(eglGetProcAddress);
    binding.display = m_display;
    binding.config = m_config;
    binding.context = m_context;
#endif
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = m_system;
    XR_CHECK(xrCreateSession(m_instance, &sci, &m_session));

    m_preferStage = preferStage;
    if (!CreateAppSpace()) return false;
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    rs.poseInReferenceSpace = {};
    rs.poseInReferenceSpace.orientation.w = 1;
    XR_CHECK(xrCreateReferenceSpace(m_session, &rs, &m_viewSpace));

    // Enumerate supported swapchain formats
    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data()));

    bool hasSrgb = false;
    bool hasRgba8 = false;
    for (int64_t f : formats) {
        if (f == GL_SRGB8_ALPHA8) hasSrgb = true;
        if (f == GL_RGBA8) hasRgba8 = true;
    }

    if (hasSrgb) {
        m_colorSwapchainFormat = GL_SRGB8_ALPHA8;
        m_srgbSwapchain = true;
        RC_LOGI("OpenXR Swapchain color format: GL_SRGB8_ALPHA8 (sRGB enabled)");
    } else if (hasRgba8) {
        m_colorSwapchainFormat = GL_RGBA8;
        m_srgbSwapchain = false;
        RC_LOGW("OpenXR Swapchain: GL_SRGB8_ALPHA8 unsupported by runtime, falling back to GL_RGBA8 (non-sRGB)");
    } else if (!formats.empty()) {
        m_colorSwapchainFormat = formats[0];
        m_srgbSwapchain = (m_colorSwapchainFormat == GL_SRGB8_ALPHA8);
        RC_LOGW("OpenXR Swapchain: using runtime preferred format 0x%llx (sRGB=%d)",
                static_cast<unsigned long long>(m_colorSwapchainFormat), m_srgbSwapchain ? 1 : 0);
    } else {
        m_colorSwapchainFormat = GL_SRGB8_ALPHA8;
        m_srgbSwapchain = true;
    }

    // Eye swapchains.
    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                               &viewCount, nullptr));
    if (viewCount < 2) {
        RC_LOGE("OpenXR reported %u primary stereo views; two eye buffers are required", viewCount);
        return false;
    }
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                               viewCount, &viewCount, views.data()));
    if (viewCount < 2 || views.size() < 2) {
        RC_LOGE("OpenXR filled %u of %zu primary stereo views; two eye buffers are required", viewCount, views.size());
        return false;
    }
    for (uint32_t i = 0; i < 2; i++) {
        const XrViewConfigurationView& view = views[i];
        if (view.recommendedImageRectWidth == 0 || view.recommendedImageRectHeight == 0 ||
            view.maxImageRectWidth == 0 || view.maxImageRectHeight == 0) {
            RC_LOGE("OpenXR view %u has an empty image rect: recommended=%ux%u max=%ux%u", i,
                    view.recommendedImageRectWidth, view.recommendedImageRectHeight, view.maxImageRectWidth,
                    view.maxImageRectHeight);
            return false;
        }
    }
    m_recommendedWidth = views[0].recommendedImageRectWidth;
    m_recommendedHeight = views[0].recommendedImageRectHeight;
    // Supersample the UI slightly for crisper text (within runtime limits).
    const int w = int(std::min<float>(views[0].maxImageRectWidth, m_recommendedWidth * 1.2f));
    const int h = int(std::min<float>(views[0].maxImageRectHeight, m_recommendedHeight * 1.2f));
    if (w <= 0 || h <= 0) {
        RC_LOGE("Eye buffer size %dx%d is invalid (recommended %ux%u, max %ux%u)", w, h, m_recommendedWidth,
                m_recommendedHeight, views[0].maxImageRectWidth, views[0].maxImageRectHeight);
        return false;
    }
    for (int eye = 0; eye < 2; eye++) {
        if (!CreateSwapchain(w, h, m_colorSwapchainFormat, 1, true, m_eyes[eye], 1,
                             eye == 0 ? "eye-left" : "eye-right")) {
            for (int previous = 0; previous < eye; previous++) {
                if (!DestroySwapchain(m_eyes[previous]))
                    RC_LOGE("Eye %d swapchain failed and %s could not be released immediately", eye,
                            m_eyes[previous].role.c_str());
            }
            return false;
        }
    }
    RC_LOGI("Eye buffers %dx%d (recommended %ux%u)", w, h, m_recommendedWidth, m_recommendedHeight);

    EnumerateRefreshRates();

    if (HasExtension(XR_FB_COLOR_SPACE_EXTENSION_NAME)) {
        // Content is authored in sRGB/Rec.709; let the runtime convert to the panel.
        auto setCs = GetProc<PFN_xrSetColorSpaceFB>(m_instance, "xrSetColorSpaceFB");
        if (setCs) setCs(m_session, XR_COLOR_SPACE_REC709_FB);
    }

    if (HasExtension(XR_FB_PASSTHROUGH_EXTENSION_NAME)) {
        auto create = GetProc<PFN_xrCreatePassthroughFB>(m_instance, "xrCreatePassthroughFB");
        XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        if (create && XR_SUCCEEDED(create(m_session, &pci, &m_passthrough))) {
            auto createLayer = GetProc<PFN_xrCreatePassthroughLayerFB>(m_instance, "xrCreatePassthroughLayerFB");
            XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
            lci.passthrough = m_passthrough;
            lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
            if (!createLayer || XR_FAILED(createLayer(m_session, &lci, &m_passthroughLayer))) {
                m_passthroughLayer = XR_NULL_HANDLE;
            }
        } else {
            m_passthrough = XR_NULL_HANDLE;
        }
    }
    if (!m_passthroughLayer) {
        uint32_t n = 0;
        xrEnumerateEnvironmentBlendModes(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
        std::vector<XrEnvironmentBlendMode> modes(n);
        if (n) xrEnumerateEnvironmentBlendModes(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, modes.data());
        m_alphaBlend = std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) != modes.end();
        if (m_alphaBlend) RC_LOGI("Passthrough through the alpha-blend environment mode");
    }
    return true;
}

bool XrContext::Init(android_app* app, const std::vector<const char*>& extensions) {
    return InitEgl() && CreateInstance(app, extensions);
}

// Prefer a floor-level space so SteamVR games get correct heights. The local floor follows
// the user: recentering (or the runtime doing it) moves it, and with it everything SteamVR
// sees. The stage stays with the play area, which other tracking systems calibrated against
// the headset (Lighthouse trackers through Space Calibrator) need.
bool XrContext::CreateAppSpace() {
    uint32_t n = 0;
    xrEnumerateReferenceSpaces(m_session, 0, &n, nullptr);
    std::vector<XrReferenceSpaceType> spaces(n);
    xrEnumerateReferenceSpaces(m_session, n, &n, spaces.data());
    auto has = [&](XrReferenceSpaceType t) { return std::find(spaces.begin(), spaces.end(), t) != spaces.end(); };
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    if (m_preferStage && has(XR_REFERENCE_SPACE_TYPE_STAGE))
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    else if (has(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT))
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
    else if (has(XR_REFERENCE_SPACE_TYPE_STAGE))
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    else {
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rs.poseInReferenceSpace.position.y = -1.6f;  // approximate floor
    }
    if (m_appSpace && rs.referenceSpaceType == m_appSpaceType) return true;
    XrSpace space = XR_NULL_HANDLE;
    XR_CHECK(xrCreateReferenceSpace(m_session, &rs, &space));
    std::lock_guard lock(m_spaceMutex);
    if (m_appSpace) {
        xrDestroySpace(m_appSpace);
        m_spaceGeneration++;  // like a recenter: windows are placed again
    }
    m_appSpace = space;
    m_appSpaceType = rs.referenceSpaceType;
    RC_LOGI("App space: %s", m_appSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? "stage" : m_appSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT ? "local floor" : "local");
    return true;
}

void XrContext::SetPreferStage(bool stage) {
    if (!m_session || stage == m_preferStage) return;
    m_preferStage = stage;
    CreateAppSpace();
}

bool XrContext::StartSession(bool preferStage, bool velocitiesFromPoses) {
    m_velocitiesFromPoses = velocitiesFromPoses;
    return CreateSession(preferStage);
}

void XrContext::Shutdown() {
    for (auto& e : m_eyes) DestroySwapchain(e);
    RetryRetiredSwapchains();
    if (m_passthroughLayer) {
        if (auto destroyLayer = GetProc<PFN_xrDestroyPassthroughLayerFB>(m_instance, "xrDestroyPassthroughLayerFB"))
            destroyLayer(m_passthroughLayer);
        m_passthroughLayer = XR_NULL_HANDLE;
    }
    if (m_passthrough) {
        if (auto destroyPt = GetProc<PFN_xrDestroyPassthroughFB>(m_instance, "xrDestroyPassthroughFB"))
            destroyPt(m_passthrough);
        m_passthrough = XR_NULL_HANDLE;
    }
    if (m_appSpace) xrDestroySpace(m_appSpace);
    if (m_viewSpace) xrDestroySpace(m_viewSpace);
    if (m_session) xrDestroySession(m_session);
    if (m_instance) xrDestroyInstance(m_instance);
    if (m_display != EGL_NO_DISPLAY) {
        const auto deleteGl = [](XrSwapchainGl& sc) {
            if (!sc.fbos.empty()) glDeleteFramebuffers(GLsizei(sc.fbos.size()), sc.fbos.data());
            if (sc.depth) glDeleteRenderbuffers(1, &sc.depth);
            sc = {};
        };
        for (auto& e : m_eyes) deleteGl(e);
        for (auto& sc : m_retiredSwapchains) deleteGl(sc);
        m_retiredSwapchains.clear();
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_surface != EGL_NO_SURFACE) eglDestroySurface(m_display, m_surface);
        if (m_context != EGL_NO_CONTEXT) eglDestroyContext(m_display, m_context);
        eglTerminate(m_display);
    }
}

void XrContext::HandleStateChange(XrSessionState state) {
    m_state = state;
    RC_LOGI("Session state %d", int(state));
    if (state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (XR_SUCCEEDED(xrBeginSession(m_session, &bi))) m_running = true;
#ifdef __ANDROID__
        if (HasExtension(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME)) {
            if (auto set = GetProc<PFN_xrSetAndroidApplicationThreadKHR>(m_instance, "xrSetAndroidApplicationThreadKHR"))
                set(m_session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, gettid());
        }
#endif
        // Rate requests before the session runs may be ignored.
        // R2: the swapchain-time enumeration may have run before the session was ready and
        // come back empty (72 Hz fallback). With the session now running, ask once more.
        if (m_running && m_refreshRates.size() <= 1) EnumerateRefreshRates();
        if (m_requestedHz > 0) SetRefreshRate(m_requestedHz);
        SetPerformanceLevels();
    } else if (state == XR_SESSION_STATE_STOPPING) {
        xrEndSession(m_session);
        m_running = false;
    }
}

bool XrContext::PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(m_instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                HandleStateChange(e->state);
                if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) return false;
                break;
            }
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                const auto* e = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);
                const XrVector3f& p = e->poseInPreviousSpace.position;
                RC_LOGI("Reference space %d moved by %.2f m (recentered)%s", int(e->referenceSpaceType),
                        std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z), e->referenceSpaceType == m_appSpaceType ? ", SteamVR's too" : "");
                m_spaceGeneration++;
                break;
            }
            case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
                const auto* e = reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&ev);
                // Away from the rate asked for, without being asked: the headset's own doing.
                m_rateLoweredBySystem = m_targetHz > 0 && std::fabs(e->fromDisplayRefreshRate - m_targetHz) < 0.5f &&
                                        e->toDisplayRefreshRate < m_targetHz - 0.5f;
                if (m_rateLoweredBySystem)
                    RC_LOGW("Refresh rate: the headset lowered it from %.0f to %.0f Hz itself (heat: %s; Meta's first step "
                            "against heat is 72 Hz)", e->fromDisplayRefreshRate, e->toDisplayRefreshRate, ThermalStatus());
                else
                    RC_LOGI("Refresh rate changed from %.0f to %.0f Hz (heat: %s)", e->fromDisplayRefreshRate,
                            e->toDisplayRefreshRate, ThermalStatus());
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                return false;
            default:
                // XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT would land here on newer runtimes.
                break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

void XrContext::RetryRetiredSwapchains() {
    if (m_retiredSwapchains.empty()) return;
    std::erase_if(m_retiredSwapchains, [this](XrSwapchainGl& sc) { return DestroySwapchain(sc); });
}

bool XrContext::CreateSwapchain(int width, int height, int64_t format, int arraySize, bool withDepth,
                                XrSwapchainGl& out, int mipCount, const char* role) {
    RetryRetiredSwapchains();
    XrSwapchainGl created{};
    created.role = role ? role : "swapchain";
    const auto fail = [&](const char* reason) {
        RC_LOGE("Swapchain creation failed: %s role=%s size=%dx%d", reason, created.role.c_str(), width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (created.handle) {
            const XrResult destroyResult = xrDestroySwapchain(created.handle);
            if (XR_FAILED(destroyResult)) {
                RC_LOGE("Swapchain partial-create cleanup failed: %s role=%s result=%d",
                        XrResultString(destroyResult), created.role.c_str(), int(destroyResult));
                m_retiredSwapchains.push_back(std::move(created));
                return false;
            }
            created.handle = XR_NULL_HANDLE;
        }
        if (!created.fbos.empty()) glDeleteFramebuffers(GLsizei(created.fbos.size()), created.fbos.data());
        if (created.depth) glDeleteRenderbuffers(1, &created.depth);
        return false;
    };
    if (width <= 0 || height <= 0 || arraySize <= 0 || mipCount <= 0)
        return fail("invalid swapchain dimensions");
    int64_t chosenFormat = format;
    if (!m_srgbSwapchain && format == GL_SRGB8_ALPHA8) {
        chosenFormat = m_colorSwapchainFormat;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = chosenFormat;
    ci.sampleCount = 1;
    ci.width = uint32_t(width);
    ci.height = uint32_t(height);
    ci.faceCount = 1;
    ci.arraySize = uint32_t(arraySize);
    ci.mipCount = uint32_t(mipCount);
    XrResult result = xrCreateSwapchain(m_session, &ci, &created.handle);
    if (result == XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED && ci.format != GL_RGBA8) {
        RC_LOGW("Swapchain format 0x%llx unsupported, retrying with GL_RGBA8",
                static_cast<unsigned long long>(ci.format));
        ci.format = GL_RGBA8;
        result = xrCreateSwapchain(m_session, &ci, &created.handle);
    }
    if (XR_FAILED(result))
        return fail(rc::StrFormat("xrCreateSwapchain result=%d (%s)", int(result), XrResultString(result)).c_str());
    uint32_t n = 0;
    result = xrEnumerateSwapchainImages(created.handle, 0, &n, nullptr);
    if (XR_FAILED(result))
        return fail(rc::StrFormat("image count result=%d (%s)", int(result), XrResultString(result)).c_str());
    if (!n) return fail("runtime returned zero images");
    // OpenGL and OpenGL ES swapchain images have the same layout, only their type differs.
    std::vector<XrSwapchainImageOpenGLESKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
#ifndef __ANDROID__
    if (m_glNotEs)
        for (auto& i : imgs) i.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
#endif
    result = xrEnumerateSwapchainImages(created.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    if (XR_FAILED(result))
        return fail(rc::StrFormat("image enumeration result=%d (%s)", int(result), XrResultString(result)).c_str());
    if (n != imgs.size()) return fail("runtime changed the swapchain image count during enumeration");
    created.width = width;
    created.height = height;
    if (withDepth) {
        glGenRenderbuffers(1, &created.depth);
        if (!created.depth) return fail("glGenRenderbuffers failed");
        glBindRenderbuffer(GL_RENDERBUFFER, created.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    }
    for (const auto& img : imgs) {
        created.images.push_back(img.image);
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        if (!fbo) return fail("glGenFramebuffers failed");
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, img.image, 0);
        if (withDepth) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, created.depth);
        created.fbos.push_back(fbo);
        const GLenum framebufferStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (framebufferStatus != GL_FRAMEBUFFER_COMPLETE)
            return fail(rc::StrFormat("framebuffer status=%d", int(framebufferStatus)).c_str());
        if (mipCount > 1) {
            glBindTexture(GL_TEXTURE_2D, img.image);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    out = std::move(created);
    m_swapchainImages = n;  // ROUND18 L8-TELEM: how many buffers the runtime queues this swapchain with
    return true;
}

static void WarnSwapchain(const char* operation, const XrSwapchainGl& sc, XrResult result);

bool XrContext::DestroySwapchain(XrSwapchainGl& sc) {
    if (!sc.handle) return true;
    if (sc.imageAcquired && !ReleaseImage(sc)) return false;
    const XrResult result = xrDestroySwapchain(sc.handle);
    if (XR_FAILED(result)) {
        WarnSwapchain("destroy failed", sc, result);
        return false;
    }
    if (!sc.fbos.empty()) glDeleteFramebuffers(GLsizei(sc.fbos.size()), sc.fbos.data());
    if (sc.depth) glDeleteRenderbuffers(1, &sc.depth);
    sc = {};
    return true;
}

static void WarnSwapchain(const char* operation, const XrSwapchainGl& sc, XrResult result) {
    static std::mutex mutex;
    static std::map<std::string, int64_t> lastWarnNs;
    const std::string key = sc.role + ":" + operation;
    std::lock_guard lock(mutex);
    const int64_t now = rc::NowNs();
    auto& last = lastWarnNs[key];
    if (last && now - last < 5'000'000'000LL) return;
    last = now;
    if (XR_SUCCEEDED(result))
        RC_LOGW("Swapchain: %s role=%s images=%zu image=%u outstanding=%d result=%d", operation, sc.role.c_str(),
                sc.fbos.size(), sc.acquired, int(sc.imageAcquired), int(result));
    else
        RC_LOGW("Swapchain %s: %s role=%s images=%zu image=%u outstanding=%d result=%d", operation,
                XrResultString(result), sc.role.c_str(), sc.fbos.size(), sc.acquired, int(sc.imageAcquired), int(result));
}

GLuint XrContext::AcquireImage(XrSwapchainGl& sc) {
    if (!sc.handle || sc.fbos.empty()) return 0;
    if (sc.imageAcquired && !ReleaseImage(sc)) return 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index = 0;
    XrResult result = xrAcquireSwapchainImage(sc.handle, &ai, &index);
    if (XR_FAILED(result)) {
        WarnSwapchain("acquire failed", sc, result);
        return 0;
    }
    sc.acquired = index;
    sc.imageAcquired = true;
    if (index >= sc.fbos.size() || !sc.fbos[index]) {
        ReleaseImage(sc);
        WarnSwapchain("returned an invalid image index", sc, XR_SUCCESS);
        return 0;
    }
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(sc.handle, &wi);
    if (XR_FAILED(result)) {
        ReleaseImage(sc);
        WarnSwapchain("wait failed", sc, result);
        return 0;
    }
    return sc.fbos[index];
}

bool XrContext::ReleaseImage(XrSwapchainGl& sc) {
    if (!sc.handle || !sc.imageAcquired) return true;
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult result = xrReleaseSwapchainImage(sc.handle, &ri);
    if (XR_SUCCEEDED(result)) {
        sc.imageAcquired = false;
        return true;
    }
    WarnSwapchain("release failed", sc, result);
    return false;
}

bool XrContext::LocateSpace(XrSpace space, XrTime time, Pose& pose, Vec3* linVel, Vec3* angVel) {
    XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (linVel || angVel) loc.next = &vel;
    std::lock_guard lock(m_spaceMutex);
    if (!m_appSpace || XR_FAILED(xrLocateSpace(space, m_appSpace, time, &loc))) return false;
    const bool valid = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                       (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    if (!valid) return false;
    pose = SanitizePose(ToPose(loc.pose));
    if ((linVel || angVel) && m_velocitiesFromPoses) {
        // From where it will be a little later, in the app's space like the pose.
        constexpr XrDuration kStep = 5'000'000;
        XrSpaceLocation later{XR_TYPE_SPACE_LOCATION};
        Pose next = pose;
        if (XR_SUCCEEDED(xrLocateSpace(space, m_appSpace, time + kStep, &later)) &&
            (later.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (later.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
            next = SanitizePose(ToPose(later.pose));
        const float dt = float(kStep) * 1e-9f;
        if (linVel) *linVel = (next.position - pose.position) * (1.f / dt);
        if (angVel) {
            Quat d = Normalize(next.orientation * pose.orientation.Conjugate());
            if (d.w < 0) d = {-d.x, -d.y, -d.z, -d.w};
            const float s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            const float angle = 2.f * std::atan2(s, d.w);
            *angVel = s > 1e-6f ? Vec3{d.x, d.y, d.z} * (angle / s / dt) : Vec3{};
        }
        return true;
    }
    if (linVel) *linVel = (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
                              ? Vec3{vel.linearVelocity.x, vel.linearVelocity.y, vel.linearVelocity.z}
                              : Vec3{};
    if (angVel) *angVel = (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
                              ? Vec3{vel.angularVelocity.x, vel.angularVelocity.y, vel.angularVelocity.z}
                              : Vec3{};
    return true;
}

bool XrContext::BeginFrame(FrameState& frame) {
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XR_CHECK(xrWaitFrame(m_session, nullptr, &fs));
    XR_CHECK(xrBeginFrame(m_session, nullptr));
    frame.predictedDisplayTime = fs.predictedDisplayTime;
    frame.period = fs.predictedDisplayPeriod;
    if (fs.predictedDisplayPeriod > 0) {
        m_measuredPeriodNs = fs.predictedDisplayPeriod;
        if (m_compatPlainOpenXr || !HasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
            const float measured = std::clamp(std::round(1e9f / float(m_measuredPeriodNs)), 45.f, 144.f);
            if (m_refreshRates.empty() || std::fabs(m_refreshRates[0] - measured) >= 0.5f) {
                m_refreshRates = {measured};
                RC_LOGI("Measured display refresh rate (no XR_FB_display_refresh_rate): %.0f Hz", measured);
            }
        }
    }
    frame.shouldRender = fs.shouldRender;

    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = m_appSpace;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    const XrResult located = xrLocateViews(m_session, &vli, &vs, 2, &count, views);
    // TASK-A80 M2: the runtime's own field of view is only a trustworthy reading if this call
    // filled the views. It used to be ignored: on a failure the two XrView stay zeroed, e.fov
    // becomes 0/0/0/0, and the app's new fov comparison line would then read as a contract
    // mismatch rather than as what it is - no runtime answer at all. Log only, at most 1 Hz.
    if (XR_FAILED(located) || count < 2) {
        static int64_t s_locateWarnNs = 0;
        const int64_t now = rc::NowNs();
        if (now - s_locateWarnNs > 1'000'000'000LL) {
            s_locateWarnNs = now;
            RC_LOGW("A80-M2 xrLocateViews: %s, %u of 2 views - the runtime fov below is not a measurement",
                    XrResultString(located), count);
        }
    }
    for (int eye = 0; eye < 2; eye++) {
        EyeView& e = frame.eyes[eye];
        e.pose = SanitizePose(ToPose(views[eye].pose));
        e.fov = views[eye].fov;
        e.projection = Mat4::Projection(std::tan(e.fov.angleLeft), std::tan(e.fov.angleRight), std::tan(e.fov.angleUp),
                                        std::tan(e.fov.angleDown), 0.05f, 500.f);
        e.view = Mat4::FromPose(e.pose.Inverse());
    }
    frame.headValid = LocateSpace(m_viewSpace, fs.predictedDisplayTime, frame.head);
    if (!frame.headValid) frame.head = {frame.eyes[0].pose.orientation, (frame.eyes[0].pose.position + frame.eyes[1].pose.position) * 0.5f};
    return true;
}

bool XrContext::BindEye(int eye) {
    const GLuint fbo = AcquireImage(m_eyes[eye]);
    if (!fbo) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, m_eyes[eye].width, m_eyes[eye].height);
    return true;
}

void XrContext::EndFrame(const FrameState& frame, const std::vector<const XrCompositionLayerBaseHeader*>& underlays,
                         bool projectionHasAlpha, const std::vector<const XrCompositionLayerBaseHeader*>& overlays,
                         bool eyeLayer, int displayDeltaFrames) {
    bool projectionReady = false;
    if (eyeLayer) {
        const bool acquired0 = m_eyes[0].imageAcquired;
        const bool acquired1 = m_eyes[1].imageAcquired;
        projectionReady = acquired0 && acquired1;
        const bool released0 = ReleaseImage(m_eyes[0]);
        const bool released1 = ReleaseImage(m_eyes[1]);
        projectionReady = projectionReady && released0 && released1;
        if (!frame.shouldRender) {
            NoteProjectionOmission(ProjectionOmission::NoRender);
        } else if (!acquired0 || !acquired1) {
            NoteProjectionOmission(ProjectionOmission::NotAcquired);
        } else if (!released0 || !released1) {
            NoteProjectionOmission(ProjectionOmission::ReleaseFailed);
        }
    } else {
        ReleaseImage(m_eyes[0]);
        ReleaseImage(m_eyes[1]);
        NoteProjectionOmission(frame.shouldRender ? ProjectionOmission::EyeLayerOff : ProjectionOmission::NoRender);
    }

    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for (int eye = 0; eye < 2; eye++) {
        pv[eye].pose = ToXr(frame.eyes[eye].pose);
        pv[eye].fov = frame.eyes[eye].fov;
        pv[eye].subImage.swapchain = m_eyes[eye].handle;
        pv[eye].subImage.imageRect.extent = {m_eyes[eye].width, m_eyes[eye].height};
    }
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    proj.space = m_appSpace;
    proj.viewCount = 2;
    proj.views = pv;
    if (projectionHasAlpha) proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;

    std::vector<const XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerPassthroughFB pt{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    if (m_passthroughActive && m_passthroughLayer) {
        pt.layerHandle = m_passthroughLayer;
        pt.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&pt));
    }
    if (frame.shouldRender) {
        layers.insert(layers.end(), underlays.begin(), underlays.end());
        if (projectionReady) layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj));
        layers.insert(layers.end(), overlays.begin(), overlays.end());
    }
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    // ROUND19 E1 probe: a negative delta asks the compositor to light this frame that many frame
    // periods earlier than the predicted slot; 0 multiplies out to +0, bit-identical to the old
    // assignment.
    const int delta = std::clamp(displayDeltaFrames, -3, 0);
    fei.displayTime = frame.predictedDisplayTime + (int64_t)delta * frame.period;
    fei.environmentBlendMode = m_passthroughActive && m_alphaBlend && !m_passthroughLayer ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
                                                                                         : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = uint32_t(layers.size());
    fei.layers = layers.data();
    const XrResult r = xrEndFrame(m_session, &fei);
    if (XR_FAILED(r)) RC_LOGW("xrEndFrame: %s", XrResultString(r));
    RetryRetiredSwapchains();
}

float XrContext::RefreshRate() const {
    if (m_compatPlainOpenXr || !HasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
        if (m_measuredPeriodNs > 0) {
            float measured = std::round(1e9f / float(m_measuredPeriodNs));
            return std::clamp(measured, 45.f, 144.f);
        }
        return 72.f;
    }
    auto get = GetProc<PFN_xrGetDisplayRefreshRateFB>(m_instance, "xrGetDisplayRefreshRateFB");
    // 0 when it can't be read: a failed read reported as 72 would send the PC to 72 for nothing.
    float hz = 0;
    if (!get || XR_FAILED(get(m_session, &hz))) {
        if (m_measuredPeriodNs > 0) {
            float measured = std::round(1e9f / float(m_measuredPeriodNs));
            return std::clamp(measured, 45.f, 144.f);
        }
        return 0;
    }
    return hz;
}

// Sustained low, always. It is a range the headset scales within, not a fixed clock: on a Quest
// CPU levels 2 to 4 and GPU 1 to 4, raised as soon as the work needs it. Sustained high pins the
// CPU at 4 and keeps the GPU at 3 or more, the same ceilings with a far higher floor, and the
// headsets ran hot with it while streaming. It is also what a Quest app gets that asks for
// nothing, so this is asked for as soon as the session runs.
void XrContext::SetPerformanceLevels() {
    if (!HasExtension(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME) || !m_running) return;
    auto set = GetProc<PFN_xrPerfSettingsSetPerformanceLevelEXT>(m_instance, "xrPerfSettingsSetPerformanceLevelEXT");
    if (!set) return;
    set(m_session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT);
    set(m_session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT);
    RC_LOGI("Performance level: sustained low");
}

// ROUND20 L17 rework R2: the enumeration the swapchain setup used to inline. The XrResult
// is checked now - a call made before the session is ready can come back empty, and the
// READY handler asks again instead of silently living on the 72 Hz fallback.
void XrContext::EnumerateRefreshRates() {
    if (m_compatPlainOpenXr || !HasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
        if (m_measuredPeriodNs > 0) {
            float measured = std::clamp(std::round(1e9f / float(m_measuredPeriodNs)), 45.f, 144.f);
            m_refreshRates = {measured};
        } else {
            m_refreshRates = {72.f};
        }
        RC_LOGI("Supported refresh rates (measured/fallback): %.0f Hz", m_refreshRates[0]);
        return;
    }
    auto enumRates = GetProc<PFN_xrEnumerateDisplayRefreshRatesFB>(m_instance, "xrEnumerateDisplayRefreshRatesFB");
    if (!enumRates) return;
    uint32_t rc = 0;
    XrResult r = enumRates(m_session, 0, &rc, nullptr);
    if (XR_SUCCEEDED(r) && rc) {
        m_refreshRates.resize(rc);
        r = enumRates(m_session, rc, &rc, m_refreshRates.data());
        m_refreshRates.resize(XR_SUCCEEDED(r) ? rc : 0);
    } else {
        m_refreshRates.clear();
    }
    if (XR_FAILED(r)) RC_LOGW("Refresh rates: enumeration failed (%s)", XrResultString(r));
    std::string list;
    for (float rate : m_refreshRates) list += (list.empty() ? "" : ", ") + std::to_string(int(std::lround(rate)));
    if (m_refreshRates.empty()) RC_LOGW("Refresh rates: the headset listed none, so only 72 Hz is offered");
    else RC_LOGI("Refresh rates: %s Hz", list.c_str());
    if (const std::string forced = RefreshRateOverride(); !forced.empty())
        RC_LOGW("Refresh rate: the developer setting debug.oculus.refreshRate = %s sets the display's rate, "
                "whatever this app asks for", forced.c_str());
    if (m_refreshRates.empty()) m_refreshRates = {72.f};
    // ROUND18 L8-TELEM: one line naming every rate the runtime honors, the 72 Hz fallback included.
    std::string supported;
    for (float rate : m_refreshRates) supported += (supported.empty() ? "" : ", ") + std::to_string(int(std::lround(rate)));
    RC_LOGI("Supported refresh rates: %s Hz", supported.c_str());
}

void XrContext::SetRefreshRate(float hz) {
    if (!HasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) return;
    m_requestedHz = hz;
    // Pick the closest supported rate.
    float best = m_refreshRates.empty() ? hz : m_refreshRates[0];
    for (float r : m_refreshRates)
        if (std::fabs(r - hz) < std::fabs(best - hz)) best = r;
    m_targetHz = best;
    auto set = GetProc<PFN_xrRequestDisplayRefreshRateFB>(m_instance, "xrRequestDisplayRefreshRateFB");
    if (set) {
        const XrResult r = set(m_session, best);
        if (XR_FAILED(r)) RC_LOGW("Refresh rate %.0f Hz refused: %s", best, XrResultString(r));
        else if (std::fabs(best - hz) >= 0.5f) {
            // R2 evidence: a clamped request names the whole supported list, so a logcat
            // tail alone answers "does this runtime offer the rate at all".
            std::string list;
            for (float rate : m_refreshRates) list += (list.empty() ? "" : ", ") + std::to_string(int(std::lround(rate)));
            RC_LOGW("Refresh rate: %.0f Hz asked for but only [%s] Hz supported, using %.0f Hz (now %.0f Hz)",
                    hz, list.c_str(), best, RefreshRate());
        } else RC_LOGI("Refresh rate: %.0f Hz asked for, %.0f Hz now", best, RefreshRate());
    }
}

// A request the runtime took but never applied leaves the display at its default 72 Hz while
// the PC renders at the rate asked for: 90 frames a second into 72 shows four in five, a
// stutter whenever the view moves. Look every two seconds and ask again.
void XrContext::KeepRefreshRate() {
    if (!m_running || m_targetHz <= 0) return;
    const int64_t now = rc::NowNs();
    // Lowered by the headset itself (heat): it gives the rate back when it cools down, so ask
    // rarely; asking every two seconds only fills the log.
    const int64_t interval = m_rateLoweredBySystem ? 30'000'000'000LL : 2'000'000'000LL;
    if (now - m_refreshCheckNs < interval) return;
    m_refreshCheckNs = now;
    const float hz = RefreshRate();
    if (hz <= 0 || std::fabs(hz - m_targetHz) < 0.5f) {
        if (hz > 0) m_rateLoweredBySystem = false;
        return;
    }
    auto set = GetProc<PFN_xrRequestDisplayRefreshRateFB>(m_instance, "xrRequestDisplayRefreshRateFB");
    const XrResult r = set ? set(m_session, m_targetHz) : XR_ERROR_FUNCTION_UNSUPPORTED;
    const std::string forced = RefreshRateOverride();
    RC_LOGW("Refresh rate %.0f Hz instead of %.0f, asked again: %s (heat: %s%s%s)", hz, m_targetHz, XrResultString(r),
            ThermalStatus(), forced.empty() ? "" : "; developer setting debug.oculus.refreshRate = ", forced.c_str());
}

void XrContext::SetPassthrough(bool enable) {
    if (enable == m_passthroughActive) return;
    if (!m_passthroughLayer && m_alphaBlend) {  // nothing to start: the frame's blend mode says it
        m_passthroughActive = enable;
        return;
    }
    if (!m_passthrough || !m_passthroughLayer) return;
    if (enable) {
        if (auto start = GetProc<PFN_xrPassthroughStartFB>(m_instance, "xrPassthroughStartFB")) start(m_passthrough);
        if (auto resume = GetProc<PFN_xrPassthroughLayerResumeFB>(m_instance, "xrPassthroughLayerResumeFB")) resume(m_passthroughLayer);
    } else {
        if (auto pauseLayer = GetProc<PFN_xrPassthroughLayerPauseFB>(m_instance, "xrPassthroughLayerPauseFB")) pauseLayer(m_passthroughLayer);
        if (auto pause = GetProc<PFN_xrPassthroughPauseFB>(m_instance, "xrPassthroughPauseFB")) pause(m_passthrough);
    }
    m_passthroughActive = enable;
}

int64_t XrContext::ToMonotonicNs(XrTime t) const {
    if (m_toTimespec) {
        timespec ts{};
        if (XR_SUCCEEDED(m_toTimespec(m_instance, t, &ts))) return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
    }
    return t;
}

XrTime XrContext::FromMonotonicNs(int64_t ns) const {
    if (m_fromTimespec) {
        timespec ts{time_t(ns / 1000000000LL), long(ns % 1000000000LL)};
        XrTime t = 0;
        if (XR_SUCCEEDED(m_fromTimespec(m_instance, &ts, &t))) return t;
    }
    return ns;
}

}  // namespace rv
