package dev.linrtx.mc.mixin;

import com.mojang.blaze3d.buffers.GpuBufferSlice;
import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.resource.ResourceHandle;
import dev.linrtx.mc.render.LinRtxPipeline;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.feature.FeatureRenderDispatcher;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Snapshots world depth right before {@code LevelRenderer.lambda$addAlwaysOnTopPass$0} clears
 * the main render target's depth buffer to 0.0 when Iris shaderpacks are inactive.
 */
@Mixin(value = LevelRenderer.class, priority = 900)
public abstract class LevelRendererMixin {
    @Inject(
        method = "lambda$addAlwaysOnTopPass$0(Lcom/mojang/blaze3d/buffers/GpuBufferSlice;Lcom/mojang/blaze3d/resource/ResourceHandle;Lnet/minecraft/client/renderer/feature/FeatureRenderDispatcher$PreparedFrame;)V",
        at = @At("HEAD")
    )
    private void linrtx$beforeAlwaysOnTopClear(
        GpuBufferSlice fogBuffer,
        ResourceHandle<RenderTarget> mainHandle,
        FeatureRenderDispatcher.PreparedFrame preparedFrame,
        CallbackInfo ci
    ) {
        if (mainHandle != null && LinRtxPipeline.get().hasActiveWorldFrame()) {
            LinRtxPipeline.get().captureWorldDepthSnapshot(mainHandle.get());
        }
    }
}
