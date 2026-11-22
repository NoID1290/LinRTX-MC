package dev.linrtx.mc.config;

public final class LinRtxEnums {
    private LinRtxEnums() {}

    public enum DlssMode {
        OFF(0, "Off (Native)", 1.0f),
        DLAA(1, "DLAA (1.00x AA)", 1.0f),
        QUALITY(2, "Quality (0.67x)", 1.0f / 1.5f),
        BALANCED(3, "Balanced (0.58x)", 1.0f / 1.724138f),
        PERFORMANCE(4, "Performance (0.50x)", 0.5f),
        ULTRA_PERFORMANCE(5, "Ultra Perf (0.33x)", 1.0f / 3.0f),
        CUSTOM(6, "Custom Scale", 0.75f);

        public final int nativeId;
        public final String displayName;
        public final float defaultRatio;

        DlssMode(int nativeId, String displayName, float defaultRatio) {
            this.nativeId = nativeId;
            this.displayName = displayName;
            this.defaultRatio = defaultRatio;
        }
    }

    public enum DlssPreset {
        DEFAULT(0, "Default (Driver Auto)"),
        K(11, "Preset K (DLSS 4.0 Transformer - Sharp Quality)"),
        J(10, "Preset J (DLSS 4.0 Transformer - Low Ghosting)"),
        L(12, "Preset L (DLSS 4.5 Gen-2 Transformer - Crisp/Ultra)"),
        M(13, "Preset M (DLSS 4.5 Gen-2 Transformer - Motion/Perf)"),
        E(5, "Preset E (Legacy CNN Fast)"),
        F(6, "Preset F (Legacy CNN Soft)");

        public final int nativeId;
        public final String displayName;

        DlssPreset(int nativeId, String displayName) {
            this.nativeId = nativeId;
            this.displayName = displayName;
        }

        public static DlssPreset fromNativeId(int id) {
            for (DlssPreset p : values()) {
                if (p.nativeId == id) return p;
            }
            return DEFAULT;
        }
    }

    public enum FrameGenMode {
        OFF(0, "Off (1x)", 1),
        X2(1, "DLSS-G 2x (+1 Gen Frame)", 2),
        X3(2, "Multi-Frame 3x (+2 Gen)", 3),
        X4(3, "Multi-Frame 4x (+3 Gen)", 4);

        public final int nativeId;
        public final String displayName;
        public final int multiplier;

        FrameGenMode(int nativeId, String displayName, int multiplier) {
            this.nativeId = nativeId;
            this.displayName = displayName;
            this.multiplier = multiplier;
        }
    }

    public enum HdrEngineMode {
        OFF(0, "Off"),
        AUTO(1, "Auto (TrueHDR -> GPU Shader)"),
        TRUEHDR_ONLY(2, "NVIDIA TrueHDR NGX Only"),
        SHADER_ONLY(3, "GPU Compute RTX HDR Shader");

        public final int nativeId;
        public final String displayName;

        HdrEngineMode(int nativeId, String displayName) {
            this.nativeId = nativeId;
            this.displayName = displayName;
        }
    }

    public enum HdrOutputMode {
        SDR_BOOST(0, "SDR Tone Boost (Standard Monitor)"),
        PQ_BT2020(1, "HDR10 BT.2020 PQ (Gamescope / HDR Display)"),
        SCRGB_FP16(2, "scRGB Linear FP16");

        public final int nativeId;
        public final String displayName;

        HdrOutputMode(int nativeId, String displayName) {
            this.nativeId = nativeId;
            this.displayName = displayName;
        }
    }

    public enum HdrTonePreset {
        CUSTOM("Custom", -1, -1, -1, -1, -1f),
        CINEMATIC("Cinematic (650 nits)", 110, 108, 48, 650, 0.35f),
        VIVID("Vivid (1000 nits)", 125, 122, 52, 1000, 0.60f),
        NATURAL("Natural (500 nits)", 100, 102, 50, 500, 0.20f),
        TRUEHDR_REFERENCE("TrueHDR Reference (800 nits)", 100, 100, 50, 800, 0.15f);

        public final String displayName;
        public final int contrast;
        public final int saturation;
        public final int middleGray;
        public final int maxLuminance;
        public final float highlightBoost;

