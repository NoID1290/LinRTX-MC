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

typedef enum LinRtxNeuralEngineMode {
    LINRTX_NEURAL_OFF = 0,           /* Disabled */
    LINRTX_NEURAL_AUTO = 1,          /* Try NGX Feature 18 (DLSSNR) with RTX unlock, fallback to Vulkan Neural Filter */
    LINRTX_NEURAL_NGX_ONLY = 2,      /* NVIDIA NGX Feature 18 (libnvidia-ngx-dlssnr.so) only */
    LINRTX_NEURAL_VULKAN_ONLY = 3    /* Built-in Vulkan Compute Neural Engine Filter (unlocked for any RTX GPU) */
} LinRtxNeuralEngineMode;

typedef enum LinRtxNeuralStyle {
    LINRTX_NEURAL_STYLE_DEFAULT = 0,   /* Balanced faithful neural enhancement */
    LINRTX_NEURAL_STYLE_NATURAL = 1,   /* Natural organic lighting & material vibrancy */
    LINRTX_NEURAL_STYLE_CINEMATIC = 2  /* Filmic local contrast, deep shadows & crisp structure */
} LinRtxNeuralStyle;

typedef enum LinRtxNeuralPreset {
    LINRTX_NEURAL_PRESET_AUTO = 0,      /* Driver / Engine Auto */
    LINRTX_NEURAL_PRESET_QUALITY = 1,   /* Quality (WEIGHTS_HT / Full 5x5 Joint-Bilateral + Temporal) */
    LINRTX_NEURAL_PRESET_SMALL_FP8 = 2  /* Fast Small FP8 (WEIGHTS_CC_SMALL_FP8 / 3x3 Fast Kernel) */
} LinRtxNeuralPreset;

typedef enum LinRtxNeuralPlacement {
    LINRTX_NEURAL_POST_SR = 0,       /* Display-res World (after DLSS/CAS, before HUD) */
    LINRTX_NEURAL_PRE_SR = 1         /* Render-res World (before DLSS-SR upscaling) */
} LinRtxNeuralPlacement;

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
    const char* neuralSearchDirs;  /* Semicolon/colon separated directories to search for libnvidia-ngx-dlssnr.so */
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

    /* DLSS 5 Neural Engine Filter (DLSS-NR / NGX Feature 18 + Unlocked Vulkan Compute) */
    int32_t neuralMode;                  /* LinRtxNeuralEngineMode (600) */
    int32_t neuralStyle;                 /* LinRtxNeuralStyle: 0=Default, 1=Natural, 2=Cinematic (604) */
    int32_t neuralPreset;                /* LinRtxNeuralPreset: 0=Auto, 1=Quality, 2=Small FP8 (608) */
    int32_t neuralPlacement;             /* LinRtxNeuralPlacement: 0=Post-SR, 1=Pre-SR (612) */
    int32_t neuralUnlockAnyRtx;          /* 1 = Unlock across RTX 20/30/40/50 (616) */
    int32_t neuralUseAutoMask;           /* 1 = DLSSNR.UseAutoMask heuristic protection (620) */
    float neuralWorkingScale;            /* 0.50 .. 1.00 cost-scaler working scale (624) */
    float neuralIntensity;               /* 0.00 .. 2.00 DLSSNR.Intensity (628) */
    float neuralLocalToneStrength;       /* 0.00 .. 2.00 DLSSNR.LocalToneStrength (632) */
    float neuralLocalStructureStrength;  /* 0.00 .. 2.00 DLSSNR.LocalStructureStrength (636) */
    float neuralSkinStructureStrength;   /* -1.00 .. 2.00 DLSSNR.SkinStructureStrength (-1=Auto) (640) */
    uint32_t neuralReserved0;            /* Tail alignment pad (644..648) */
} LinRtxFrameConfig;

#define LINRTX_FRAME_FLAG_PRETONEMAP_RR_ACTIVE (1u << 0)

/**
 * Standalone pre-tonemap HDR GPU denoiser / Ray Reconstruction invocation
 * (used for Iris path-traced radiance buffers such as Camellia colortex5 prior to bloom & tonemapping).
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

    /* Extended Pre-Tonemap DLSS-RR (DLSSD 1:1) + F1 progressive sample convergence (offset 96..504) */
    int32_t rrMode;                   /* LinRtxRrMode: 0=OFF, 1=AUTO, 2=ON (96) */
    int32_t rrPreset;                 /* LinRtxRrPreset (100) */
    int32_t denoiseMode;              /* LinRtxDenoiseMode: 0=OFF, 1=SVGF_FAST, 2=SVGF_ATROUS (104) */
    uint32_t inputsTopLeftVk;         /* 1=depth/mvec/albedo/normals are Vulkan top-left; hdrColor/outHdrColor stay GL bottom-up (108) */
    uint32_t f1AccumActive;           /* 1=F1 / hideGUI sample-count convergence active (112) */
    float f1FadeSamples;              /* 0=stay 100% denoised; >1=fade toward raw colortex5.rgb by N samples (116) */
    float jitterOffsetX;              /* (120) */
    float jitterOffsetY;              /* (124) */
    float cameraFovRadians;           /* (128) */
    float cameraAspect;               /* (132) */
    float cameraPos[3];               /* (136) */
    float cameraUp[3];                /* (148) */
    float cameraRight[3];             /* (160) */
    float cameraFwd[3];               /* (172) */
    float viewToClip[16];             /* (184) */
    float clipToView[16];             /* (248) */
    float clipToPrevClip[16];         /* (312) */
    float prevClipToClip[16];         /* (376) */
    float worldToView[16];            /* (440..504) */
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

    /* DLSS 5 Neural Engine Filter diagnostics (offset 1104..1352) */
    int32_t dlssnrNgxLoaded;       /* offset 1104 */
    int32_t dlssnrNgxSupported;    /* offset 1108 */
    int32_t dlssnrActive;          /* offset 1112 */
    int32_t dlssnrUsingNgx;        /* offset 1116 */
    int32_t dlssnrUnlocked;        /* offset 1120 */
    int32_t activeNeuralPreset;    /* offset 1124 */
    char gpuArchName[32];          /* offset 1128..1160 */
    char dlssnrVersion[64];        /* offset 1160..1224 */
    char dlssnrStatus[128];        /* offset 1224..1352 */
} LinRtxStatus;

#ifdef __cplusplus
}
#endif

#endif /* LINRTX_TYPES_H */
