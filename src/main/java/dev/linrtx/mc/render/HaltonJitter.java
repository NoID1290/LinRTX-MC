package dev.linrtx.mc.render;

import org.joml.Matrix4f;

/**
 * Low-discrepancy Halton (base 2, base 3) subpixel jitter generator for DLSS 4.5.
 *
 * <p>Coordinates follow Vulkan top-left pixel convention in [-0.5, +0.5).
 * Projection jitter left-multiplies Translation(-2*jx/W, +2*jy/H, 0) onto MC's
 * OpenGL clip matrix so the world shifts (-jx, +jy_gl) and sample centers probe
 * (+jx, +jy) in Vulkan top-left space, matching DLSS InJitterOffset = (jx, jy).
 */
public final class HaltonJitter {
    private int frameIndex = 0;
    private float currentX = 0.0f;
    private float currentY = 0.0f;
    private final Matrix4f scratchJitter = new Matrix4f();

    public static float halton(int index, int base) {
        float result = 0.0f;
        float invBase = 1.0f / base;
        float fraction = invBase;
        int i = index;
        while (i > 0) {
            result += (i % base) * fraction;
            i /= base;
            fraction *= invBase;
        }
        return result;
    }

    public static int computeCycleLength(int renderW, int displayW) {
        if (renderW <= 0 || displayW <= 0 || renderW >= displayW) {
            return 8;
        }
        double scale = (double) displayW / (double) renderW;
        int len = (int) Math.ceil(8.0 * scale * scale);
        return Math.max(8, Math.min(64, len));
    }

    public void reset() {
        frameIndex = 0;
        currentX = 0.0f;
        currentY = 0.0f;
    }

    public void advance(int renderW, int displayW) {
        int cycle = computeCycleLength(renderW, displayW);
        int idx = (frameIndex % cycle) + 1;
        frameIndex++;
        currentX = halton(idx, 2) - 0.5f;
        currentY = halton(idx, 3) - 0.5f;
    }

    public float currentX() {
        return currentX;
    }

    public float currentY() {
        return currentY;
    }

    /**
     * Left-multiplies subpixel jitter translation into an OpenGL clip projection matrix.
     */
    public void applyProjectionJitter(Matrix4f projGl, int renderW, int renderH) {
        if (projGl == null || renderW <= 0 || renderH <= 0) {
            return;
        }
        float tx = -2.0f * currentX / (float) renderW;
        float ty = 2.0f * currentY / (float) renderH;
        scratchJitter.translation(tx, ty, 0.0f);
        scratchJitter.mul(projGl, projGl);
    }
}
