#ifndef LINRTX_TYPES_H
#define LINRTX_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum LinRtxDlssMode {
    LINRTX_DLSS_OFF = 0,
    LINRTX_DLSS_DLAA = 1,
    LINRTX_DLSS_QUALITY = 2,
    LINRTX_DLSS_BALANCED = 3,
    LINRTX_DLSS_PERFORMANCE = 4,
    LINRTX_DLSS_ULTRA_PERFORMANCE = 5,
    LINRTX_DLSS_CUSTOM = 6
} LinRtxDlssMode;

typedef enum LinRtxDlssPreset {
    LINRTX_PRESET_DEFAULT = 0, /* Auto / Driver Default */
    LINRTX_PRESET_E = 5,       /* Legacy CNN */
    LINRTX_PRESET_F = 6,       /* Legacy CNN */
    LINRTX_PRESET_J = 10,      /* DLSS 4 Transformer (lower ghosting) */
    LINRTX_PRESET_K = 11,      /* DLSS 4 Transformer (recommended DLAA/Quality) */
    LINRTX_PRESET_L = 12,      /* DLSS 4.5 2nd-Gen Transformer (Ultra Perf / sharp) */
    LINRTX_PRESET_M = 13       /* DLSS 4.5 2nd-Gen Transformer (Perf / balanced) */
} LinRtxDlssPreset;

typedef enum LinRtxFgMode {
    LINRTX_FG_OFF = 0,
    LINRTX_FG_2X = 1,
    LINRTX_FG_3X = 2,
    LINRTX_FG_4X = 3
} LinRtxFgMode;

typedef enum LinRtxHdrEngine {
    LINRTX_HDR_OFF = 0,
    LINRTX_HDR_AUTO = 1,       /* Try NVIDIA TrueHDR NGX, fall back to GPU compute inverse-tonemap */
    LINRTX_HDR_TRUEHDR_ONLY = 2,
    LINRTX_HDR_SHADER_ONLY = 3
} LinRtxHdrEngine;

typedef enum LinRtxHdrOutputMode {
    LINRTX_HDR_OUT_SDR_BOOST = 0,    /* Inverse-tonemapped HDR expanded to SDR display / shader-friendly */
    LINRTX_HDR_OUT_PQ_BT2020 = 1,    /* RGB10A2 BT.2020 ST.2084 PQ (gamescope --hdr-enabled / Vulkan HDR10) */
    LINRTX_HDR_OUT_SCRGB_FP16 = 2    /* Linear scRGB FP16 (Vulkan HDR / FP16 pipeline) */
} LinRtxHdrOutputMode;

typedef enum LinRtxPresentBackend {
    LINRTX_PRESENT_OPENGL = 0,       /* Blit/swap through Minecraft GLFW OpenGL backbuffer */
    LINRTX_PRESENT_VULKAN_HDR = 1    /* Experimental child-X11 / Wayland-subsurface Vulkan HDR10 swapchain */
} LinRtxPresentBackend;

typedef enum LinRtxDenoiseMode {
    LINRTX_DENOISE_OFF = 0,          /* Disabled */
    LINRTX_DENOISE_SVGF_ATROUS = 1,  /* Spatio-temporal variance-guided + 4-pass Atrous filter */
    LINRTX_DENOISE_SVGF_FAST = 2     /* Fast temporal + 2-pass Atrous filter */
} LinRtxDenoiseMode;

typedef enum LinRtxRrMode {
    LINRTX_RR_OFF = 0,               /* Standard DLSS-SR (or SVGF + DLSS-SR) */
    LINRTX_RR_ON = 1,                /* Enable NVIDIA DLSS Ray Reconstruction (DLSS-RR / DLSSD) */
    LINRTX_RR_AUTO = 2               /* Use DLSS-RR when active shaderpack provides G-buffer/PT cues, else DLSS-SR */
} LinRtxRrMode;

typedef enum LinRtxRrPreset {
    LINRTX_RR_PRESET_DEFAULT = 0,    /* Driver Default Transformer RR */
    LINRTX_RR_PRESET_D = 4,          /* Preset D (Transformer Ray Reconstruction) */
    LINRTX_RR_PRESET_E = 5,          /* Preset E (Transformer Ray Reconstruction, balanced/sharp) */
    LINRTX_RR_PRESET_F = 6           /* Preset F (Transformer Ray Reconstruction, temporal stability) */
} LinRtxRrPreset;

