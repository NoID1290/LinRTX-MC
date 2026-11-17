package dev.linrtx.mc.render;

import org.joml.Matrix4d;
import org.joml.Matrix4f;
import org.joml.Matrix4fc;
import org.joml.Vector3d;

/**
 * Tracks unjittered camera transforms using double-precision world deltas so
 * motion vectors and DLSS-G optical flow matrices remain exact even millions of
 * blocks away from the world origin.
 */
public final class CameraStateTracker {
    private static final double TELEPORT_RESET_DIST_SQ = 64.0 * 64.0;

    private final Matrix4d currViewProjGl = new Matrix4d();
    private final Matrix4d prevViewProjGl = new Matrix4d();
    private final Matrix4d currProjGl = new Matrix4d();
    private final Matrix4d currViewRot = new Matrix4d();

    private final Matrix4f mcInvProjGlMat = new Matrix4f();
    private final Matrix4f glClipCurrToPrevClipMat = new Matrix4f();
    private final Matrix4f glSkyCurrToPrevClipMat = new Matrix4f();
    private final Matrix4f glViewToPrevClipMat = new Matrix4f();

    private final Matrix4f viewToClipVkMat = new Matrix4f();
    private final Matrix4f clipToViewVkMat = new Matrix4f();
    private final Matrix4f clipToPrevClipVkMat = new Matrix4f();
    private final Matrix4f prevClipToClipVkMat = new Matrix4f();

    private final float[] viewToClipArr = new float[16];
    private final float[] clipToViewArr = new float[16];
    private final float[] clipToPrevClipArr = new float[16];
    private final float[] prevClipToClipArr = new float[16];
    private final float[] worldToViewArr = new float[16];

    private final float[] cameraPosArr = new float[3];
    private final float[] cameraUpArr = new float[3];
    private final float[] cameraRightArr = new float[3];
    private final float[] cameraFwdArr = new float[3];

    private final Vector3d currPos = new Vector3d();
    private final Vector3d prevPos = new Vector3d();

    private float cameraNear = 0.05f;
    private float cameraFar = 256.0f;
    private float cameraFovRadians = (float) Math.toRadians(70.0);
    private float cameraAspect = 16.0f / 9.0f;

    private Object lastWorldRef = null;
    private boolean hasHistory = false;
    private boolean resetRequested = true;

    // Scratch matrices (allocation-free)
    private final Matrix4d tmpA = new Matrix4d();
    private final Matrix4d tmpB = new Matrix4d();
    private final Matrix4d tmpInvCurrVp = new Matrix4d();
    private final Matrix4d tmpGlCurrToPrev = new Matrix4d();
    private final Matrix4d tmpGlViewToPrev = new Matrix4d();
    private final Matrix4d tmpVkFlip = new Matrix4d().scaling(1.0, -1.0, 1.0);
    private final Matrix4d tmpVkMat = new Matrix4d();

    public void requestReset() {
        hasHistory = false;
        resetRequested = true;
    }

    public boolean isResetPending() {
        return resetRequested;
    }

    public boolean checkAndClearReset() {
        boolean r = resetRequested;
        resetRequested = false;
        return r;
    }

