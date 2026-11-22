package dev.linrtx.mc.config;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import dev.linrtx.mc.config.LinRtxEnums.*;
import net.fabricmc.loader.api.FabricLoader;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

public final class LinRtxConfig {
    private static final Logger LOGGER = LoggerFactory.getLogger("LinRTX");
    private static final Gson GSON = new GsonBuilder().setPrettyPrinting().create();
    private static volatile LinRtxConfig instance;

    // DLSS 4.5 Super Resolution
    public DlssMode dlssMode = DlssMode.QUALITY;
    public DlssPreset dlssPreset = DlssPreset.K;
    public float customScale = 0.75f;
    public float sharpness = 0.25f;
    public boolean autoMipBias = true;
    public boolean combineDistantHorizonsDepth = true;

    // DLSS-G Frame Generation
    public FrameGenMode fgMode = FrameGenMode.X2;
    public boolean hudlessSplit = true;

    // Denoise & DLSS Ray Reconstruction
    public DenoiseMode denoiseMode = DenoiseMode.OFF;
    public RayReconstructionMode rrMode = RayReconstructionMode.AUTO;
    public RrPreset rrPreset = RrPreset.DEFAULT;
    public GbufferGuideMode gbufferGuideMode = GbufferGuideMode.AUTO;
    public boolean preTonemapIrisDenoise = true;
    public float denoiseTemporalBlend = 0.88f;
    public float denoiseStrength = 1.0f;
    public int denoiseAtrousPasses = 4;
    public int f1AccumFadeSamples = 48;

    // RTX HDR / TrueHDR
    public HdrEngineMode hdrEngine = HdrEngineMode.AUTO;
    public HdrOutputMode hdrOutputMode = HdrOutputMode.SDR_BOOST;
    public HdrTonePreset hdrTonePreset = HdrTonePreset.CINEMATIC;
    public int hdrContrast = 110;
    public int hdrSaturation = 108;
    public int hdrMiddleGray = 48;
    public int hdrMaxLuminance = 650;
    public boolean hdrDebanding = true;
    public float hdrHighlightBoost = 0.35f;
    public String customTrueHdrDir = "";

    // DLSS 5 Neural Engine Filter (DLSS-NR / NGX Feature 18 + Unlocked Vulkan Engine)
    public NeuralEngineMode neuralMode = NeuralEngineMode.OFF;
    public NeuralStyle neuralStyle = NeuralStyle.DEFAULT;
    public NeuralPreset neuralPreset = NeuralPreset.AUTO;
    public NeuralPlacement neuralPlacement = NeuralPlacement.POST_SR;
    public boolean neuralUnlockAnyRtx = true;
    public boolean neuralUseAutoMask = true;
    public float neuralWorkingScale = 1.0f;
    public float neuralIntensity = 0.75f;
    public float neuralLocalToneStrength = 1.0f;
    public float neuralLocalStructureStrength = 1.0f;
    public float neuralSkinStructureStrength = -1.0f;
    public String customNeuralDir = "";

    // Presentation & UI
    public PresentBackend presentBackend = PresentBackend.OPENGL;
    public boolean showStatusOverlay = true;
    public boolean enableVulkanValidation = false;

    public static LinRtxConfig get() {
        LinRtxConfig cur = instance;
        if (cur == null) {
            cur = load(defaultConfigPath());
            instance = cur;
        }
        return cur;
    }

    public static Path defaultConfigPath() {
        try {
            return FabricLoader.getInstance().getConfigDir().resolve("linrtx.json");
        } catch (Throwable ignored) {
            return Path.of("config", "linrtx.json");
        }
    }

    public static LinRtxConfig load(Path path) {
        LinRtxConfig cfg = new LinRtxConfig();
        if (Files.isRegularFile(path)) {
            try {
                String json = Files.readString(path);
                LinRtxConfig parsed = GSON.fromJson(json, LinRtxConfig.class);
                if (parsed != null) {
                    cfg = parsed;
                }
            } catch (Exception e) {
                LOGGER.warn("Failed to parse LinRTX config {}, using defaults", path, e);
            }
        }
        cfg.clamp();
        return cfg;
    }

