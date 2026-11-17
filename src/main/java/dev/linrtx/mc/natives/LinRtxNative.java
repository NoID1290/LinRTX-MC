package dev.linrtx.mc.natives;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;

/**
 * Zero-allocation Java 25 Foreign Function & Memory (FFM) bindings for liblinrtx.so.
 * Matches linrtx_types.h / linrtx_api.h layouts verified by native C++ test_linrtx_abi.
 */
public final class LinRtxNative {
    private static final Logger LOGGER = LoggerFactory.getLogger("LinRTX");

    // LinRtxInitConfig (72 bytes)
    public static final StructLayout INIT_CONFIG_LAYOUT = MemoryLayout.structLayout(
        ValueLayout.JAVA_INT.withName("structSize"),            // 0
        ValueLayout.JAVA_INT.withName("enableValidation"),      // 4
        ValueLayout.JAVA_LONG.withName("appId"),                // 8
        ValueLayout.JAVA_LONG.withName("windowHandle"),         // 16
        ValueLayout.JAVA_LONG.withName("nativeDisplayPtr"),     // 24
        ValueLayout.JAVA_LONG.withName("nativeWindowHandle"),   // 32
        ValueLayout.JAVA_INT.withName("isWayland"),             // 40
        ValueLayout.JAVA_INT.withName("reserved0"),             // 44
        ValueLayout.ADDRESS.withName("ngxLibDir"),              // 48
        ValueLayout.ADDRESS.withName("trueHdrSearchDirs"),      // 56
        ValueLayout.ADDRESS.withName("logDir")                  // 64
    );