    /**
     * Updates camera matrices for the current frame.
     *
     * @param worldTag           Opaque level object identity (change triggers history reset)
     * @param posX               Double world camera X
     * @param posY               Double world camera Y
     * @param posZ               Double world camera Z
     * @param unjitteredProjGl   OpenGL projection matrix BEFORE Halton jitter
     * @param viewRotation       Camera rotation matrix (world offset -> camera view space)
     * @param nearPlane          Near clip plane distance in blocks
     * @param farPlane           Far clip plane distance in blocks
     * @param fovRadians         Vertical FOV in radians
     * @param aspect             Viewport width / height
     */
    public void update(
        Object worldTag,
        double posX,
        double posY,
        double posZ,
        Matrix4fc unjitteredProjGl,
        Matrix4fc viewRotation,
        float nearPlane,
        float farPlane,
        float fovRadians,
        float aspect
    ) {
        if (worldTag != null && worldTag != lastWorldRef) {
            lastWorldRef = worldTag;
            hasHistory = false;
            resetRequested = true;
        }

        currPos.set(posX, posY, posZ);
        double dx = currPos.x - prevPos.x;
        double dy = currPos.y - prevPos.y;
        double dz = currPos.z - prevPos.z;
        if (!hasHistory || (dx * dx + dy * dy + dz * dz) > TELEPORT_RESET_DIST_SQ) {
            dx = 0.0;
            dy = 0.0;
            dz = 0.0;
            resetRequested = true;
            prevPos.set(currPos);
        }

        float derivedFov = fovRadians;
        float derivedAspect = aspect;
        if (Math.abs(unjitteredProjGl.m11()) > 1e-4f) {
            derivedFov = (float) (2.0 * Math.atan(1.0 / Math.abs(unjitteredProjGl.m11())));
            if (Math.abs(unjitteredProjGl.m00()) > 1e-4f) {
                derivedAspect = Math.abs(unjitteredProjGl.m11() / unjitteredProjGl.m00());
            }
        }
        cameraNear = nearPlane > 0.0f ? nearPlane : 0.05f;
        cameraFar = farPlane > cameraNear ? farPlane : 256.0f;
        cameraFovRadians = derivedFov > 0.05f ? derivedFov : (float) Math.toRadians(70.0);
        cameraAspect = derivedAspect > 0.05f ? derivedAspect : (16.0f / 9.0f);

        currProjGl.set(unjitteredProjGl);
        currViewRot.set(viewRotation);
        currProjGl.mul(currViewRot, currViewProjGl);

        if (!hasHistory) {
            prevViewProjGl.set(currViewProjGl);
        }

        // Camera position relative to previous frame origin, transformed into prev clip space:
        // p_rel_prev = (p_rel_curr) + (currPos - prevPos)
        tmpA.translation(dx, dy, dz);
        currViewRot.invert(tmpB);
        tmpGlViewToPrev.set(prevViewProjGl).mul(tmpA).mul(tmpB);
        glViewToPrevClipMat.set(tmpGlViewToPrev);

        currViewProjGl.invert(tmpInvCurrVp);
        tmpGlCurrToPrev.set(prevViewProjGl).mul(tmpA).mul(tmpInvCurrVp);
        glClipCurrToPrevClipMat.set(tmpGlCurrToPrev);

        // Sky / infinite-distance reprojection excludes camera translation
        tmpA.set(prevViewProjGl).mul(tmpInvCurrVp);
        glSkyCurrToPrevClipMat.set(tmpA);

        currProjGl.invert(tmpA);
        mcInvProjGlMat.set(tmpA);

        // Vulkan top-left matrices (Y-flipped clip space relative to OpenGL clip space)
        tmpVkMat.set(tmpVkFlip).mul(currProjGl);
        viewToClipVkMat.set(tmpVkMat);
        tmpVkMat.invert(tmpA);
        clipToViewVkMat.set(tmpA);

        // clipToPrevClip_vk = S_vk * glClipCurrToPrevClip * S_vk
        tmpVkMat.set(tmpVkFlip).mul(tmpGlCurrToPrev).mul(tmpVkFlip);
        clipToPrevClipVkMat.set(tmpVkMat);
        tmpVkMat.invert(tmpA);
        prevClipToClipVkMat.set(tmpA);

        viewToClipVkMat.get(viewToClipArr);
        clipToViewVkMat.get(clipToViewArr);
        clipToPrevClipVkMat.get(clipToPrevClipArr);
        prevClipToClipVkMat.get(prevClipToClipArr);

        // Extract camera basis in world space from viewRotation (inverse = transpose for pure rotation)
        // Row 0 of viewRotation = right, Row 1 = up, -Row 2 = forward
        cameraRightArr[0] = (float) currViewRot.m00();
        cameraRightArr[1] = (float) currViewRot.m10();
        cameraRightArr[2] = (float) currViewRot.m20();
        cameraUpArr[0] = (float) currViewRot.m01();
        cameraUpArr[1] = (float) currViewRot.m11();
        cameraUpArr[2] = (float) currViewRot.m21();
        cameraFwdArr[0] = (float) -currViewRot.m02();
        cameraFwdArr[1] = (float) -currViewRot.m12();
        cameraFwdArr[2] = (float) -currViewRot.m22();

        cameraPosArr[0] = (float) posX;
        cameraPosArr[1] = (float) posY;
        cameraPosArr[2] = (float) posZ;

        // Row-major world->view rotation (3x3 viewRotation + identity W)
        worldToViewArr[0] = cameraRightArr[0];
        worldToViewArr[1] = cameraRightArr[1];
        worldToViewArr[2] = cameraRightArr[2];
        worldToViewArr[3] = 0.0f;
        worldToViewArr[4] = cameraUpArr[0];
        worldToViewArr[5] = cameraUpArr[1];
        worldToViewArr[6] = cameraUpArr[2];
        worldToViewArr[7] = 0.0f;
        worldToViewArr[8] = -cameraFwdArr[0];
        worldToViewArr[9] = -cameraFwdArr[1];
        worldToViewArr[10] = -cameraFwdArr[2];
        worldToViewArr[11] = 0.0f;
        worldToViewArr[12] = 0.0f;
        worldToViewArr[13] = 0.0f;
        worldToViewArr[14] = 0.0f;
        worldToViewArr[15] = 1.0f;

        prevViewProjGl.set(currViewProjGl);
        prevPos.set(currPos);
        hasHistory = true;
    }

    public Matrix4fc mcInvProjGl() { return mcInvProjGlMat; }
    public Matrix4fc glClipCurrToPrevClip() { return glClipCurrToPrevClipMat; }
    public Matrix4fc glSkyCurrToPrevClip() { return glSkyCurrToPrevClipMat; }
    public Matrix4fc glViewToPrevClip() { return glViewToPrevClipMat; }

    public float[] viewToClipVk() { return viewToClipArr; }
    public float[] clipToViewVk() { return clipToViewArr; }
    public float[] clipToPrevClipVk() { return clipToPrevClipArr; }
    public float[] prevClipToClipVk() { return prevClipToClipArr; }
    public float[] worldToViewRowMajor() { return worldToViewArr; }

    public float[] cameraPos() { return cameraPosArr; }
    public float[] cameraUp() { return cameraUpArr; }
    public float[] cameraRight() { return cameraRightArr; }
    public float[] cameraFwd() { return cameraFwdArr; }

    public float cameraNear() { return cameraNear; }
    public float cameraFar() { return cameraFar; }
    public float cameraFovRadians() { return cameraFovRadians; }
    public float cameraAspect() { return cameraAspect; }
}
