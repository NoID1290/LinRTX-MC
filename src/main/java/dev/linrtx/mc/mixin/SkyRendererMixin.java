package dev.linrtx.mc.mixin;

import com.llamalad7.mixinextras.injector.ModifyExpressionValue;
import com.mojang.blaze3d.pipeline.RenderTarget;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.SkyRenderer;
import org.objectweb.asm.Opcodes;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * SkyRenderer caches {@code this.gameRenderer.mainRenderTarget()} once at LevelRenderer construction.
 * When LinRTX switches presets or DLSS modes at runtime, {@code TargetManager} destroys and
 * recreates the scaled world {@code MainTarget}, leaving that cached field pointing at freed
 * GPU textures (null {@code colorTextureView}). Resolve {@code mainRenderTarget()} dynamically.
 */
@Mixin(SkyRenderer.class)
public class SkyRendererMixin {

    @ModifyExpressionValue(
        method = {
            "renderSkyDisc(I)V",
            "renderDarkDisc()V",
            "renderSun(FLcom/mojang/blaze3d/vertex/PoseStack;)V",
            "renderMoon(Lnet/minecraft/world/level/MoonPhase;FLcom/mojang/blaze3d/vertex/PoseStack;)V",
            "renderStars(FLcom/mojang/blaze3d/vertex/PoseStack;)V",
            "renderSunriseAndSunset(Lcom/mojang/blaze3d/vertex/PoseStack;FI)V",
            "renderEndSky()V",
            "renderEndFlash(Lcom/mojang/blaze3d/vertex/PoseStack;FFF)V"
        },
        at = @At(
            value = "FIELD",
            target = "Lnet/minecraft/client/renderer/SkyRenderer;renderTarget:Lcom/mojang/blaze3d/pipeline/RenderTarget;",
            opcode = Opcodes.GETFIELD
        )
    )
    private RenderTarget linrtx$useActiveMainRenderTarget(RenderTarget original) {
        Minecraft mc = Minecraft.getInstance();
        if (mc != null && mc.gameRenderer != null) {
            RenderTarget current = mc.gameRenderer.mainRenderTarget();
            if (current != null && current.getColorTextureView() != null) {
                return current;
            }
        }
        return original;
    }
}
