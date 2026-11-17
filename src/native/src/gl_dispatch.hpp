#pragma once

#include <cstdint>
#include <array>

#ifndef GLAPIENTRY
#define GLAPIENTRY
#endif

namespace linrtx {

using GLuint = uint32_t;
using GLint = int32_t;
using GLsizei = int32_t;
using GLenum = uint32_t;
using GLboolean = uint8_t;
using GLuint64 = uint64_t;
using GLubyte = uint8_t;

inline constexpr GLenum kGlDeviceUuidExt = 0x9597;
inline constexpr GLenum kGlHandleTypeOpaqueFdExt = 0x9586;
inline constexpr GLenum kGlDedicatedMemoryObjectExt = 0x9581;
inline constexpr GLenum kGlTexture2D = 0x0DE1;
inline constexpr GLenum kGlRgba8 = 0x8058;
inline constexpr GLenum kGlRgba16f = 0x881A;
inline constexpr GLenum kGlRgb10A2 = 0x8059;
inline constexpr GLenum kGlR32f = 0x822E;
inline constexpr GLenum kGlRg16f = 0x822F;
inline constexpr GLenum kGlLayoutGeneralExt = 0x958D;
inline constexpr GLenum kGlLayoutColorAttachmentExt = 0x958E;
inline constexpr GLenum kGlLayoutShaderReadOnlyExt = 0x9591;
inline constexpr GLenum kGlLayoutTransferSrcExt = 0x9592;
inline constexpr GLenum kGlLayoutTransferDstExt = 0x9593;
inline constexpr GLenum kGlRenderer = 0x1F01;
inline constexpr GLenum kGlVersion = 0x1F02;

struct GlDispatch {
    bool initialized = false;
    bool hasExtMemoryObjectFd = false;
    bool hasExtSemaphoreFd = false;
    bool hasCopyImage = false;

    void(GLAPIENTRY* pGetIntegerv)(GLenum, GLint*) = nullptr;
    const GLubyte*(GLAPIENTRY* pGetString)(GLenum) = nullptr;
    void(GLAPIENTRY* pGetUnsignedBytei_vEXT)(GLenum, GLuint, GLubyte*) = nullptr;
    void(GLAPIENTRY* pFlush)() = nullptr;
    void(GLAPIENTRY* pFinish)() = nullptr;

    void(GLAPIENTRY* pGenTextures)(GLsizei, GLuint*) = nullptr;
    void(GLAPIENTRY* pDeleteTextures)(GLsizei, const GLuint*) = nullptr;
    void(GLAPIENTRY* pBindTexture)(GLenum, GLuint) = nullptr;
    void(GLAPIENTRY* pTexParameteri)(GLenum, GLenum, GLint) = nullptr;

    void(GLAPIENTRY* pCreateMemoryObjectsEXT)(GLsizei, GLuint*) = nullptr;
    void(GLAPIENTRY* pDeleteMemoryObjectsEXT)(GLsizei, const GLuint*) = nullptr;
    void(GLAPIENTRY* pImportMemoryFdEXT)(GLuint, GLuint64, GLenum, GLint) = nullptr;
    void(GLAPIENTRY* pMemoryObjectParameterivEXT)(GLuint, GLenum, const GLint*) = nullptr;
    void(GLAPIENTRY* pTextureStorageMem2DEXT)(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64) = nullptr;

    void(GLAPIENTRY* pGenSemaphoresEXT)(GLsizei, GLuint*) = nullptr;
    void(GLAPIENTRY* pDeleteSemaphoresEXT)(GLsizei, const GLuint*) = nullptr;
    void(GLAPIENTRY* pImportSemaphoreFdEXT)(GLuint, GLenum, GLint) = nullptr;
    void(GLAPIENTRY* pWaitSemaphoreEXT)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*) = nullptr;
    void(GLAPIENTRY* pSignalSemaphoreEXT)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*) = nullptr;

    void(GLAPIENTRY* pCopyImageSubData)(GLuint, GLenum, GLint, GLint, GLint, GLint,
                                        GLuint, GLenum, GLint, GLint, GLint, GLint,
                                        GLsizei, GLsizei, GLsizei) = nullptr;

    bool init();
    bool queryDeviceUuid(std::array<uint8_t, 16>& outUuid) const;
    const char* getRendererString() const;
};

} // namespace linrtx
