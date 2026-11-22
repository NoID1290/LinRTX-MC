package dev.linrtx.mc.gui;

import dev.linrtx.mc.compat.DistantHorizonsCompat;
import dev.linrtx.mc.compat.IrisCompat;
import dev.linrtx.mc.config.LinRtxConfig;
import dev.linrtx.mc.config.LinRtxEnums.*;
import dev.linrtx.mc.config.LinRtxPresetProfile;
import dev.linrtx.mc.natives.LinRtxNative;
import dev.linrtx.mc.render.LinRtxPipeline;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.components.AbstractSliderButton;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.network.chat.Component;

import java.util.List;
import java.util.Locale;
import java.util.function.Consumer;
import java.util.function.Function;

/**
 * Live in-game LinRTX Settings & Preset Switcher UI (opened via F8, ModMenu, or Sodium Video Settings).
 * Every control updates the active pipeline immediately with zero restart required.
 */
public final class LinRtxConfigScreen extends Screen {
    private enum Tab {
        PROFILES("Profiles"),
        DLSS("DLSS 4.5"),
        NEURAL("DLSS 5"),
        DENOISE_RR("Denoise & RR"),
        FRAME_GEN("Frame Gen"),
        RTX_HDR("RTX HDR");

        final String label;
        Tab(String label) { this.label = label; }
    }

    private final Screen parent;
    private Tab activeTab = Tab.DLSS;

    private int panelX;
    private int panelY;
    private int panelW;
    private int panelH;

    public LinRtxConfigScreen(Screen parent) {
        super(Component.translatable("linrtx.title"));
        this.parent = parent;
    }

    @Override
    protected void init() {
        LinRtxConfig cfg = LinRtxConfig.get();
        panelW = Math.min(560, Math.max(380, this.width - 32));
        panelH = Math.min(372, Math.max(280, this.height - 20));
        panelX = (this.width - panelW) / 2;
        panelY = (this.height - panelH) / 2;

        // Top navigation tabs
        Tab[] tabs = Tab.values();
        int tabGap = 4;
        int tabW = (panelW - 24 - tabGap * (tabs.length - 1)) / tabs.length;
        int tabY = panelY + 28;
        for (int i = 0; i < tabs.length; i++) {
            Tab tab = tabs[i];
            int tx = panelX + 12 + i * (tabW + tabGap);
            String label = (tab == activeTab ? "> " : "") + tab.label;
            Button btn = Button.builder(Component.literal(label), b -> {
                this.activeTab = tab;
                this.rebuildWidgets();
            }).bounds(tx, tabY, tabW, 20).build();
            btn.active = (tab != activeTab);
            addRenderableWidget(btn);
        }

        int contentY = panelY + 56;
        switch (activeTab) {
            case PROFILES -> buildProfilesTab(cfg, contentY);
            case DLSS -> buildDlssTab(cfg, contentY);
            case NEURAL -> buildNeuralTab(cfg, contentY);
            case DENOISE_RR -> buildDenoiseRrTab(cfg, contentY);
            case FRAME_GEN -> buildFrameGenTab(cfg, contentY);
            case RTX_HDR -> buildHdrTab(cfg, contentY);
        }

        // Bottom persistent controls
        int footY = panelY + panelH - 28;
        int half = (panelW - 36) / 2;
        addRenderableWidget(Button.builder(Component.literal("Reset Defaults"), b -> {
            LinRtxPresetProfile.applyProfile(cfg, LinRtxPresetProfile.PROFILES.get(1));
            cfg.hdrOutputMode = HdrOutputMode.SDR_BOOST;
            cfg.presentBackend = PresentBackend.OPENGL;
            cfg.customScale = 0.75f;
            cfg.autoMipBias = true;
            cfg.combineDistantHorizonsDepth = true;
            cfg.hudlessSplit = true;
            cfg.hdrDebanding = true;
            cfg.rrPreset = RrPreset.DEFAULT;
            cfg.gbufferGuideMode = GbufferGuideMode.AUTO;
            cfg.preTonemapIrisDenoise = true;
            cfg.denoiseTemporalBlend = 0.88f;
            cfg.denoiseStrength = 1.0f;
            cfg.denoiseAtrousPasses = 4;
            cfg.f1AccumFadeSamples = 48;
            cfg.neuralMode = NeuralEngineMode.OFF;
            cfg.neuralStyle = NeuralStyle.DEFAULT;
            cfg.neuralPreset = NeuralPreset.AUTO;
            cfg.neuralPlacement = NeuralPlacement.POST_SR;
            cfg.neuralUnlockAnyRtx = true;
            cfg.neuralUseAutoMask = true;
            cfg.neuralWorkingScale = 1.0f;
            cfg.neuralIntensity = 0.75f;
            cfg.neuralLocalToneStrength = 1.0f;
            cfg.neuralLocalStructureStrength = 1.0f;
            cfg.neuralSkinStructureStrength = -1.0f;
            cfg.save();
            LinRtxPipeline.get().requestHistoryReset();
            this.rebuildWidgets();
        }).bounds(panelX + 12, footY, half, 20).build());

        addRenderableWidget(Button.builder(Component.literal("Done"), b -> onClose())
            .bounds(panelX + 24 + half, footY, half, 20).build());
    }

