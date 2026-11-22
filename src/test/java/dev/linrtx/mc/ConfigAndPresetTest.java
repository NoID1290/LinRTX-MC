package dev.linrtx.mc;

import dev.linrtx.mc.config.LinRtxConfig;
import dev.linrtx.mc.config.LinRtxEnums.*;
import dev.linrtx.mc.config.LinRtxPresetProfile;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.file.Path;

import static org.junit.jupiter.api.Assertions.*;

class ConfigAndPresetTest {
    @Test
    void jsonSaveLoadRoundTripAndClamping(@TempDir Path tempDir) {
        Path cfgPath = tempDir.resolve("linrtx.json");
        LinRtxConfig cfg = new LinRtxConfig();
        cfg.dlssMode = DlssMode.CUSTOM;
        cfg.dlssPreset = DlssPreset.L;
        cfg.customScale = 9.0f;      // out of range -> clamp to 1.0f
        cfg.sharpness = -1.0f;       // out of range -> clamp to 0.0f
        cfg.hdrContrast = 500;       // clamp to 200
        cfg.hdrMaxLuminance = 50;    // clamp to 400
        cfg.denoiseMode = DenoiseMode.SVGF_ATROUS;
        cfg.rrMode = RayReconstructionMode.ON;
        cfg.rrPreset = RrPreset.E;
        cfg.gbufferGuideMode = GbufferGuideMode.CAMELLIA_IRIS;
        cfg.preTonemapIrisDenoise = false;
        cfg.denoiseTemporalBlend = 5.0f;  // clamp to 0.96f
        cfg.denoiseStrength = 0.01f;      // clamp to 0.25f
        cfg.denoiseAtrousPasses = 99;     // clamp to 5
        cfg.f1AccumFadeSamples = 999;     // clamp to 256
        cfg.neuralMode = NeuralEngineMode.AUTO;
        cfg.neuralStyle = NeuralStyle.CINEMATIC;
        cfg.neuralPreset = NeuralPreset.SMALL_FP8;
        cfg.neuralPlacement = NeuralPlacement.PRE_SR;
        cfg.neuralUnlockAnyRtx = true;
        cfg.neuralUseAutoMask = false;
        cfg.neuralWorkingScale = 0.10f;            // clamp to 0.50f
        cfg.neuralIntensity = 5.0f;                // clamp to 2.00f
        cfg.neuralLocalToneStrength = -2.0f;       // clamp to 0.00f
        cfg.neuralLocalStructureStrength = 3.5f;   // clamp to 2.00f
        cfg.neuralSkinStructureStrength = -0.8f;   // normalize to -1.0f (Auto)
        cfg.save(cfgPath);

        LinRtxConfig reloaded = LinRtxConfig.load(cfgPath);
        assertEquals(DlssMode.CUSTOM, reloaded.dlssMode);
        assertEquals(DlssPreset.L, reloaded.dlssPreset);
        assertEquals(1.0f, reloaded.customScale, 1e-5f);
        assertEquals(0.0f, reloaded.sharpness, 1e-5f);
        assertEquals(200, reloaded.hdrContrast);
        assertEquals(400, reloaded.hdrMaxLuminance);
        assertEquals(DenoiseMode.SVGF_ATROUS, reloaded.denoiseMode);
        assertEquals(RayReconstructionMode.ON, reloaded.rrMode);
        assertEquals(RrPreset.E, reloaded.rrPreset);
        assertEquals(GbufferGuideMode.CAMELLIA_IRIS, reloaded.gbufferGuideMode);
        assertFalse(reloaded.preTonemapIrisDenoise);
        assertEquals(0.96f, reloaded.denoiseTemporalBlend, 1e-5f);
        assertEquals(0.25f, reloaded.denoiseStrength, 1e-5f);
        assertEquals(5, reloaded.denoiseAtrousPasses);
        assertEquals(256, reloaded.f1AccumFadeSamples);
        assertEquals(NeuralEngineMode.AUTO, reloaded.neuralMode);
        assertEquals(NeuralStyle.CINEMATIC, reloaded.neuralStyle);
        assertEquals(NeuralPreset.SMALL_FP8, reloaded.neuralPreset);
        assertEquals(NeuralPlacement.PRE_SR, reloaded.neuralPlacement);
        assertTrue(reloaded.neuralUnlockAnyRtx);
        assertFalse(reloaded.neuralUseAutoMask);
        assertEquals(0.50f, reloaded.neuralWorkingScale, 1e-5f);
        assertEquals(2.00f, reloaded.neuralIntensity, 1e-5f);
        assertEquals(0.00f, reloaded.neuralLocalToneStrength, 1e-5f);
        assertEquals(2.00f, reloaded.neuralLocalStructureStrength, 1e-5f);
        assertEquals(-1.00f, reloaded.neuralSkinStructureStrength, 1e-5f);
    }

    @Test
    void neuralProfileEnablesDlss5NeuralFilterAndVanillaDisablesIt() {
        LinRtxPresetProfile.Bundle nrProfile = LinRtxPresetProfile.PROFILES.stream()
            .filter(p -> p.name().contains("DLSS 5 Neural"))
            .findFirst()
            .orElseThrow();
        LinRtxPresetProfile.Bundle vanillaProfile = LinRtxPresetProfile.PROFILES.stream()
            .filter(p -> p.name().contains("Vanilla"))
            .findFirst()
            .orElseThrow();
        LinRtxConfig cfg = new LinRtxConfig();
        LinRtxPresetProfile.applyProfile(cfg, nrProfile);
        assertEquals(NeuralEngineMode.AUTO, cfg.neuralMode);
        assertEquals(NeuralStyle.CINEMATIC, cfg.neuralStyle);

        LinRtxPresetProfile.applyProfile(cfg, vanillaProfile);
        assertEquals(NeuralEngineMode.OFF, cfg.neuralMode);
    }

    @Test
    void pathTracingProfileEnablesDenoiseAndRr(@TempDir Path tempDir) {
        LinRtxPresetProfile.Bundle ptProfile = LinRtxPresetProfile.PROFILES.stream()
            .filter(p -> p.name().contains("Camellia"))
            .findFirst()
            .orElseThrow();
        LinRtxConfig cfg = new LinRtxConfig();
        LinRtxPresetProfile.applyProfile(cfg, ptProfile);
        assertEquals(DenoiseMode.SVGF_ATROUS, cfg.denoiseMode);
        assertEquals(RayReconstructionMode.AUTO, cfg.rrMode);
    }

    @Test
    void livePresetCyclingOrder() {
        LinRtxConfig cfg = new LinRtxConfig();
        cfg.dlssPreset = DlssPreset.J;
        assertEquals(DlssPreset.K, LinRtxPresetProfile.cycleNextPreset(cfg));
        assertEquals(DlssPreset.L, LinRtxPresetProfile.cycleNextPreset(cfg));
        assertEquals(DlssPreset.M, LinRtxPresetProfile.cycleNextPreset(cfg));
        assertEquals(DlssPreset.DEFAULT, LinRtxPresetProfile.cycleNextPreset(cfg));
        assertEquals(DlssPreset.J, LinRtxPresetProfile.cycleNextPreset(cfg));
    }
}
