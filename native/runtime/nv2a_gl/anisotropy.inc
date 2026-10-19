// Copyright (C) 2026 skeezixmccat-coder. SPDX-License-Identifier: GPL-2.0-only
// Optional host filtering enhancement; no guest registers or mip levels are changed.
static unsigned g_anisotropy = 1;
static bool g_anisotropy_supported = false;
static void Nv2aAnisotropyInit() {
    char value[16] = {}; DWORD n = GetEnvironmentVariableA("GH_ANISOTROPY",value,sizeof value);
    unsigned requested = 1;
    if(n) {
        if(n >= sizeof value || (strcmp(value,"1") && strcmp(value,"2") && strcmp(value,"4") && strcmp(value,"8") && strcmp(value,"16")))
            Nv2aGlUnknown("GH_ANISOTROPY must be 1, 2, 4, 8 or 16");
        requested = unsigned(atoi(value));
    }
    GLint major=0,minor=0; glGetIntegerv(GL_MAJOR_VERSION,&major);glGetIntegerv(GL_MINOR_VERSION,&minor);
    g_anisotropy_supported = major > 4 || (major == 4 && minor >= 6) ||
        glo_check_extension("GL_EXT_texture_filter_anisotropic") || glo_check_extension("GL_ARB_texture_filter_anisotropic");
    GLfloat maximum=1;
    if(g_anisotropy_supported) glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT,&maximum);
    g_anisotropy=std::max(1u,std::min(requested,unsigned(maximum)));
    Nv2aGlLog("anisotropic filtering: requested %ux, effective %ux (eligible mipmapped 2D textures only)",requested,g_anisotropy);
}
static unsigned Nv2aTextureAnisotropy(GLenum target, bool eligible, GLenum min_filter, GLenum mag_filter) {
    return target==GL_TEXTURE_2D && eligible && mag_filter==GL_LINEAR &&
        (min_filter==GL_LINEAR_MIPMAP_LINEAR || min_filter==GL_LINEAR_MIPMAP_NEAREST) ? g_anisotropy : 1;
}
static void Nv2aApplyAnisotropy(GLenum target,unsigned value) {
    if(g_anisotropy_supported) glTexParameterf(target,GL_TEXTURE_MAX_ANISOTROPY_EXT,GLfloat(value));
}
