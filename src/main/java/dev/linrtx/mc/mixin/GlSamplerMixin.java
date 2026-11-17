package dev.linrtx.mc.mixin;

import com.mojang.blaze3d.opengl.GlSampler;
import com.mojang.blaze3d.textures.AddressMode;
import com.mojang.blaze3d.textures.FilterMode;
import dev.linrtx.mc.render.LinRtxPipeline;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

import java.util.OptionalDouble;

/**
 * Tracks mipmapped {@link GlSampler} instances so LinRTX can dynamically apply
 * NVIDIA's negative LOD bias ({@code log2(renderW/displayW) - 1.0}) during DLSS upscaling.
 */
@Mixin(GlSampler.class)
public abstract class GlSamplerMixin {
    @Shadow
    @Final
    private int id;

    @Shadow
    @Final
    private OptionalDouble maxLod;

    @Inject(method = "<init>", at = @At("RETURN"))
    private void linrtx$onInit(
        AddressMode u,
        AddressMode v,
        FilterMode minFilter,
        FilterMode magFilter,
        int maxAnisotropy,
        OptionalDouble maxLod,
        CallbackInfo ci
    ) {
        if (this.maxLod.isEmpty() || this.maxLod.getAsDouble() > 0.0) {
            LinRtxPipeline.get().registerMipmappedSampler(this.id);
        }
    }

    @Inject(method = "close()V", at = @At("HEAD"))
    private void linrtx$onClose(CallbackInfo ci) {
        LinRtxPipeline.get().unregisterMipmappedSampler(this.id);
    }
}
