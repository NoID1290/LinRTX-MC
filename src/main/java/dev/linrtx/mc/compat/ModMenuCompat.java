package dev.linrtx.mc.compat;

import com.terraformersmc.modmenu.api.ConfigScreenFactory;
import com.terraformersmc.modmenu.api.ModMenuApi;
import dev.linrtx.mc.gui.LinRtxConfigScreen;

/**
 * Registers the LinRTX-MC configuration screen with ModMenu.
 */
public final class ModMenuCompat implements ModMenuApi {
    @Override
    public ConfigScreenFactory<?> getModConfigScreenFactory() {
        return LinRtxConfigScreen::new;
    }
}