    // LinRtxFrameConfig (464 bytes)
    public static final StructLayout FRAME_CONFIG_LAYOUT = MemoryLayout.structLayout(
        ValueLayout.JAVA_INT.withName("structSize"),            // 0
        ValueLayout.JAVA_INT.withName("renderWidth"),           // 4
        ValueLayout.JAVA_INT.withName("renderHeight"),          // 8
        ValueLayout.JAVA_INT.withName("displayWidth"),          // 12
        ValueLayout.JAVA_INT.withName("displayHeight"),         // 16
        ValueLayout.JAVA_INT.withName("dlssMode"),              // 20
        ValueLayout.JAVA_INT.withName("dlssPreset"),            // 24
        ValueLayout.JAVA_INT.withName("fgMode"),                // 28
        ValueLayout.JAVA_INT.withName("hdrEngine"),             // 32
        ValueLayout.JAVA_INT.withName("hdrOutputMode"),         // 36
        ValueLayout.JAVA_INT.withName("presentBackend"),        // 40
        ValueLayout.JAVA_FLOAT.withName("sharpness"),           // 44
        ValueLayout.JAVA_FLOAT.withName("jitterOffsetX"),       // 48
        ValueLayout.JAVA_FLOAT.withName("jitterOffsetY"),       // 52
        ValueLayout.JAVA_FLOAT.withName("mvScaleX"),            // 56
        ValueLayout.JAVA_FLOAT.withName("mvScaleY"),            // 60
        ValueLayout.JAVA_FLOAT.withName("frameTimeMs"),         // 64
        ValueLayout.JAVA_INT.withName("resetHistory"),          // 68
        ValueLayout.JAVA_INT.withName("depthInverted"),         // 72
        ValueLayout.JAVA_INT.withName("colorIsHdrInput"),       // 76
        ValueLayout.JAVA_INT.withName("useHudlessSplit"),       // 80
        ValueLayout.JAVA_INT.withName("hdrContrast"),           // 84
        ValueLayout.JAVA_INT.withName("hdrSaturation"),         // 88
        ValueLayout.JAVA_INT.withName("hdrMiddleGray"),         // 92
        ValueLayout.JAVA_INT.withName("hdrMaxLuminance"),       // 96
        ValueLayout.JAVA_INT.withName("hdrDebandingOff"),       // 100
        ValueLayout.JAVA_FLOAT.withName("hdrHighlightBoost"),   // 104
        ValueLayout.JAVA_INT.withName("colorInTexGlId"),        // 108
        ValueLayout.JAVA_INT.withName("depthInTexGlId"),        // 112
        ValueLayout.JAVA_INT.withName("mvecInTexGlId"),         // 116
        ValueLayout.JAVA_INT.withName("outUpscaledTexGlId"),    // 120
        ValueLayout.JAVA_INT.withName("hudlessInTexGlId"),      // 124
        ValueLayout.JAVA_INT.withName("backbufferInTexGlId"),   // 128
        ValueLayout.JAVA_INT.withName("outHdrOrFgTexGlId"),     // 132
        ValueLayout.JAVA_INT.withName("outInterpTexGlId"),      // 136
        ValueLayout.JAVA_INT.withName("inputsAlreadyTopLeft"),  // 140
        ValueLayout.JAVA_FLOAT.withName("cameraNear"),          // 144
        ValueLayout.JAVA_FLOAT.withName("cameraFar"),           // 148
        ValueLayout.JAVA_FLOAT.withName("cameraFovRadians"),    // 152
        ValueLayout.JAVA_FLOAT.withName("cameraAspect"),        // 156
        MemoryLayout.sequenceLayout(3, ValueLayout.JAVA_FLOAT).withName("cameraPos"),      // 160
        MemoryLayout.sequenceLayout(3, ValueLayout.JAVA_FLOAT).withName("cameraUp"),       // 172
        MemoryLayout.sequenceLayout(3, ValueLayout.JAVA_FLOAT).withName("cameraRight"),    // 184
        MemoryLayout.sequenceLayout(3, ValueLayout.JAVA_FLOAT).withName("cameraFwd"),      // 196
        MemoryLayout.sequenceLayout(16, ValueLayout.JAVA_FLOAT).withName("viewToClip"),    // 208
        MemoryLayout.sequenceLayout(16, ValueLayout.JAVA_FLOAT).withName("clipToView"),    // 272
        MemoryLayout.sequenceLayout(16, ValueLayout.JAVA_FLOAT).withName("clipToPrevClip"),// 336
        MemoryLayout.sequenceLayout(16, ValueLayout.JAVA_FLOAT).withName("prevClipToClip"),// 400
        ValueLayout.JAVA_INT.withName("denoiseMode"),           // 464
        ValueLayout.JAVA_INT.withName("rrMode"),                // 468
        ValueLayout.JAVA_INT.withName("rrPreset"),              // 472
        ValueLayout.JAVA_INT.withName("useGbufferGuides"),      // 476
        ValueLayout.JAVA_INT.withName("gbufferNormalsAreWorld"),// 480
        ValueLayout.JAVA_FLOAT.withName("denoiseTemporalBlend"),// 484
        ValueLayout.JAVA_FLOAT.withName("denoiseStrength"),     // 488
        ValueLayout.JAVA_INT.withName("denoiseAtrousPasses"),   // 492
        ValueLayout.JAVA_INT.withName("diffuseAlbedoInTexGlId"),// 496
        ValueLayout.JAVA_INT.withName("diffuseAlbedoFormatGl"), // 500
        ValueLayout.JAVA_INT.withName("specularAlbedoInTexGlId"),// 504
        ValueLayout.JAVA_INT.withName("specularAlbedoFormatGl"),// 508
        ValueLayout.JAVA_INT.withName("normalsInTexGlId"),      // 512
        ValueLayout.JAVA_INT.withName("normalsFormatGl"),       // 516
        ValueLayout.JAVA_INT.withName("roughnessInTexGlId"),    // 520
        ValueLayout.JAVA_INT.withName("roughnessFormatGl"),     // 524
        ValueLayout.JAVA_INT.withName("flagsReserved0"),        // 528
        MemoryLayout.sequenceLayout(16, ValueLayout.JAVA_FLOAT).withName("worldToView"), // 532
        ValueLayout.JAVA_INT.withName("flagsReserved1")         // 596
    );