    private void buildProfilesTab(LinRtxConfig cfg, int yStart) {
        int colW = panelW - 24;
        int y = yStart;
        for (LinRtxPresetProfile.Bundle bundle : LinRtxPresetProfile.PROFILES) {
            String txt = bundle.name() + "  [" + bundle.subtitle() + "]";
            addRenderableWidget(Button.builder(Component.literal(txt), b -> {
                LinRtxPresetProfile.applyProfile(cfg, bundle);
                LinRtxPipeline.get().requestHistoryReset();
                LinRtxHudOverlay.showToast("LinRTX Profile -> " + bundle.name());
                this.rebuildWidgets();
            }).bounds(panelX + 12, y, colW, 19).build());
            y += 22;
        }
    }

    private void buildDlssTab(LinRtxConfig cfg, int yStart) {
        int leftX = panelX + 12;
        int fullW = panelW - 24;
        int halfW = (fullW - 8) / 2;
        int rightX = leftX + halfW + 8;

        // Row 1: DLSS Mode & Auto Mip Bias
        addRenderableWidget(Button.builder(
            Component.literal("Quality Mode: " + cfg.dlssMode.displayName),
            b -> {
                cfg.dlssMode = nextEnum(cfg.dlssMode);
                cfg.save();
                LinRtxPipeline.get().requestHistoryReset();
                this.rebuildWidgets();
            }).bounds(leftX, yStart, halfW, 20)
            .tooltip(Tooltip.create(Component.literal("Switch DLSS 4.5 render resolution mode live.")))
            .build());

        addRenderableWidget(Button.builder(
            Component.literal("Auto LOD Mip Bias: " + (cfg.autoMipBias ? "ON" : "OFF")),
            b -> {
                cfg.autoMipBias = !cfg.autoMipBias;
                cfg.save();
                this.rebuildWidgets();
            }).bounds(rightX, yStart, halfW, 20)
            .tooltip(Tooltip.create(Component.literal("Applies official negative texture mip bias during DLSS upscaling for sharp blocks.")))
            .build());

        // Row 2: 1-click Live Preset Buttons (J, K, L, M, E, F, Default)
        List<DlssPreset> presets = List.of(
            DlssPreset.M, DlssPreset.L, DlssPreset.K, DlssPreset.J,
            DlssPreset.E, DlssPreset.F, DlssPreset.DEFAULT
        );
        int gap = 4;
        int pW = (fullW - gap * (presets.size() - 1)) / presets.size();
        int py = yStart + 36;
        for (int i = 0; i < presets.size(); i++) {
            DlssPreset p = presets.get(i);
            String shortTag = (p == DlssPreset.DEFAULT) ? "Auto" : ("Preset " + p.name());
            if (p == cfg.dlssPreset) {
                shortTag = "* " + shortTag;
            }
            Button pBtn = Button.builder(Component.literal(shortTag), b -> {
                cfg.dlssPreset = p;
                cfg.save();
                LinRtxPipeline.get().requestHistoryReset();
                LinRtxHudOverlay.showToast("LinRTX DLSS -> " + p.displayName);
                this.rebuildWidgets();
            }).bounds(leftX + i * (pW + gap), py, pW, 20)
                .tooltip(Tooltip.create(Component.literal(p.displayName)))
                .build();
            pBtn.active = (p != cfg.dlssPreset);
            addRenderableWidget(pBtn);
        }

        // Row 3: Custom Scale + Sharpness sliders
        int sy = yStart + 64;
        addRenderableWidget(new ParamSlider(
            leftX, sy, halfW, 20, 0.25, 1.00, cfg.customScale,
            v -> String.format(Locale.ROOT, "Custom Scale: %.2fx", v),
            v -> {
                cfg.customScale = v.floatValue();
                cfg.save();
                LinRtxPipeline.get().requestHistoryReset();
            }
        ));

        addRenderableWidget(new ParamSlider(
            rightX, sy, halfW, 20, 0.0, 1.0, cfg.sharpness,
            v -> String.format(Locale.ROOT, "CAS Sharpness: %d%%", Math.round(v * 100.0)),
            v -> {
                cfg.sharpness = v.floatValue();
                cfg.save();
            }
        ));

        // Row 4: Distant Horizons merge + Reset History button
        int dy = yStart + 90;
        addRenderableWidget(Button.builder(
            Component.literal("DH Depth Merge: " + (cfg.combineDistantHorizonsDepth ? "ON" : "OFF")),
            b -> {
                cfg.combineDistantHorizonsDepth = !cfg.combineDistantHorizonsDepth;
                cfg.save();
                this.rebuildWidgets();
            }).bounds(leftX, dy, halfW, 20)
            .tooltip(Tooltip.create(Component.literal("Merges Distant Horizons LOD depth with MC depth for accurate distant motion vectors.")))
            .build());

        addRenderableWidget(Button.builder(
            Component.literal("Reset Temporal History"),
            b -> LinRtxPipeline.get().requestHistoryReset()
        ).bounds(rightX, dy, halfW, 20).build());
    }