    public synchronized void save() {
        save(defaultConfigPath());
    }

    public synchronized void save(Path path) {
        clamp();
        try {
            Files.createDirectories(path.getParent());
            Files.writeString(path, GSON.toJson(this));
        } catch (IOException e) {
            LOGGER.warn("Failed to save LinRTX config {}", path, e);
        }
    }

    public void applyHdrPreset(HdrTonePreset preset) {
        this.hdrTonePreset = preset;
        if (preset != null && preset.contrast >= 0) {
            this.hdrContrast = preset.contrast;
            this.hdrSaturation = preset.saturation;
            this.hdrMiddleGray = preset.middleGray;
            this.hdrMaxLuminance = preset.maxLuminance;
            this.hdrHighlightBoost = preset.highlightBoost;
        }
    }

    public void clamp() {
        if (dlssMode == null) dlssMode = DlssMode.QUALITY;
        if (dlssPreset == null) dlssPreset = DlssPreset.K;
        if (fgMode == null) fgMode = FrameGenMode.OFF;
        if (hdrEngine == null) hdrEngine = HdrEngineMode.AUTO;
        if (hdrOutputMode == null) hdrOutputMode = HdrOutputMode.SDR_BOOST;
        if (hdrTonePreset == null) hdrTonePreset = HdrTonePreset.CINEMATIC;
        if (presentBackend == null) presentBackend = PresentBackend.OPENGL;
        if (denoiseMode == null) denoiseMode = DenoiseMode.OFF;
        if (rrMode == null) rrMode = RayReconstructionMode.AUTO;
        if (rrPreset == null) rrPreset = RrPreset.DEFAULT;
        if (gbufferGuideMode == null) gbufferGuideMode = GbufferGuideMode.AUTO;
        if (customTrueHdrDir == null) customTrueHdrDir = "";
        if (neuralMode == null) neuralMode = NeuralEngineMode.OFF;
        if (neuralStyle == null) neuralStyle = NeuralStyle.DEFAULT;
        if (neuralPreset == null) neuralPreset = NeuralPreset.AUTO;
        if (neuralPlacement == null) neuralPlacement = NeuralPlacement.POST_SR;
        if (customNeuralDir == null) customNeuralDir = "";

        customScale = Math.max(0.25f, Math.min(1.0f, customScale));
        sharpness = Math.max(0.0f, Math.min(1.0f, sharpness));
        denoiseTemporalBlend = Math.max(0.0f, Math.min(0.96f, denoiseTemporalBlend));
        denoiseStrength = Math.max(0.25f, Math.min(2.5f, denoiseStrength));
        denoiseAtrousPasses = Math.max(1, Math.min(5, denoiseAtrousPasses));
        f1AccumFadeSamples = Math.max(0, Math.min(256, f1AccumFadeSamples));
        hdrContrast = Math.max(0, Math.min(200, hdrContrast));
        hdrSaturation = Math.max(0, Math.min(200, hdrSaturation));
        hdrMiddleGray = Math.max(10, Math.min(100, hdrMiddleGray));
        hdrMaxLuminance = Math.max(400, Math.min(2000, hdrMaxLuminance));
        hdrHighlightBoost = Math.max(0.0f, Math.min(2.0f, hdrHighlightBoost));
        neuralWorkingScale = Math.max(0.50f, Math.min(1.0f, neuralWorkingScale));
        neuralIntensity = Math.max(0.0f, Math.min(2.0f, neuralIntensity));
        neuralLocalToneStrength = Math.max(0.0f, Math.min(2.0f, neuralLocalToneStrength));
        neuralLocalStructureStrength = Math.max(0.0f, Math.min(2.0f, neuralLocalStructureStrength));
        neuralSkinStructureStrength = (neuralSkinStructureStrength < -0.5f)
            ? -1.0f
            : Math.max(0.0f, Math.min(2.0f, neuralSkinStructureStrength));
    }
}