    // LinRtxDenoiseConfig (96 bytes)
    public static final StructLayout DENOISE_CONFIG_LAYOUT = MemoryLayout.structLayout(
        ValueLayout.JAVA_INT.withName("structSize"),            // 0
        ValueLayout.JAVA_INT.withName("width"),                 // 4
        ValueLayout.JAVA_INT.withName("height"),                // 8
        ValueLayout.JAVA_INT.withName("resetHistory"),          // 12
        ValueLayout.JAVA_INT.withName("hdrColorTexGlId"),       // 16
        ValueLayout.JAVA_INT.withName("hdrColorFormatGl"),      // 20
        ValueLayout.JAVA_INT.withName("outHdrColorTexGlId"),    // 24
        ValueLayout.JAVA_INT.withName("outHdrColorFormatGl"),   // 28
        ValueLayout.JAVA_INT.withName("depthTexGlId"),          // 32
        ValueLayout.JAVA_INT.withName("depthFormatGl"),         // 36
        ValueLayout.JAVA_INT.withName("mvecTexGlId"),           // 40
        ValueLayout.JAVA_INT.withName("mvecFormatGl"),          // 44
        ValueLayout.JAVA_INT.withName("diffuseAlbedoTexGlId"),  // 48
        ValueLayout.JAVA_INT.withName("diffuseAlbedoFormatGl"), // 52
        ValueLayout.JAVA_INT.withName("normalsTexGlId"),        // 56
        ValueLayout.JAVA_INT.withName("normalsFormatGl"),       // 60
        ValueLayout.JAVA_FLOAT.withName("temporalBlend"),       // 64
        ValueLayout.JAVA_FLOAT.withName("denoiseStrength"),     // 68
        ValueLayout.JAVA_INT.withName("atrousPasses"),          // 72
        ValueLayout.JAVA_INT.withName("preserveAlpha"),         // 76
        ValueLayout.JAVA_INT.withName("demodulateAlbedo"),      // 80
        ValueLayout.JAVA_INT.withName("normalsAreWorld"),       // 84
        ValueLayout.JAVA_FLOAT.withName("cameraNear"),          // 88
        ValueLayout.JAVA_FLOAT.withName("cameraFar")            // 92
    );

    // LinRtxStatus (1104 bytes)
    public static final StructLayout STATUS_LAYOUT = MemoryLayout.structLayout(
        ValueLayout.JAVA_INT.withName("structSize"),            // 0
        ValueLayout.JAVA_INT.withName("vkReady"),               // 4
        ValueLayout.JAVA_INT.withName("ngxReady"),              // 8
        ValueLayout.JAVA_INT.withName("dlssSupported"),         // 12
        ValueLayout.JAVA_INT.withName("dlssActive"),            // 16
        ValueLayout.JAVA_INT.withName("dlssgSupported"),        // 20
        ValueLayout.JAVA_INT.withName("dlssgActive"),           // 24
        ValueLayout.JAVA_INT.withName("trueHdrNgxLoaded"),      // 28
        ValueLayout.JAVA_INT.withName("trueHdrActive"),         // 32
        ValueLayout.JAVA_INT.withName("hdrShaderActive"),       // 36
        ValueLayout.JAVA_INT.withName("vkSwapchainActive"),     // 40
        ValueLayout.JAVA_INT.withName("vkSwapchainHdr10"),      // 44
        ValueLayout.JAVA_INT.withName("maxFgMultiplier"),       // 48
        ValueLayout.JAVA_INT.withName("activeDlssPreset"),      // 52
        ValueLayout.JAVA_INT.withName("lastErrorCode"),         // 56
        ValueLayout.JAVA_INT.withName("generatedFrameCount"),   // 60
        MemoryLayout.sequenceLayout(128, ValueLayout.JAVA_BYTE).withName("gpuName"),        // 64
        MemoryLayout.sequenceLayout(64, ValueLayout.JAVA_BYTE).withName("driverVersion"),   // 192
        MemoryLayout.sequenceLayout(64, ValueLayout.JAVA_BYTE).withName("dlssVersion"),     // 256
        MemoryLayout.sequenceLayout(64, ValueLayout.JAVA_BYTE).withName("dlssgVersion"),    // 320
        MemoryLayout.sequenceLayout(128, ValueLayout.JAVA_BYTE).withName("trueHdrStatus"),  // 384
        MemoryLayout.sequenceLayout(128, ValueLayout.JAVA_BYTE).withName("swapchainStatus"),// 512
        MemoryLayout.sequenceLayout(256, ValueLayout.JAVA_BYTE).withName("lastErrorMsg"),   // 640
        ValueLayout.JAVA_INT.withName("dlssdSupported"),        // 896
        ValueLayout.JAVA_INT.withName("dlssdActive"),           // 900
        ValueLayout.JAVA_INT.withName("denoiserActive"),        // 904
        ValueLayout.JAVA_INT.withName("activeRrPreset"),        // 908
        MemoryLayout.sequenceLayout(64, ValueLayout.JAVA_BYTE).withName("dlssdVersion"),    // 912
        MemoryLayout.sequenceLayout(128, ValueLayout.JAVA_BYTE).withName("denoiserStatus")  // 976
    );