    private void buildFrameGenTab(LinRtxConfig cfg, int yStart) {
        int leftX = panelX + 12;
        int fullW = panelW - 24;
        int halfW = (fullW - 8) / 2;
        int rightX = leftX + halfW + 8;

        // 1-click FG Multiplier buttons (OFF, 2x, 3x, 4x)
        FrameGenMode[] fgModes = FrameGenMode.values();
        int gap = 6;
        int bw = (fullW - gap * (fgModes.length - 1)) / fgModes.length;
        for (int i = 0; i < fgModes.length; i++) {
            FrameGenMode m = fgModes[i];
            String tag = (m == cfg.fgMode ? "* " : "") + m.displayName;
            Button btn = Button.builder(Component.literal(tag), b -> {
                cfg.fgMode = m;
                cfg.save();
                LinRtxPipeline.get().requestHistoryReset();
                LinRtxHudOverlay.showToast("LinRTX Frame Gen -> " + m.displayName);
                this.rebuildWidgets();
            }).bounds(leftX + i * (bw + gap), yStart + 14, bw, 22).build();
            btn.active = (m != cfg.fgMode);
            addRenderableWidget(btn);
        }

        addRenderableWidget(Button.builder(
            Component.literal("HUDless UI Split: " + (cfg.hudlessSplit ? "ON" : "OFF")),
            b -> {
                cfg.hudlessSplit = !cfg.hudlessSplit;
                cfg.save();
                this.rebuildWidgets();
            }).bounds(leftX, yStart + 46, halfW, 20)
            .tooltip(Tooltip.create(Component.literal("Supplies world-only HUDless color to DLSS-G to prevent crosshairs/hotbar warping.")))
            .build());

        addRenderableWidget(Button.builder(
            Component.literal("Status HUD Pill: " + (cfg.showStatusOverlay ? "ON" : "OFF")),
            b -> {
                cfg.showStatusOverlay = !cfg.showStatusOverlay;
                cfg.save();
                this.rebuildWidgets();
            }).bounds(rightX, yStart + 46, halfW, 20).build());

        addRenderableWidget(Button.builder(
            Component.literal("Present Backend: " + cfg.presentBackend.displayName),
            b -> {
                cfg.presentBackend = nextEnum(cfg.presentBackend);
                cfg.save();
                this.rebuildWidgets();
            }).bounds(leftX, yStart + 74, fullW, 20)
            .tooltip(Tooltip.create(Component.literal("OpenGL Swap supports gamescope --hdr-enabled cleanly; Vulkan HDR10 uses direct X11 HDR swapchain.")))
            .build());
    }

