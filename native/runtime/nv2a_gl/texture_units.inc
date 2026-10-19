// GPU-thread texture bindings. The download worker has a separate GL
// context, so its temporary bindings must not invalidate the GPU's records.
struct Nv2aUnitBinding { bool known = false, empty = false, rectangle_empty = false; GLenum target = 0; GLuint texture = 0; };
static thread_local Nv2aUnitBinding g_texture_units[4];
static thread_local int g_texture_active_unit = -1;
static unsigned long long g_texture_unit_hits = 0, g_texture_gl_binds = 0;
static void Nv2aInvalidateTextureUnits() {
    for (auto& u : g_texture_units) u.known = false;
    g_texture_active_unit = -1;
}
static void Nv2aForeignBindTexture(GLenum target, GLuint texture) {
    Nv2aInvalidateTextureUnits();
    glBindTexture(target, texture);
}
static void Nv2aForeignDeleteTextures(GLsizei count, const GLuint* textures) {
    Nv2aInvalidateTextureUnits();
    glDeleteTextures(count, textures);
}
static void Nv2aForeignActiveTexture(GLenum texture) {
    g_texture_active_unit = -1;
    glActiveTexture(texture);
}
static void Nv2aActivateTextureUnit(unsigned unit) {
    if (g_texture_active_unit != int(unit)) {
        glActiveTexture(GL_TEXTURE0 + unit);
        g_texture_active_unit = int(unit);
    }
}
static void Nv2aBindTextureUnit(unsigned unit, GLenum target, GLuint texture) {
    auto& u = g_texture_units[unit];
    if (u.known && !u.empty && u.target == target && u.texture == texture) return;
    Nv2aActivateTextureUnit(unit);
    glBindTexture(target, texture); ++g_texture_gl_binds;
    u = {true, false, false, target, texture};
}
static void Nv2aEmptyTextureUnit(unsigned unit, bool rectangle) {
    auto& u = g_texture_units[unit];
    if (u.known && u.empty && (!rectangle || u.rectangle_empty)) return;
    Nv2aActivateTextureUnit(unit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0); glBindTexture(GL_TEXTURE_1D, 0);
    glBindTexture(GL_TEXTURE_2D, 0); glBindTexture(GL_TEXTURE_3D, 0);
    g_texture_gl_binds += 4;
    if (rectangle) { glBindTexture(GL_TEXTURE_RECTANGLE, 0); ++g_texture_gl_binds; }
    u = {true, true, rectangle, 0, 0};
}
