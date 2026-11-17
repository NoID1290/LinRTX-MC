package dev.linrtx.mc;

import com.mojang.blaze3d.platform.InputConstants;
import dev.linrtx.mc.compat.DistantHorizonsCompat;
import dev.linrtx.mc.config.LinRtxConfig;
import dev.linrtx.mc.config.LinRtxEnums.DlssPreset;
import dev.linrtx.mc.config.LinRtxPresetProfile;
import dev.linrtx.mc.gui.LinRtxConfigScreen;
import dev.linrtx.mc.gui.LinRtxHudOverlay;
import dev.linrtx.mc.render.LinRtxPipeline;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientLifecycleEvents;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.client.keymapping.v1.KeyMappingHelper;
import net.fabricmc.fabric.api.client.rendering.v1.hud.HudElementRegistry;
import net.minecraft.client.KeyMapping;
import net.minecraft.resources.Identifier;
import org.lwjgl.glfw.GLFW;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * Fabric client initializer for LinRTX-MC on Minecraft 26.2 Linux.
 */
public final class LinRtxClientMod implements ClientModInitializer {
    private static final Logger LOGGER = LoggerFactory.getLogger("LinRTX");

    private static KeyMapping openMenuKey;
    private static KeyMapping cyclePresetKey;
    private static KeyMapping toggleOverlayKey;

    @Override
    public void onInitializeClient() {
        LinRtxConfig.get();
        DistantHorizonsCompat.ensureRegistered();

        KeyMapping.Category category = KeyMapping.Category.register(
            Identifier.fromNamespaceAndPath("linrtx", "main"));

        openMenuKey = KeyMappingHelper.registerKeyMapping(new KeyMapping(
            "key.linrtx.open_menu",
            InputConstants.Type.KEYSYM,
            GLFW.GLFW_KEY_F8,
            category
        ));

        cyclePresetKey = KeyMappingHelper.registerKeyMapping(new KeyMapping(
            "key.linrtx.cycle_preset",
            InputConstants.Type.KEYSYM,
            GLFW.GLFW_KEY_F9,
            category
        ));

        toggleOverlayKey = KeyMappingHelper.registerKeyMapping(new KeyMapping(
            "key.linrtx.toggle_overlay",
            InputConstants.Type.KEYSYM,
            GLFW.GLFW_KEY_F7,
            category
        ));

        HudElementRegistry.addLast(
            Identifier.fromNamespaceAndPath("linrtx", "status_overlay"),
            LinRtxHudOverlay::extractRenderState
        );

        ClientTickEvents.END_CLIENT_TICK.register(client -> {
            while (openMenuKey.consumeClick()) {
                if (!(client.gui.screen() instanceof LinRtxConfigScreen)) {
                    client.gui.setScreen(new LinRtxConfigScreen(client.gui.screen()));
                }
            }
            while (cyclePresetKey.consumeClick()) {
                DlssPreset next = LinRtxPresetProfile.cycleNextPreset(LinRtxConfig.get());
                LinRtxHudOverlay.showToast("LinRTX Preset -> " + next.displayName);
            }
            while (toggleOverlayKey.consumeClick()) {
                LinRtxConfig cfg = LinRtxConfig.get();
                cfg.showStatusOverlay = !cfg.showStatusOverlay;
                cfg.save();
                LinRtxHudOverlay.showToast("LinRTX Overlay: " + (cfg.showStatusOverlay ? "Shown" : "Hidden"));
            }
        });

        ClientLifecycleEvents.CLIENT_STOPPING.register(client -> LinRtxPipeline.get().shutdown());
        LOGGER.info("LinRTX-MC initialized (Press F8 for Settings, F9 to Cycle DLSS 4.5 Presets J/K/L/M)");
    }
}