    private void buildHdrTab(LinRtxConfig cfg, int yStart) {
        int leftX = panelX + 12;
        int fullW = panelW - 24;
        int halfW = (fullW - 8) / 2;
        int rightX = leftX + halfW + 8;

        addRenderableWidget(Button.builder(
            Component.literal("HDR Engine: " + cfg.hdrEngine.displayName),
            b -> {
                cfg.hdrEngine = nextEnum(cfg.hdrEngine);
                cfg.save();
                this.rebuildWidgets();
            }).bounds(leftX, yStart, halfW, 20).build());

        addRenderableWidget(Button.builder(
            Component.literal("Output Format: " + cfg.hdrOutputMode.displayName),
            b -> {
                cfg.hdrOutputMode = nextEnum(cfg.hdrOutputMode);
                cfg.save();
                this.rebuildWidgets();
            }).bounds(rightX, yStart, halfW, 20).build());

        addRenderableWidget(Button.builder(
            Component.literal("Tone Preset: " + cfg.hdrTonePreset.displayName),
            b -> {
                HdrTonePreset next = nextEnum(cfg.hdrTonePreset);
                if (next == HdrTonePreset.CUSTOM) {
                    next = HdrTonePreset.CINEMATIC;
                }
                cfg.applyHdrPreset(next);
                cfg.save();
                this.rebuildWidgets();
            }).bounds(leftX, yStart + 26, halfW, 20).build());

        addRenderableWidget(Button.builder(
            Component.literal("HDR Debanding: " + (cfg.hdrDebanding ? "ON" : "OFF")),
            b -> {
                cfg.hdrDebanding = !cfg.hdrDebanding;
                cfg.save();
                this.rebuildWidgets();
            }).bounds(rightX, yStart + 26, halfW, 20).build());

        addRenderableWidget(new ParamSlider(
            leftX, yStart + 54, halfW, 20, 50, 175, cfg.hdrContrast,
            v -> "Contrast: " + Math.round(v) + "%",
            v -> { cfg.hdrContrast = (int) Math.round(v); cfg.hdrTonePreset = HdrTonePreset.CUSTOM; cfg.save(); }
        ));
        addRenderableWidget(new ParamSlider(
            rightX, yStart + 54, halfW, 20, 50, 175, cfg.hdrSaturation,
            v -> "Saturation: " + Math.round(v) + "%",
            v -> { cfg.hdrSaturation = (int) Math.round(v); cfg.hdrTonePreset = HdrTonePreset.CUSTOM; cfg.save(); }
        ));
        addRenderableWidget(new ParamSlider(
            leftX, yStart + 80, halfW, 20, 10, 100, cfg.hdrMiddleGray,
            v -> "Middle Gray: " + Math.round(v),
            v -> { cfg.hdrMiddleGray = (int) Math.round(v); cfg.hdrTonePreset = HdrTonePreset.CUSTOM; cfg.save(); }
        ));
        addRenderableWidget(new ParamSlider(
            rightX, yStart + 80, halfW, 20, 250, 1500, cfg.hdrMaxLuminance,
            v -> "Peak Nits: " + Math.round(v),
            v -> { cfg.hdrMaxLuminance = (int) Math.round(v); cfg.hdrTonePreset = HdrTonePreset.CUSTOM; cfg.save(); }
        ));
        addRenderableWidget(new ParamSlider(
            leftX, yStart + 106, fullW, 20, 0.0, 1.5, cfg.hdrHighlightBoost,
            v -> String.format(Locale.ROOT, "RTX Highlight Boost: %.2f", v),
            v -> { cfg.hdrHighlightBoost = v.floatValue(); cfg.hdrTonePreset = HdrTonePreset.CUSTOM; cfg.save(); }
        ));
    }