    public record StatusSnapshot(
        boolean vkReady,
        boolean ngxReady,
        boolean dlssSupported,
        boolean dlssActive,
        boolean dlssgSupported,
        boolean dlssgActive,
        boolean trueHdrNgxLoaded,
        boolean trueHdrActive,
        boolean hdrShaderActive,
        boolean vkSwapchainActive,
        boolean vkSwapchainHdr10,
        int maxFgMultiplier,
        int activeDlssPreset,
        int lastErrorCode,
        int generatedFrameCount,
        String gpuName,
        String driverVersion,
        String dlssVersion,
        String dlssgVersion,
        String trueHdrStatus,
        String swapchainStatus,
        String lastErrorMsg,
        boolean dlssdSupported,
        boolean dlssdActive,
        boolean denoiserActive,
        int activeRrPreset,
        String dlssdVersion,
        String denoiserStatus
    ) {
        public static final StatusSnapshot EMPTY = new StatusSnapshot(
            false, false, false, false, false, false, false, false, false, false, false,
            1, 0, 0, 0, "Not initialized", "-", "-", "-", "Inactive", "OpenGL Swap", "",
            false, false, false, 0, "-", "Inactive"
        );
    }

    private static volatile boolean loaded = false;
    private static volatile String loadError = "";

    private static MethodHandle mhInit;
    private static MethodHandle mhGetOptimalScale;
    private static MethodHandle mhEvaluateDlss;
    private static MethodHandle mhEvaluatePost;
    private static MethodHandle mhEvaluateDenoise;
    private static MethodHandle mhFetchGeneratedFrame;
    private static MethodHandle mhVkPresentFrame;
    private static MethodHandle mhResizeDisplay;
    private static MethodHandle mhGetStatus;
    private static MethodHandle mhShutdown;

    // Thread-safe reusable native scratch memory (avoids per-frame heap/native allocs)
    private static Arena persistentArena;
    private static MemorySegment frameConfigSeg;
    private static MemorySegment denoiseConfigSeg;
    private static MemorySegment statusSeg;
    private static MemorySegment outWSeg;
    private static MemorySegment outHSeg;
    private static MemorySegment outSharpSeg;

    private LinRtxNative() {}

