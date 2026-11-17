package dev.linrtx.mc.mixin;

import com.mojang.blaze3d.opengl.GlSurface;
import com.mojang.blaze3d.systems.GpuSurface;
import dev.linrtx.mc.render.LinRtxPipeline;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Intercepts {@link GlSurface#present()} to run Stage-2 RTX HDR + DLSS-G 2x/3x/4x
 * Frame Generation and drive OpenGL swap or Vulkan HDR10 swapchain cadence.
 */
@Mixin(GlSurface.class)
public abstract class GlSurfaceMixin {
    @Shadow
    @Final
    private long windowHandle;

    @Shadow
    private int swapchainWidth;

    @Shadow
    private int swapchainHeight;

    @Inject(method = "configure(Lcom/mojang/blaze3d/systems/GpuSurface$Configuration;)V", at = @At("TAIL"))
    private void linrtx$onConfigure(GpuSurface.Configuration config, CallbackInfo ci) {
        LinRtxPipeline.get().onWindowResize(this.swapchainWidth, this.swapchainHeight);
    }

    @Inject(method = "present()V", at = @At("HEAD"), cancellable = true)
    private void linrtx$onPresent(CallbackInfo ci) {
        if (LinRtxPipeline.get().presentFrame(this.windowHandle, this.swapchainWidth, this.swapchainHeight)) {
            ci.cancel();
        }
    }
}
