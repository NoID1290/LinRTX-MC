package dev.linrtx.mc.render;

import org.joml.Matrix4f;

/**
 * Low-discrepancy Halton (base 2, base 3) subpixel jitter generator for DLSS 4.5 / DLSS-RR.
 *
 * <p>Coordinates follow Vulkan top-left pixel convention in [-0.5, +0.5).
 * Projection jitter left-multiplies Translation(+2*jx/W, -2*jy/H, 0) onto MC's
 * OpenGL clip matrix so rendered geometry shifts (+jx, -jy_gl) = (+jx, +jy) in
 * Vulkan top-left pixel space, matching NVIDIA NGX InJitterOffset = (jx, jy)
 * for both DLSS-SR and DLSS-RR.
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
            return 16;
        }
        double scale = (double) displayW / (double) renderW;
        int len = (int) Math.ceil(8.0 * scale * scale);
        return Math.max(16, Math.min(72, len));
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
     * OpenGL NDC X translation applied to the projection matrix for a Vulkan top-left
     * pixel jitter {@code jitterX}: shifts geometry by {@code +jitterX} pixels right.
     */
    public static float ndcOffsetX(float jitterX, int renderW) {
        return (renderW > 0) ? (2.0f * jitterX / (float) renderW) : 0.0f;
    }

    /**
     * OpenGL NDC Y translation applied to the projection matrix for a Vulkan top-left
     * pixel jitter {@code jitterY}: shifts geometry by {@code +jitterY} pixels down
     * (negative in GL Y-up NDC, positive in Vulkan top-left pixel coordinates).
     */
    public static float ndcOffsetY(float jitterY, int renderH) {
        return (renderH > 0) ? (-2.0f * jitterY / (float) renderH) : 0.0f;
    }

    /**
     * Left-multiplies subpixel jitter translation into an OpenGL clip projection matrix.
     */
    public void applyProjectionJitter(Matrix4f projGl, int renderW, int renderH) {
        if (projGl == null || renderW <= 0 || renderH <= 0) {
            return;
        }
        float tx = ndcOffsetX(currentX, renderW);
        float ty = ndcOffsetY(currentY, renderH);
        scratchJitter.translation(tx, ty, 0.0f);
        scratchJitter.mul(projGl, projGl);
    }
}