    public static synchronized boolean loadLibrary(Path soPath) {
        if (loaded) return true;
        try {
            System.load(soPath.toAbsolutePath().toString());
            SymbolLookup lookup = SymbolLookup.loaderLookup();
            Linker linker = Linker.nativeLinker();

            mhInit = downcall(linker, lookup, "linrtx_init",
                FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
            mhGetOptimalScale = downcall(linker, lookup, "linrtx_get_optimal_scale",
                FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.JAVA_INT, ValueLayout.JAVA_INT,
                    ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
            mhEvaluateDlss = downcall(linker, lookup, "linrtx_evaluate_dlss",
                FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
            mhEvaluatePost = downcall(linker, lookup, "linrtx_evaluate_post",
                FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
            mhEvaluateDenoise = downcall(linker, lookup, "linrtx_evaluate_denoise",
                FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
            mhFetchGeneratedFrame = downcall(linker, lookup, "linrtx_fetch_generated_frame",
                FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.JAVA_INT, ValueLayout.JAVA_INT));
            mhVkPresentFrame = downcall(linker, lookup, "linrtx_vk_present_frame",
                FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.JAVA_INT));
            mhResizeDisplay = downcall(linker, lookup, "linrtx_resize_display",
                FunctionDescriptor.ofVoid(ValueLayout.JAVA_INT, ValueLayout.JAVA_INT));
            mhGetStatus = downcall(linker, lookup, "linrtx_get_status",
                FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));
            mhShutdown = downcall(linker, lookup, "linrtx_shutdown",
                FunctionDescriptor.ofVoid());

            persistentArena = Arena.ofShared();
            frameConfigSeg = persistentArena.allocate(FRAME_CONFIG_LAYOUT);
            denoiseConfigSeg = persistentArena.allocate(DENOISE_CONFIG_LAYOUT);
            statusSeg = persistentArena.allocate(STATUS_LAYOUT);
            outWSeg = persistentArena.allocate(ValueLayout.JAVA_INT);
            outHSeg = persistentArena.allocate(ValueLayout.JAVA_INT);
            outSharpSeg = persistentArena.allocate(ValueLayout.JAVA_FLOAT);

            loaded = true;
            loadError = "";
            LOGGER.info("Loaded LinRTX native library via Java 25 FFM: {}", soPath);
            return true;
        } catch (Throwable t) {
            loadError = t.getClass().getSimpleName() + ": " + t.getMessage();
            LOGGER.error("Failed to load LinRTX native library {}", soPath, t);
            return false;
        }
    }

    private static MethodHandle downcall(Linker linker, SymbolLookup lookup, String symbol, FunctionDescriptor desc) {
        MemorySegment addr = lookup.find(symbol)
            .orElseThrow(() -> new UnsatisfiedLinkError("Missing symbol " + symbol));
        return linker.downcallHandle(addr, desc);
    }

    public static boolean isLoaded() {
        return loaded;
    }

    public static String getLoadError() {
        return loadError;
    }

    public static int init(
        boolean enableValidation,
        long windowHandle,
        long nativeDisplayPtr,
        long nativeWindowHandle,
        boolean isWayland,
        String ngxLibDir,
        String trueHdrSearchDirs,
        String logDir
    ) {
        if (!loaded) return -1;
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment seg = arena.allocate(INIT_CONFIG_LAYOUT);
            seg.fill((byte) 0);
            seg.set(ValueLayout.JAVA_INT, 0, (int) INIT_CONFIG_LAYOUT.byteSize());
            seg.set(ValueLayout.JAVA_INT, 4, enableValidation ? 1 : 0);
            seg.set(ValueLayout.JAVA_LONG, 8, 0x4C494E5254580001L);
            seg.set(ValueLayout.JAVA_LONG, 16, windowHandle);
            seg.set(ValueLayout.JAVA_LONG, 24, nativeDisplayPtr);
            seg.set(ValueLayout.JAVA_LONG, 32, nativeWindowHandle);
            seg.set(ValueLayout.JAVA_INT, 40, isWayland ? 1 : 0);
            seg.set(ValueLayout.ADDRESS, 48, arena.allocateFrom(ngxLibDir != null ? ngxLibDir : ""));
            seg.set(ValueLayout.ADDRESS, 56, arena.allocateFrom(trueHdrSearchDirs != null ? trueHdrSearchDirs : ""));
            seg.set(ValueLayout.ADDRESS, 64, arena.allocateFrom(logDir != null ? logDir : "/tmp"));
            return (int) mhInit.invokeExact(seg);
        } catch (Throwable t) {
            loadError = t.getMessage();
            return -1;
        }
    }

    public static int[] getOptimalScale(int dlssModeNativeId, int displayW, int displayH) {
        if (!loaded) return null;
        try {
            int rc = (int) mhGetOptimalScale.invokeExact(
                dlssModeNativeId, displayW, displayH, outWSeg, outHSeg, outSharpSeg);
            if (rc >= 0) {
                int rw = outWSeg.get(ValueLayout.JAVA_INT, 0);
                int rh = outHSeg.get(ValueLayout.JAVA_INT, 0);
                if (rw > 0 && rh > 0) {
                    return new int[]{rw, rh};
                }
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    public static MemorySegment scratchFrameConfig() {
        frameConfigSeg.fill((byte) 0);
        frameConfigSeg.set(ValueLayout.JAVA_INT, 0, (int) FRAME_CONFIG_LAYOUT.byteSize());
        return frameConfigSeg;
    }

    public static MemorySegment scratchDenoiseConfig() {
        denoiseConfigSeg.fill((byte) 0);
        denoiseConfigSeg.set(ValueLayout.JAVA_INT, 0, (int) DENOISE_CONFIG_LAYOUT.byteSize());
        return denoiseConfigSeg;
    }

    public static void putFloatArray(MemorySegment base, long offset, float[] values, int count) {
        for (int i = 0; i < count; i++) {
            base.set(ValueLayout.JAVA_FLOAT, offset + (long) i * 4L, values[i]);
        }
    }

    public static int evaluateDlss(MemorySegment frameSeg) {
        if (!loaded) return -1;
        try {
            return (int) mhEvaluateDlss.invokeExact(frameSeg);
        } catch (Throwable t) {
            return -1;
        }
    }

    public static int evaluatePost(MemorySegment frameSeg) {
        if (!loaded) return -1;
        try {
            return (int) mhEvaluatePost.invokeExact(frameSeg);
        } catch (Throwable t) {
            return -1;
        }
    }

    public static int evaluateDenoise(MemorySegment denoiseSeg) {
        if (!loaded) return -1;
        try {
            return (int) mhEvaluateDenoise.invokeExact(denoiseSeg);
        } catch (Throwable t) {
            return -1;
        }
    }

    public static int fetchGeneratedFrame(int interpIndex, int dstGlTexId) {
        if (!loaded) return -1;
        try {
            return (int) mhFetchGeneratedFrame.invokeExact(interpIndex, dstGlTexId);
        } catch (Throwable t) {
            return -1;
        }
    }

    public static int vkPresentFrame(int interpIndex) {
        if (!loaded) return -1;
        try {
            return (int) mhVkPresentFrame.invokeExact(interpIndex);
        } catch (Throwable t) {
            return -1;
        }
    }

    public static void resizeDisplay(int w, int h) {
        if (!loaded) return;
        try {
            mhResizeDisplay.invokeExact(w, h);
        } catch (Throwable ignored) {
        }
    }

    public static StatusSnapshot getStatus() {
        if (!loaded) return StatusSnapshot.EMPTY;
        try {
            statusSeg.fill((byte) 0);
            mhGetStatus.invokeExact(statusSeg);
            return new StatusSnapshot(
                statusSeg.get(ValueLayout.JAVA_INT, 4) != 0,
                statusSeg.get(ValueLayout.JAVA_INT, 8) != 0,
                statusSeg.get(ValueLayout.JAVA_INT, 12) != 0,
                statusSeg.get(ValueLayout.JAVA_INT, 16) != 0,
                statusSeg.get(ValueLayout.JAVA_INT, 20) != 0,
                statusSeg.get(ValueLayout.JAVA_INT, 24) != 0,
                statusSeg.get(ValueLayout.JAVA_INT, 28) != 0,
                statusSeg.get(ValueLayout.JAVA_INT, 32) != 0,
                statusSeg.get(ValueLayout.JAVA_INT, 36) != 0,
                statusSeg.get(ValueLayout.JAVA_INT, 40) != 0,
                statusSeg.get(ValueLayout.JAVA_INT, 44) != 0,
                statusSeg.get(ValueLayout.JAVA_INT, 48),
                statusSeg.get(ValueLayout.JAVA_INT, 52),
                statusSeg.get(ValueLayout.JAVA_INT, 56),
                statusSeg.get(ValueLayout.JAVA_INT, 60),
                readCString(statusSeg, 64, 128),
                readCString(statusSeg, 192, 64),
                readCString(statusSeg, 256, 64),
                readCString(statusSeg, 320, 64),
                readCString(statusSeg, 384, 128),
                readCString(statusSeg, 512, 128),
                readCString(statusSeg, 640, 256),
                statusSeg.get(ValueLayout.JAVA_INT, 896) != 0,
                statusSeg.get(ValueLayout.JAVA_INT, 900) != 0,
                statusSeg.get(ValueLayout.JAVA_INT, 904) != 0,
                statusSeg.get(ValueLayout.JAVA_INT, 908),
                readCString(statusSeg, 912, 64),
                readCString(statusSeg, 976, 128)
            );
        } catch (Throwable t) {
            return StatusSnapshot.EMPTY;
        }
    }

    public static synchronized void shutdown() {
        if (!loaded) return;
        try {
            mhShutdown.invokeExact();
        } catch (Throwable ignored) {
        }
    }

    private static String readCString(MemorySegment seg, long offset, int maxLen) {
        int len = 0;
        byte[] buf = new byte[maxLen];
        while (len < maxLen) {
            byte b = seg.get(ValueLayout.JAVA_BYTE, offset + len);
            if (b == 0) break;
            buf[len++] = b;
        }
        return new String(buf, 0, len, StandardCharsets.UTF_8);
    }
}