/**
 * Runtime configuration passed from Java to native C++ engine.
 * Every field is naturally aligned (4 or 8 bytes) for deterministic Java FFM layout.
 */
typedef struct LinRtxInitConfig {
    uint32_t structSize;
    uint32_t enableValidation;
    uint64_t appId;
    uint64_t windowHandle;         /* GLFW window handle or native X11/Wayland pointers below */
    uint64_t nativeDisplayPtr;     /* Display* (X11) or wl_display* (Wayland), 0 if unavailable */
    uint64_t nativeWindowHandle;   /* Window XID (X11) or wl_surface* (Wayland), 0 if unavailable */
    uint32_t isWayland;
    uint32_t reserved0;
    const char* ngxLibDir;         /* Directory holding extracted libnvidia-ngx-dlss*.so */
    const char* trueHdrSearchDirs; /* Semicolon/colon separated directories to search for libnvidia-ngx-truehdr.so */
    const char* logDir;            /* Directory for NGX logs */
} LinRtxInitConfig;

/**
 * Per-frame rendering parameters & input/output GL textures.
 */
typedef struct LinRtxFrameConfig {
    uint32_t structSize;
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t displayWidth;
    uint32_t displayHeight;

    /* Feature modes & live presets */
    int32_t dlssMode;              /* LinRtxDlssMode */
    int32_t dlssPreset;            /* LinRtxDlssPreset */
    int32_t fgMode;                /* LinRtxFgMode */
    int32_t hdrEngine;             /* LinRtxHdrEngine */
    int32_t hdrOutputMode;         /* LinRtxHdrOutputMode */
    int32_t presentBackend;        /* LinRtxPresentBackend */

    /* Quality & HDR tunables */
    float sharpness;               /* 0.0 .. 1.0 */
    float jitterOffsetX;           /* Low-res pixel space */
    float jitterOffsetY;           /* Low-res pixel space */
    float mvScaleX;
    float mvScaleY;
    float frameTimeMs;
    int32_t resetHistory;
    int32_t depthInverted;         /* 1 for Reverse-Z (MC 26.2 default), 0 for standard */
    int32_t colorIsHdrInput;       /* 1 if shaderpack outputs FP16 HDR into color texture */
    int32_t useHudlessSplit;       /* 1 if hudlessTexGlId is supplied separately from backbuffer */

    /* TrueHDR / RTX HDR shader tunables */
    uint32_t hdrContrast;          /* 0..200 (default 100) */
    uint32_t hdrSaturation;        /* 0..200 (default 100) */
    uint32_t hdrMiddleGray;        /* 10..100 (default 50) */
    uint32_t hdrMaxLuminance;      /* 400..2000 nits (default 650) */
    uint32_t hdrDebandingOff;      /* 0 or 1 */
    float hdrHighlightBoost;       /* 0.0 .. 2.0 extra specular/bloom peak lift */

    /* OpenGL texture IDs (owned by Java/MC or TargetManager) */
    uint32_t colorInTexGlId;       /* Low-res world color (Y-flipped top-left OR bottom-up per inputFlipped) */
    uint32_t depthInTexGlId;       /* Low-res merged depth GL_R32F */
    uint32_t mvecInTexGlId;        /* Low-res 2D motion vectors GL_RG16F */
    uint32_t outUpscaledTexGlId;   /* Output full-res DLSS GL_RGBA8/GL_RGBA16F (only if copying back to GL) */
    uint32_t hudlessInTexGlId;     /* Full-res world without HUD */
    uint32_t backbufferInTexGlId;  /* Full-res world + HUD */
    uint32_t outHdrOrFgTexGlId;    /* Final full-res GL texture after HDR / current real frame */
    uint32_t outInterpTexGlId;     /* Generated frame GL texture (GL_RGBA8 / GL_RGB10_A2 / GL_RGBA16F) */
    uint32_t inputsAlreadyTopLeft; /* 1 if GL shaders already stored Vulkan top-left orientation */

    /* Camera metadata for DLSS-G optical flow stability */
    float cameraNear;
    float cameraFar;
    float cameraFovRadians;
    float cameraAspect;
    float cameraPos[3];
    float cameraUp[3];
    float cameraRight[3];
    float cameraFwd[3];
    float viewToClip[16];          /* Unjittered column-major JOML 4x4 */
    float clipToView[16];          /* Unjittered inverse projection */
    float clipToPrevClip[16];      /* Unjittered current clip -> prev clip */
    float prevClipToClip[16];      /* Inverse of clipToPrevClip */

    /* Denoiser & DLSS Ray Reconstruction parameters (offset 464, natural 4/8-byte aligned) */
    int32_t denoiseMode;               /* LinRtxDenoiseMode (464) */
    int32_t rrMode;                    /* LinRtxRrMode (468) */
    int32_t rrPreset;                  /* LinRtxRrPreset (472) */
    int32_t useGbufferGuides;          /* 1 if diffuse/normals/specular/roughness guides supplied (476) */
    int32_t gbufferNormalsAreWorld;    /* 1 if normals tex holds world-space normals (480) */
    float denoiseTemporalBlend;        /* 0.0 .. 0.96 max history weight (484) */
    float denoiseStrength;             /* 0.25 .. 2.5 spatial filter sensitivity (488) */
    uint32_t denoiseAtrousPasses;      /* 1 .. 5 Atrous wavelet passes (492) */
    uint32_t diffuseAlbedoInTexGlId;   /* Low-res diffuse albedo GL tex (496) */
    uint32_t diffuseAlbedoFormatGl;    /* (500) */
    uint32_t specularAlbedoInTexGlId;  /* Low-res specular albedo GL tex (504) */
    uint32_t specularAlbedoFormatGl;   /* (508) */
    uint32_t normalsInTexGlId;         /* Low-res normals GL tex (512) */
    uint32_t normalsFormatGl;          /* (516) */
    uint32_t roughnessInTexGlId;       /* Low-res roughness GL tex (520) */
    uint32_t roughnessFormatGl;        /* (524) */
    uint32_t flagsReserved0;           /* Reserved flags (528) */
    float worldToView[16];             /* Row-major world->view 4x4 matrix for DLSS-RR (532..596) */
    uint32_t flagsReserved1;           /* Tail alignment pad (596..600) */
} LinRtxFrameConfig;

