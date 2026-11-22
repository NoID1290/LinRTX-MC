package dev.linrtx.mc.gui;

import dev.linrtx.mc.compat.DistantHorizonsCompat;
import dev.linrtx.mc.compat.IrisCompat;
import dev.linrtx.mc.config.LinRtxConfig;
import dev.linrtx.mc.config.LinRtxEnums.*;
import dev.linrtx.mc.natives.LinRtxNative;
import dev.linrtx.mc.render.LinRtxPipeline;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.Font;
import net.minecraft.client.gui.GuiGraphicsExtractor;

/**
 * Unobtrusive top-left HUD telemetry pill and live preset switch toast notification.
 */
public final class LinRtxHudOverlay {
    private static String toastText = "";
    private static long toastUntilMs = 0L;

    private LinRtxHudOverlay() {}

    public static void showToast(String message) {
        toastText = message != null ? message : "";
        toastUntilMs = System.currentTimeMillis() + 2600L;
    }

    public static void extractRenderState(GuiGraphicsExtractor gui, DeltaTracker deltaTracker) {
        Minecraft mc = Minecraft.getInstance();
        if (mc == null) {
            return;
        }
        LinRtxConfig cfg = LinRtxConfig.get();
        Font font = mc.font;
        long now = System.currentTimeMillis();

        if (!toastText.isEmpty() && now < toastUntilMs) {
            int tw = font.width(toastText) + 12;
            int tx = (gui.guiWidth() - tw) / 2;
            int ty = 12;
            gui.fill(tx, ty, tx + tw, ty + 16, 0xD0111612);
            gui.outline(tx, ty, tw, 16, 0xFF76B900);
            gui.centeredText(font, toastText, gui.guiWidth() / 2, ty + 4, 0xFF9BE628);
        }

        if (!cfg.showStatusOverlay || mc.gui.screen() != null) {
            return;
        }

        LinRtxPipeline pipe = LinRtxPipeline.get();
        LinRtxNative.StatusSnapshot st = pipe.statusSnapshot();
        StringBuilder sb = new StringBuilder("LinRTX ");
        if (st.dlssdActive()) {
            sb.append("DLSS-RR ").append(RrPreset.fromNativeId(st.activeRrPreset()).name());
            if (pipe.currentRenderWidth() > 0 && pipe.currentDisplayWidth() > 0) {
                sb.append(' ').append(pipe.currentRenderWidth()).append('x').append(pipe.currentRenderHeight())
                  .append("->").append(pipe.currentDisplayWidth()).append('x').append(pipe.currentDisplayHeight());
            }
        } else if (cfg.dlssMode != DlssMode.OFF) {
            sb.append("DLSS ").append(cfg.dlssPreset.name());
            if (pipe.currentRenderWidth() > 0 && pipe.currentDisplayWidth() > 0) {
                sb.append(' ').append(pipe.currentRenderWidth()).append('x').append(pipe.currentRenderHeight())
                  .append("->").append(pipe.currentDisplayWidth()).append('x').append(pipe.currentDisplayHeight());
            }
        } else {
            sb.append("Native");
        }
        if (st.denoiserActive() && !st.dlssdActive()) {
            sb.append(" | SVGF");
        } else if (cfg.denoiseMode != DenoiseMode.OFF && !st.dlssdActive()) {
            sb.append(" | SVGF");
        }
        if (st.dlssnrActive()) {
            sb.append(" | DLSS5-NR(").append(st.dlssnrUsingNgx() ? "F18" : "VK").append(')');
        } else if (cfg.neuralMode != NeuralEngineMode.OFF) {
            sb.append(" | DLSS5-NR");
        }
        if (cfg.fgMode != FrameGenMode.OFF) {
            sb.append(" | FG ").append(cfg.fgMode.multiplier).append('x');
            if (!st.dlssgActive() && st.vkReady() && !st.dlssgSupported()) {
                sb.append("(n/a)");
            }
        }
        if (cfg.hdrEngine != HdrEngineMode.OFF) {
            sb.append(" | ");
            if (st.trueHdrActive()) sb.append("TrueHDR");
            else if (st.hdrShaderActive()) sb.append("RTX HDR");
            else sb.append("HDR");
        }
        if (IrisCompat.isPackInUse()) {
            sb.append(" | Iris");
        }
        if (DistantHorizonsCompat.isLoaded() && cfg.combineDistantHorizonsDepth) {
            sb.append("+DH");
        }

        String line = sb.toString();
        int padX = 5;
        int w = font.width(line) + padX * 2;
        int x = 6;
        int y = 6;
        gui.fill(x, y, x + w, y + 14, 0xA8101412);
        gui.outline(x, y, w, 14, (st.dlssActive() || st.dlssdActive() || st.denoiserActive() || st.dlssnrActive()) ? 0xFF76B900 : 0xFF607068);
        gui.text(font, line, x + padX, y + 3, 0xFFE6F4EA, false);
    }
}