    private void buildNeuralTab(LinRtxConfig cfg, int yStart) {
        int leftX = panelX + 12;
        int fullW = panelW - 24;
        int halfW = (fullW - 8) / 2;
        int rightX = leftX + halfW + 8;

        // Row 1: Neural Engine Mode & Any-RTX Card Unlock
        addRenderableWidget(Button.builder(
            Component.literal("Engine: " + cfg.neuralMode.displayName),
            b -> {
                cfg.neuralMode = nextEnum(cfg.neuralMode);
                cfg.save();
                LinRtxPipeline.get().requestHistoryReset();
                this.rebuildWidgets();
            }).bounds(leftX, yStart, halfW, 20)
            .tooltip(Tooltip.create(Component.literal(
                "Selects DLSS 5 Neural Engine Filter backend (NGX Feature 18 or Vulkan Neural Compute).")))
            .build());

        addRenderableWidget(Button.builder(
            Component.literal("Any-RTX Unlock: " + (cfg.neuralUnlockAnyRtx ? "ON (20/30/40/50)" : "OFF")),
            b -> {
                cfg.neuralUnlockAnyRtx = !cfg.neuralUnlockAnyRtx;
                cfg.save();
                LinRtxPipeline.get().requestHistoryReset();
                this.rebuildWidgets();
            }).bounds(rightX, yStart, halfW, 20)
            .tooltip(Tooltip.create(Component.literal(
                "Unlocks DLSS 5 Neural Engine across RTX 20/30/40/50 (NGX arch hints + Vulkan Neural Engine).")))
            .build());

        // Row 2: 1-Click Style Buttons (Default / Natural / Cinematic)
        NeuralStyle[] styles = NeuralStyle.values();
        int gap = 4;
        int sw = (fullW - gap * (styles.length - 1)) / styles.length;
        int styleY = yStart + 35;
        for (int i = 0; i < styles.length; i++) {
            NeuralStyle st = styles[i];
            String tag = (st == cfg.neuralStyle ? "* " : "") + "Style: " + st.displayName;
            Button sBtn = Button.builder(Component.literal(tag), b -> {
                cfg.neuralStyle = st;
                cfg.save();
                LinRtxHudOverlay.showToast("LinRTX DLSS 5 Style -> " + st.displayName);
                this.rebuildWidgets();
            }).bounds(leftX + i * (sw + gap), styleY, sw, 20).build();
            sBtn.active = (st != cfg.neuralStyle);
            addRenderableWidget(sBtn);
        }

        // Row 3: Pipeline Placement & Model Preset
        int row3Y = yStart + 59;
        addRenderableWidget(Button.builder(
            Component.literal("Placement: " + cfg.neuralPlacement.displayName),
            b -> {
                cfg.neuralPlacement = nextEnum(cfg.neuralPlacement);
                cfg.save();
                LinRtxPipeline.get().requestHistoryReset();
                this.rebuildWidgets();
            }).bounds(leftX, row3Y, halfW, 20)
            .tooltip(Tooltip.create(Component.literal(
                "Post-SR runs on display-res world pre-HUD; Pre-SR runs at render resolution before DLSS-SR.")))
            .build());

        addRenderableWidget(Button.builder(
            Component.literal("Model Preset: " + cfg.neuralPreset.displayName),
            b -> {
                cfg.neuralPreset = nextEnum(cfg.neuralPreset);
                cfg.save();
                LinRtxPipeline.get().requestHistoryReset();
                this.rebuildWidgets();
            }).bounds(rightX, row3Y, halfW, 20)
            .tooltip(Tooltip.create(Component.literal(
                "DLSSNR.Hint.Render.Preset: Auto, Quality (HT / 5x5), or Small FP8 (3x3 Fast).")))
            .build());

        // Row 4: Intensity & Working Scale (Cost Scaler)
        int row4Y = yStart + 84;
        addRenderableWidget(new ParamSlider(
            leftX, row4Y, halfW, 20, 0.0, 2.0, cfg.neuralIntensity,
            v -> String.format(Locale.ROOT, "Intensity: %d%%", Math.round(v * 100.0)),
            v -> { cfg.neuralIntensity = v.floatValue(); cfg.save(); }
        ));
        addRenderableWidget(new ParamSlider(
            rightX, row4Y, halfW, 20, 0.50, 1.00, cfg.neuralWorkingScale,
            v -> String.format(Locale.ROOT, "Working Scale: %.2fx", v),
            v -> { cfg.neuralWorkingScale = v.floatValue(); cfg.save(); }
        ));

        // Row 5: Local Tone Strength & Local Structure Strength
        int row5Y = yStart + 108;
        addRenderableWidget(new ParamSlider(
            leftX, row5Y, halfW, 20, 0.0, 2.0, cfg.neuralLocalToneStrength,
            v -> String.format(Locale.ROOT, "Local Tone: %d%%", Math.round(v * 100.0)),
            v -> { cfg.neuralLocalToneStrength = v.floatValue(); cfg.save(); }
        ));
        addRenderableWidget(new ParamSlider(
            rightX, row5Y, halfW, 20, 0.0, 2.0, cfg.neuralLocalStructureStrength,
            v -> String.format(Locale.ROOT, "Local Struct: %d%%", Math.round(v * 100.0)),
            v -> { cfg.neuralLocalStructureStrength = v.floatValue(); cfg.save(); }
        ));

        // Row 6: Skin Structure Strength & Auto Mask
        int row6Y = yStart + 132;
        double initialSkinSlider = (cfg.neuralSkinStructureStrength < -0.5f)
            ? -0.25
            : cfg.neuralSkinStructureStrength;
        addRenderableWidget(new ParamSlider(
            leftX, row6Y, halfW, 20, -0.25, 2.0, initialSkinSlider,
            v -> (v < 0.0)
                ? "Skin Struct: Auto"
                : String.format(Locale.ROOT, "Skin Struct: %d%%", Math.round(v * 100.0)),
            v -> {
                cfg.neuralSkinStructureStrength = (v < 0.0) ? -1.0f : v.floatValue();
                cfg.save();
            }
        ));
        addRenderableWidget(Button.builder(
            Component.literal("Auto Mask (Skin/Sky): " + (cfg.neuralUseAutoMask ? "ON" : "OFF")),
            b -> {
                cfg.neuralUseAutoMask = !cfg.neuralUseAutoMask;
                cfg.save();
                this.rebuildWidgets();
            }).bounds(rightX, row6Y, halfW, 20)
            .tooltip(Tooltip.create(Component.literal(
                "Enables DLSSNR.UseAutoMask heuristic protection for sky gradients, motion edges, and skin tones.")))
            .build());
    }

