package dev.linrtx.mc.config;

import dev.linrtx.mc.config.LinRtxEnums.*;

import java.util.List;

/**
 * One-click full pipeline profiles and live preset cycler for in-game switching.
 */
public final class LinRtxPresetProfile {
    public record Bundle(
        String name,
        String subtitle,
        DlssMode dlssMode,
        DlssPreset dlssPreset,
        float sharpness,
        FrameGenMode fgMode,
        HdrEngineMode hdrEngine,
        HdrTonePreset hdrTonePreset,
        DenoiseMode denoiseMode,
        RayReconstructionMode rrMode,
        NeuralEngineMode neuralMode,
        NeuralStyle neuralStyle
    ) {
        public Bundle(
            String name,
            String subtitle,
            DlssMode dlssMode,
            DlssPreset dlssPreset,
            float sharpness,
            FrameGenMode fgMode,
            HdrEngineMode hdrEngine,
            HdrTonePreset hdrTonePreset
        ) {
            this(name, subtitle, dlssMode, dlssPreset, sharpness, fgMode, hdrEngine, hdrTonePreset,
                DenoiseMode.OFF, RayReconstructionMode.AUTO, NeuralEngineMode.OFF, NeuralStyle.DEFAULT);
        }

        public Bundle(
            String name,
            String subtitle,
            DlssMode dlssMode,
            DlssPreset dlssPreset,
            float sharpness,
            FrameGenMode fgMode,
            HdrEngineMode hdrEngine,
            HdrTonePreset hdrTonePreset,
            DenoiseMode denoiseMode,
            RayReconstructionMode rrMode
        ) {
            this(name, subtitle, dlssMode, dlssPreset, sharpness, fgMode, hdrEngine, hdrTonePreset,
                denoiseMode, rrMode, NeuralEngineMode.OFF, NeuralStyle.DEFAULT);
        }
    }

    public static final List<Bundle> PROFILES = List.of(
        new Bundle(
            "DLAA Reference + RTX HDR",
            "Native res DLAA, Transformer Preset K, 2x FG, Reference HDR",
            DlssMode.DLAA, DlssPreset.K, 0.15f, FrameGenMode.X2, HdrEngineMode.AUTO, HdrTonePreset.TRUEHDR_REFERENCE
        ),
        new Bundle(
            "DLSS 4.5 Balanced (Preset M)",
            "Recommended 4.5 Gen-2 Transformer + 2x FG + Cinematic HDR",
            DlssMode.BALANCED, DlssPreset.M, 0.25f, FrameGenMode.X2, HdrEngineMode.AUTO, HdrTonePreset.CINEMATIC
        ),
        new Bundle(
            "DLSS 5 Neural Cinematic (Any RTX)",
            "Balanced Preset M + DLSS 5 Neural Engine Filter (Unlocked) + 2x FG",
            DlssMode.BALANCED, DlssPreset.M, 0.15f, FrameGenMode.X2, HdrEngineMode.AUTO, HdrTonePreset.CINEMATIC,
            DenoiseMode.OFF, RayReconstructionMode.AUTO, NeuralEngineMode.AUTO, NeuralStyle.CINEMATIC
        ),
        new Bundle(
            "Path Tracing / Camellia (DLSS-RR + SVGF)",
            "Quality 0.67x, Pre-Tonemap DLSS-RR / SVGF + F1 Accum Boost + Natural HDR",
            DlssMode.QUALITY, DlssPreset.K, 0.15f, FrameGenMode.X2, HdrEngineMode.AUTO, HdrTonePreset.NATURAL,
            DenoiseMode.SVGF_ATROUS, RayReconstructionMode.AUTO
        ),
        new Bundle(
            "DLSS 4.5 Max FPS (Preset L + 4x FG)",
            "Ultra Performance 4.5 Transformer + 4x Multi-Frame + Vivid HDR",
            DlssMode.ULTRA_PERFORMANCE, DlssPreset.L, 0.35f, FrameGenMode.X4, HdrEngineMode.AUTO, HdrTonePreset.VIVID
        ),
        new Bundle(
            "Quality Low-Ghosting (Preset J)",
            "Quality 0.67x, Preset J, Frame Gen Off, Natural HDR",
            DlssMode.QUALITY, DlssPreset.J, 0.20f, FrameGenMode.OFF, HdrEngineMode.AUTO, HdrTonePreset.NATURAL
        ),
        new Bundle(
            "Vanilla Passthrough",
            "All LinRTX features bypassed (1x native OpenGL)",
            DlssMode.OFF, DlssPreset.DEFAULT, 0.0f, FrameGenMode.OFF, HdrEngineMode.OFF, HdrTonePreset.CUSTOM,
            DenoiseMode.OFF, RayReconstructionMode.OFF, NeuralEngineMode.OFF, NeuralStyle.DEFAULT
        )
    );

    private static final List<DlssPreset> LIVE_CYCLE_PRESETS = List.of(
        DlssPreset.J, DlssPreset.K, DlssPreset.L, DlssPreset.M, DlssPreset.DEFAULT
    );

    private LinRtxPresetProfile() {}

    public static void applyProfile(LinRtxConfig cfg, Bundle b) {
        cfg.dlssMode = b.dlssMode();
        cfg.dlssPreset = b.dlssPreset();
        cfg.sharpness = b.sharpness();
        cfg.fgMode = b.fgMode();
        cfg.hdrEngine = b.hdrEngine();
        cfg.denoiseMode = b.denoiseMode();
        cfg.rrMode = b.rrMode();
        cfg.neuralMode = b.neuralMode();
        cfg.neuralStyle = b.neuralStyle();
        cfg.applyHdrPreset(b.hdrTonePreset());
        cfg.clamp();
        cfg.save();
    }

    public static DlssPreset cycleNextPreset(LinRtxConfig cfg) {
        int idx = LIVE_CYCLE_PRESETS.indexOf(cfg.dlssPreset);
        DlssPreset next = LIVE_CYCLE_PRESETS.get((idx + 1) % LIVE_CYCLE_PRESETS.size());
        cfg.dlssPreset = next;
        cfg.save();
        return next;
    }
}
