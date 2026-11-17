package dev.linrtx.mc.mixin;

import net.fabricmc.loader.api.FabricLoader;
import org.objectweb.asm.tree.ClassNode;
import org.spongepowered.asm.mixin.extensibility.IMixinConfigPlugin;
import org.spongepowered.asm.mixin.extensibility.IMixinInfo;

import java.util.List;
import java.util.Set;

/**
 * Gates optional mod-integration mixins (e.g. Iris) so LinRTX initializes
 * cleanly when those optional mods are not installed.
 */
public final class LinRtxMixinPlugin implements IMixinConfigPlugin {
    private boolean irisLoaded;

    @Override
    public void onLoad(String mixinPackage) {
        this.irisLoaded = FabricLoader.getInstance().isModLoaded("iris");
    }

    @Override
    public String getRefMapperConfig() {
        return null;
    }

    @Override
    public boolean shouldApplyMixin(String targetClassName, String mixinClassName) {
        if (mixinClassName.endsWith(".IrisCompositeMixin")
                || mixinClassName.endsWith(".IrisColorSpaceMixin")) {
            return irisLoaded;
        }
        return true;
    }

    @Override
    public void acceptTargets(Set<String> myTargets, Set<String> otherTargets) {}

    @Override
    public List<String> getMixins() {
        return null;
    }

    @Override
    public void preApply(String targetClassName, ClassNode targetClass, String mixinClassName, IMixinInfo mixinInfo) {}

    @Override
    public void postApply(String targetClassName, ClassNode targetClass, String mixinClassName, IMixinInfo mixinInfo) {}
}