    private void buildDenoiseRrTab(LinRtxConfig cfg, int yStart) {
        int leftX = panelX + 12;
        int fullW = panelW - 24;
        int halfW = (fullW - 8) / 2;
        int rightX = leftX + halfW + 8;

        // Row 1: SVGF Denoiser Mode & DLSS Ray Reconstruction Mode
        addRenderableWidget(Button.builder(
            Component.literal("Denoise: " + cfg.denoiseMode.name()),
            b -> {
                cfg.denoiseMode = nextEnum(cfg.denoiseMode);
                cfg.save();
                LinRtxPipeline.get().requestHistoryReset();
                this.rebuildWidgets();
            }).bounds(leftX, yStart, halfW, 20)
            .tooltip(Tooltip.create(Component.literal("Vulkan compute SVGF + À-Trous denoiser (Active: " + cfg.denoiseMode.displayName + ")")))
            .build());

        addRenderableWidget(Button.builder(
            Component.literal("DLSS-RR: " + cfg.rrMode.name()),
            b -> {
                cfg.rrMode = nextEnum(cfg.rrMode);
                cfg.save();
                LinRtxPipeline.get().requestHistoryReset();
                this.rebuildWidgets();
            }).bounds(rightX, yStart, halfW, 20)
            .tooltip(Tooltip.create(Component.literal(cfg.rrMode.displayName)))
            .build());

        // Row 2: 1-click Live DLSS-RR Preset Buttons (Auto, D, E, F)
        List<RrPreset> rrPresets = List.of(RrPreset.DEFAULT, RrPreset.D, RrPreset.E, RrPreset.F);
        int gap = 4;
        int pW = (fullW - gap * (rrPresets.size() - 1)) / rrPresets.size();
        int py = yStart + 34;
        for (int i = 0; i < rrPresets.size(); i++) {
            RrPreset p = rrPresets.get(i);
            String label = (p == RrPreset.DEFAULT) ? "RR Auto" : ("RR Preset " + p.name());
            if (p == cfg.rrPreset) {
                label = "* " + label;
            }
            Button pBtn = Button.builder(Component.literal(label), b -> {
                cfg.rrPreset = p;
                cfg.save();
                LinRtxHudOverlay.showToast("LinRTX DLSS-RR -> " + p.displayName);
                this.rebuildWidgets();
            }).bounds(leftX + i * (pW + gap), py, pW, 20)
                .tooltip(Tooltip.create(Component.literal(p.displayName)))
                .build();
            pBtn.active = (p != cfg.rrPreset);
            addRenderableWidget(pBtn);
        }

        // Row 3: Pre-Tonemap Path-Tracing Denoise/RR & G-Buffer Guide Source
        int gy = yStart + 60;
        addRenderableWidget(Button.builder(
            Component.literal("Pre-Tonemap PT: " + (cfg.preTonemapIrisDenoise ? "ON" : "OFF")),
            b -> {
                cfg.preTonemapIrisDenoise = !cfg.preTonemapIrisDenoise;
                cfg.save();
                LinRtxPipeline.get().requestHistoryReset();
                this.rebuildWidgets();
            }).bounds(leftX, gy, halfW, 20)
            .tooltip(Tooltip.create(Component.literal("Runs Pre-Tonemap DLSS-RR / SVGF directly on linear HDR colortex5 (e.g. Camellia PT) before Bloom & AgX.")))
            .build());

        addRenderableWidget(Button.builder(
            Component.literal("Guides: " + cfg.gbufferGuideMode.name()),
            b -> {
                cfg.gbufferGuideMode = nextEnum(cfg.gbufferGuideMode);
                cfg.save();
                this.rebuildWidgets();
            }).bounds(rightX, gy, halfW, 20)
            .tooltip(Tooltip.create(Component.literal(cfg.gbufferGuideMode.displayName)))
            .build());

        // Row 4: Temporal Blend & Denoise Strength sliders
        int sy = yStart + 84;
        addRenderableWidget(new ParamSlider(
            leftX, sy, halfW, 20, 0.0, 0.96, cfg.denoiseTemporalBlend,
            v -> String.format(Locale.ROOT, "Temporal Blend: %d%%", Math.round(v * 100.0)),
            v -> { cfg.denoiseTemporalBlend = v.floatValue(); cfg.save(); }
        ));
        addRenderableWidget(new ParamSlider(
            rightX, sy, halfW, 20, 0.25, 2.50, cfg.denoiseStrength,
            v -> String.format(Locale.ROOT, "Spatial Strength: %.2f", v),
            v -> { cfg.denoiseStrength = v.floatValue(); cfg.save(); }
        ));

        // Row 5: Atrous Wavelet Passes & Reset Denoiser History
        int ay = yStart + 108;
        addRenderableWidget(new ParamSlider(
            leftX, ay, halfW, 20, 1, 5, cfg.denoiseAtrousPasses,
            v -> "À-Trous Passes: " + Math.round(v),
            v -> { cfg.denoiseAtrousPasses = (int) Math.round(v); cfg.save(); }
        ));
        addRenderableWidget(Button.builder(
            Component.literal("Reset Denoiser History"),
            b -> LinRtxPipeline.get().requestHistoryReset()
        ).bounds(rightX, ay, halfW, 20).build());

        // Row 6: Camellia F1 Progressive Accumulation Target
        int fy = yStart + 132;
        addRenderableWidget(new ParamSlider(
            leftX, fy, fullW, 20, 0, 256, cfg.f1AccumFadeSamples,
            v -> {
                int spp = (int) Math.round(v);
                return (spp <= 1)
                    ? "F1 Screenshot Accum Blend: Always 100% Denoised"
                    : ("F1 Screenshot Accum Target: " + spp + " SPP (Fade Denoise -> Raw MC)");
            },
            v -> { cfg.f1AccumFadeSamples = (int) Math.round(v); cfg.save(); }
        ));
    }