/**
 * Standalone pre-tonemap HDR GPU denoiser invocation (used for Iris path-traced
 * radiance buffers such as Camellia colortex5 prior to bloom & tonemapping).
 */
typedef struct LinRtxDenoiseConfig {
    uint32_t structSize;
    uint32_t width;
    uint32_t height;
    uint32_t resetHistory;
    uint32_t hdrColorTexGlId;
    uint32_t hdrColorFormatGl;
    uint32_t outHdrColorTexGlId;
    uint32_t outHdrColorFormatGl;
    uint32_t depthTexGlId;
    uint32_t depthFormatGl;
    uint32_t mvecTexGlId;
    uint32_t mvecFormatGl;
    uint32_t diffuseAlbedoTexGlId;
    uint32_t diffuseAlbedoFormatGl;
    uint32_t normalsTexGlId;
    uint32_t normalsFormatGl;
    float temporalBlend;
    float denoiseStrength;
    uint32_t atrousPasses;
    uint32_t preserveAlpha;
    uint32_t demodulateAlbedo;
    uint32_t normalsAreWorld;
    float cameraNear;
    float cameraFar;
} LinRtxDenoiseConfig;

/**
 * Runtime diagnostics returned to Java UI & telemetry overlay.
 */
typedef struct LinRtxStatus {
    uint32_t structSize;
    int32_t vkReady;
    int32_t ngxReady;
    int32_t dlssSupported;
    int32_t dlssActive;
    int32_t dlssgSupported;
    int32_t dlssgActive;
    int32_t trueHdrNgxLoaded;
    int32_t trueHdrActive;
    int32_t hdrShaderActive;
    int32_t vkSwapchainActive;
    int32_t vkSwapchainHdr10;
    int32_t maxFgMultiplier;       /* 1 if unsupported, 2 for Ada, 4 for Blackwell */
    int32_t activeDlssPreset;
    int32_t lastErrorCode;
    uint32_t generatedFrameCount;  /* 0 or 1+ generated frames written into outInterpTexGlId */
    char gpuName[128];
    char driverVersion[64];
    char dlssVersion[64];
    char dlssgVersion[64];
    char trueHdrStatus[128];
    char swapchainStatus[128];
    char lastErrorMsg[256];
    int32_t dlssdSupported;        /* offset 896 */
    int32_t dlssdActive;           /* offset 900 */
    int32_t denoiserActive;        /* offset 904 */
    int32_t activeRrPreset;        /* offset 908 */
    char dlssdVersion[64];         /* offset 912..976 */
    char denoiserStatus[128];      /* offset 976..1104 */
} LinRtxStatus;

#ifdef __cplusplus
}
#endif

#endif /* LINRTX_TYPES_H */
