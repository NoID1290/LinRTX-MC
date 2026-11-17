#include "gl_dispatch.hpp"

#include <dlfcn.h>
#include <cstring>
#include <string>

namespace linrtx {

namespace {

using ProcFn = void* (*)(const char*);

ProcFn resolveGetProcAddress() {
    for (const char* lib : {"libGL.so.1", "libGLX.so.0"}) {
        if (void* gl = dlopen(lib, RTLD_LAZY | RTLD_NOLOAD)) {
            if (auto p = reinterpret_cast<ProcFn>(dlsym(gl, "glXGetProcAddressARB"))) {
                return p;
            }
            if (auto p = reinterpret_cast<ProcFn>(dlsym(gl, "glXGetProcAddress"))) {
                return p;
            }
        }
    }
    if (void* egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_NOLOAD)) {
        if (auto p = reinterpret_cast<ProcFn>(dlsym(egl, "eglGetProcAddress"))) {
            return p;
        }
    }
    if (void* gld = dlopen("libGLdispatch.so.0", RTLD_LAZY | RTLD_NOLOAD)) {
        if (auto p = reinterpret_cast<ProcFn>(dlsym(gld, "__glDispatchGetProcAddress"))) {
            return p;
        }
    }
    // Fallback to global process symbol lookup
    if (auto p = reinterpret_cast<ProcFn>(dlsym(RTLD_DEFAULT, "glXGetProcAddressARB"))) {
        return p;
    }
    if (auto p = reinterpret_cast<ProcFn>(dlsym(RTLD_DEFAULT, "eglGetProcAddress"))) {
        return p;
    }
    return nullptr;
}

template <typename T>
bool loadGlFn(ProcFn getter, const char* name, T& outFn) {
    void* sym = getter ? getter(name) : nullptr;
    if (!sym) {
        sym = dlsym(RTLD_DEFAULT, name);
    }
    outFn = reinterpret_cast<T>(sym);
    return outFn != nullptr;
}

} // namespace

bool GlDispatch::init() {
    if (initialized) return hasExtMemoryObjectFd && hasExtSemaphoreFd;

    ProcFn getter = resolveGetProcAddress();
    if (!getter) return false;

    bool coreOk = true;
    coreOk &= loadGlFn(getter, "glGetIntegerv", pGetIntegerv);
    coreOk &= loadGlFn(getter, "glGetString", pGetString);
    coreOk &= loadGlFn(getter, "glFlush", pFlush);
    coreOk &= loadGlFn(getter, "glFinish", pFinish);
    coreOk &= loadGlFn(getter, "glGenTextures", pGenTextures);
    coreOk &= loadGlFn(getter, "glDeleteTextures", pDeleteTextures);
    coreOk &= loadGlFn(getter, "glBindTexture", pBindTexture);
    coreOk &= loadGlFn(getter, "glTexParameteri", pTexParameteri);
    if (!coreOk) return false;

    loadGlFn(getter, "glGetUnsignedBytei_vEXT", pGetUnsignedBytei_vEXT);

    bool memOk = true;
    memOk &= loadGlFn(getter, "glCreateMemoryObjectsEXT", pCreateMemoryObjectsEXT);
    memOk &= loadGlFn(getter, "glDeleteMemoryObjectsEXT", pDeleteMemoryObjectsEXT);
    memOk &= loadGlFn(getter, "glImportMemoryFdEXT", pImportMemoryFdEXT);
    memOk &= loadGlFn(getter, "glMemoryObjectParameterivEXT", pMemoryObjectParameterivEXT);
    memOk &= loadGlFn(getter, "glTextureStorageMem2DEXT", pTextureStorageMem2DEXT);
    hasExtMemoryObjectFd = memOk;

    bool semOk = true;
    semOk &= loadGlFn(getter, "glGenSemaphoresEXT", pGenSemaphoresEXT);
    semOk &= loadGlFn(getter, "glDeleteSemaphoresEXT", pDeleteSemaphoresEXT);
    semOk &= loadGlFn(getter, "glImportSemaphoreFdEXT", pImportSemaphoreFdEXT);
    semOk &= loadGlFn(getter, "glWaitSemaphoreEXT", pWaitSemaphoreEXT);
    semOk &= loadGlFn(getter, "glSignalSemaphoreEXT", pSignalSemaphoreEXT);
    hasExtSemaphoreFd = semOk;

    hasCopyImage = loadGlFn(getter, "glCopyImageSubData", pCopyImageSubData);

    initialized = true;
    return hasExtMemoryObjectFd && hasExtSemaphoreFd;
}

bool GlDispatch::queryDeviceUuid(std::array<uint8_t, 16>& outUuid) const {
    if (!hasExtMemoryObjectFd || !pGetIntegerv || !pGetUnsignedBytei_vEXT) return false;
    GLint numUuids = 0;
    pGetIntegerv(0x9596 /* GL_NUM_DEVICE_UUIDS_EXT */, &numUuids);
    if (numUuids <= 0) return false;
    outUuid.fill(0);
    pGetUnsignedBytei_vEXT(kGlDeviceUuidExt, 0, outUuid.data());
    for (uint8_t b : outUuid) {
        if (b != 0) return true;
    }
    return false;
}

const char* GlDispatch::getRendererString() const {
    if (!pGetString) return "Unknown OpenGL";
    const GLubyte* s = pGetString(kGlRenderer);
    return s ? reinterpret_cast<const char*>(s) : "Unknown OpenGL";
}

} // namespace linrtx