    @Override
    public void extractRenderState(GuiGraphicsExtractor gui, int mouseX, int mouseY, float delta) {
        gui.fill(panelX, panelY, panelX + panelW, panelY + panelH, 0xD80D1117);
        gui.fill(panelX, panelY, panelX + panelW, panelY + 24, 0xFF1A2418);
        gui.outline(panelX, panelY, panelW, panelH, 0xFF76B900);

        super.extractRenderState(gui, mouseX, mouseY, delta);

        gui.centeredText(this.font, this.title, this.width / 2, panelY + 8, 0xFF9BE628);

        LinRtxConfig cfg = LinRtxConfig.get();
        if (activeTab == Tab.DLSS) {
            gui.text(this.font, "DLSS 4.5 / 4.0 Transformer Preset (Live Switch):", panelX + 14, panelY + 80, 0xFFB8D0C0, false);
        } else if (activeTab == Tab.NEURAL) {
            gui.text(this.font, "DLSS 5 Neural Engine Filter Style (Default / Natural / Cinematic):", panelX + 14, panelY + 80, 0xFFB8D0C0, false);
        } else if (activeTab == Tab.DENOISE_RR) {
            gui.text(this.font, "DLSS Ray Reconstruction Preset (Live Switch):", panelX + 14, panelY + 80, 0xFFB8D0C0, false);
        } else if (activeTab == Tab.FRAME_GEN) {
            gui.text(this.font, "DLSS-G Optical Flow Frame Generation Multiplier:", panelX + 14, panelY + 58, 0xFFB8D0C0, false);
        }

        // Live GPU / NGX Telemetry footer box inside panel
        int statY = panelY + panelH - 102;
        int statH = 66;
        gui.fill(panelX + 12, statY, panelX + panelW - 12, statY + statH, 0xCC111916);
        gui.outline(panelX + 12, statY, panelW - 24, statH, 0xFF33523D);

        LinRtxPipeline pipe = LinRtxPipeline.get();
        LinRtxNative.StatusSnapshot st = pipe.statusSnapshot();
        String archTag = (!st.gpuArchName().isEmpty() && !st.gpuArchName().equals("-"))
            ? (" [" + st.gpuArchName() + "]")
            : "";
        String gpuLine = "GPU: " + st.gpuName() + archTag
            + (st.driverVersion().equals("-") ? "" : (" | Driver " + st.driverVersion()));
        String ngxLine = String.format(Locale.ROOT,
            "NGX: %s | Preset: %s (RR: %s) | %dx%d -> %dx%d | %s",
            st.ngxReady() ? "Ready" : "Standby",
            cfg.dlssPreset.name(),
            RrPreset.fromNativeId(st.activeRrPreset()).name(),
            pipe.currentRenderWidth(), pipe.currentRenderHeight(),
            pipe.currentDisplayWidth(), pipe.currentDisplayHeight(),
            st.swapchainStatus());
        String rrState = st.dlssdActive() ? "ON" : (st.dlssdSupported() ? "OK" : "N/A");
        String nrState = st.dlssnrActive()
            ? (st.dlssnrUsingNgx() ? "F18" : "VK")
            : (cfg.neuralMode != NeuralEngineMode.OFF ? "WAIT" : "OFF");
        String denoiseState = st.denoiserActive() ? st.denoiserStatus() : cfg.denoiseMode.name();
        String compatLine = String.format(Locale.ROOT,
            "DLSS=%s RR=%s FG=%s(%dx) NR=%s | Denoise=%s | HDR=%s | Iris=%s DH=%s",
            st.dlssSupported() ? "OK" : "N/A",
            rrState,
            st.dlssgSupported() ? "OK" : "N/A",
            st.maxFgMultiplier(),
            nrState,
            denoiseState,
            st.trueHdrStatus(),
            IrisCompat.isLoaded() ? (IrisCompat.isPackInUse() ? "Active" : "Yes") : "No",
            DistantHorizonsCompat.isLoaded() ? "Yes" : "No");

        gui.text(this.font, trimToWidth(gpuLine, panelW - 36), panelX + 18, statY + 7, 0xFF9BE628, false);
        gui.text(this.font, trimToWidth(ngxLine, panelW - 36), panelX + 18, statY + 21, 0xFFD9E6DF, false);
        gui.text(this.font, trimToWidth(compatLine, panelW - 36), panelX + 18, statY + 35, 0xFFAABCB1, false);
        if (!st.lastErrorMsg().isEmpty()) {
            gui.text(this.font, trimToWidth("Note: " + st.lastErrorMsg(), panelW - 36), panelX + 18, statY + 49, 0xFFFFB070, false);
        } else if (activeTab == Tab.NEURAL || st.dlssnrActive()) {
            gui.text(this.font, trimToWidth("DLSS 5 NR: " + st.dlssnrStatus(), panelW - 36), panelX + 18, statY + 49, 0xFF8BE8A8, false);
        }
    }