        HdrTonePreset(String displayName, int contrast, int saturation, int middleGray, int maxLuminance, float highlightBoost) {
            this.displayName = displayName;
            this.contrast = contrast;
            this.saturation = saturation;
            this.middleGray = middleGray;
            this.maxLuminance = maxLuminance;
            this.highlightBoost = highlightBoost;
        }
    }

    public enum PresentBackend {
        OPENGL(0, "OpenGL Swap (Best Mod Compat / Gamescope HDR)"),
        VULKAN_HDR(1, "Vulkan HDR10 Swapchain (Experimental X11)");

        public final int nativeId;
        public final String displayName;

        PresentBackend(int nativeId, String displayName) {
            this.nativeId = nativeId;
            this.displayName = displayName;
        }
    }

    public enum DenoiseMode {
        OFF(0, "Off"),
        SVGF_ATROUS(1, "SVGF + À-Trous Wavelet (High Quality)"),
        SVGF_FAST(2, "SVGF Fast (2-Pass À-Trous)");

        public final int nativeId;
        public final String displayName;

        DenoiseMode(int nativeId, String displayName) {
            this.nativeId = nativeId;
            this.displayName = displayName;
        }
    }

    public enum RayReconstructionMode {
        OFF(0, "Off"),
        ON(1, "On (Force DLSS-RR)"),
        AUTO(2, "Auto (RTX + G-Buffer Guides / Fallback SVGF)");

        public final int nativeId;
        public final String displayName;

        RayReconstructionMode(int nativeId, String displayName) {
            this.nativeId = nativeId;
            this.displayName = displayName;
        }
    }

    public enum RrPreset {
        DEFAULT(0, "Default (NGX Auto)"),
        D(4, "Preset D (Classic Transformer)"),
        E(5, "Preset E (Crisper Reflections)"),
        F(6, "Preset F (Path Tracing Stability)");

        public final int nativeId;
        public final String displayName;

        RrPreset(int nativeId, String displayName) {
            this.nativeId = nativeId;
            this.displayName = displayName;
        }

        public static RrPreset fromNativeId(int id) {
            for (RrPreset p : values()) {
                if (p.nativeId == id) return p;
            }
            return DEFAULT;
        }
    }

    public enum GbufferGuideMode {
        AUTO(0, "Auto (Camellia Iris -> Synthetic Fallback)"),
        CAMELLIA_IRIS(1, "Iris G-Buffers Only"),
        SYNTHETIC(2, "Depth/Color Synthetic Guides"),
        NONE(3, "Color/Depth/MV Only (No Guides)");

        public final int nativeId;
        public final String displayName;

        GbufferGuideMode(int nativeId, String displayName) {
            this.nativeId = nativeId;
            this.displayName = displayName;
        }
    }

    public enum NeuralEngineMode {
        OFF(0, "Off"),
        AUTO(1, "Auto (NGX F18 -> Vulkan)"),
        NGX_ONLY(2, "NVIDIA NGX Feature 18 Only"),
        VULKAN_ONLY(3, "Vulkan Neural Engine (Any RTX)");

        public final int nativeId;
        public final String displayName;

        NeuralEngineMode(int nativeId, String displayName) {
            this.nativeId = nativeId;
            this.displayName = displayName;
        }
    }

    public enum NeuralStyle {
        DEFAULT(0, "Default"),
        NATURAL(1, "Natural"),
        CINEMATIC(2, "Cinematic");

        public final int nativeId;
        public final String displayName;

        NeuralStyle(int nativeId, String displayName) {
            this.nativeId = nativeId;
            this.displayName = displayName;
        }
    }

    public enum NeuralPreset {
        AUTO(0, "Auto"),
        QUALITY(1, "Quality (HT)"),
        SMALL_FP8(2, "Small FP8");

        public final int nativeId;
        public final String displayName;

        NeuralPreset(int nativeId, String displayName) {
            this.nativeId = nativeId;
            this.displayName = displayName;
        }

        public static NeuralPreset fromNativeId(int id) {
            for (NeuralPreset p : values()) {
                if (p.nativeId == id) return p;
            }
            return AUTO;
        }
    }

    public enum NeuralPlacement {
        POST_SR(0, "Post-SR (Display World)"),
        PRE_SR(1, "Pre-SR (Render World)");

        public final int nativeId;
        public final String displayName;

        NeuralPlacement(int nativeId, String displayName) {
            this.nativeId = nativeId;
            this.displayName = displayName;
        }
    }
}
