package dev.linrtx.mc.compat;

import dev.linrtx.mc.gui.LinRtxConfigScreen;
import net.caffeinemc.mods.sodium.api.config.ConfigEntryPoint;
import net.caffeinemc.mods.sodium.api.config.structure.ConfigBuilder;
import net.minecraft.client.Minecraft;
import net.minecraft.network.chat.Component;

/**
 * Registers the "LinRTX" tab inside Sodium 0.9.x Video Settings so users can
 * open LinRTX-MC preset and HDR settings directly from Sodium's Options menu.
 */
public final class SodiumCompat implements ConfigEntryPoint {
    @Override
    public void registerConfigLate(ConfigBuilder builder) {
        builder.registerOwnModOptions()
                .setName("LinRTX")
                .setColorTheme(builder.createColorTheme().setBaseThemeRGB(0x76B900))
                .addPage(builder.createExternalPage()
                        .setName(Component.translatable("linrtx.sodium.page"))
                        .setScreenConsumer(parent ->
                                Minecraft.getInstance().gui.setScreen(new LinRtxConfigScreen(parent))));
    }
}