    @Override
    public void onClose() {
        LinRtxConfig.get().save();
        if (this.minecraft != null) {
            this.minecraft.gui.setScreen(this.parent);
        }
    }

    private String trimToWidth(String text, int maxPx) {
        if (this.font.width(text) <= maxPx) {
            return text;
        }
        String t = text;
        while (t.length() > 4 && this.font.width(t + "...") > maxPx) {
            t = t.substring(0, t.length() - 1);
        }
        return t + "...";
    }

    private static <E extends Enum<E>> E nextEnum(E cur) {
        @SuppressWarnings("unchecked")
        E[] vals = (E[]) cur.getDeclaringClass().getEnumConstants();
        return vals[(cur.ordinal() + 1) % vals.length];
    }

    private static final class ParamSlider extends AbstractSliderButton {
        private final double minVal;
        private final double maxVal;
        private final Function<Double, String> formatter;
        private final Consumer<Double> onChange;

        ParamSlider(int x, int y, int w, int h, double minVal, double maxVal, double initial,
                    Function<Double, String> formatter, Consumer<Double> onChange) {
            super(x, y, w, h, Component.literal(formatter.apply(initial)),
                Math.max(0.0, Math.min(1.0, (initial - minVal) / (maxVal - minVal))));
            this.minVal = minVal;
            this.maxVal = maxVal;
            this.formatter = formatter;
            this.onChange = onChange;
            updateMessage();
        }

        private double currentActual() {
            return minVal + this.value * (maxVal - minVal);
        }

        @Override
        protected void updateMessage() {
            setMessage(Component.literal(formatter.apply(currentActual())));
        }

        @Override
        protected void applyValue() {
            onChange.accept(currentActual());
        }
    }
}
