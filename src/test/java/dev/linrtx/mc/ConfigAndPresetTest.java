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
