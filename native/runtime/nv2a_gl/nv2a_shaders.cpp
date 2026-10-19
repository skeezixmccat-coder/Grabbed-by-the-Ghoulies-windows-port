// Licence selection for this modified copy (2026-10-19): under LGPL section 3,
// references to the LGPL in the retained upstream notice below are replaced
// by the GNU General Public License, version 2, for the adapted LGPL portion.
// The upstream notice is reproduced verbatim as provenance of the original.
// Additional expressions adapted from xemu-vsh-ff.c at xemu 36334d95b86f9c3c6ae34efb9af06785273f8e1a.
// The adapted LGPL-2.0-or-later portion is distributed under GPL v2 via LGPL section 3.
/*
 * Geforce NV2A PGRAPH GLSL Shader Generator
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2020-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
// Modified for the Ghoulies native Windows preview, 2026-10-14 through 2026-10-19.
// Port integration, validation and rendering changes: see docs/PORT-PROVENANCE.md.
#include "compat.h"
/*
 * QEMU Geforce NV2A shader generator
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2015 Jannik Vogel
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

//#include "qemu-common.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "nv2a_debug.h"
#include "nv2a_shaders_common.h"
#include "nv2a_shaders.h"
extern "C" unsigned Nv2aGlRenderScale();

#ifndef glue
#define xglue(x, y) x ## y
#define glue(x, y) xglue(x, y)
#define stringify(s)	tostring(s)
#define tostring(s)	#s
#endif

static QString* generate_geometry_shader(
                                      enum ShaderPolygonMode polygon_front_mode,
                                      enum ShaderPolygonMode polygon_back_mode,
                                      enum ShaderPrimitiveMode primitive_mode,
                                      GLenum *gl_primitive_mode,
                                      bool wide_lines)
{
    /* Native port: lines wider than the host line-width range are expanded
     * to screen-space quads with the OpenGL aliased wide-line rule: x-major
     * lines widen vertically, y-major lines horizontally, by lineWidth pixels
     * in surface (clip) coordinates. */
    if (wide_lines && (primitive_mode == PRIM_TYPE_LINES || primitive_mode == PRIM_TYPE_LINE_LOOP ||
                       primitive_mode == PRIM_TYPE_LINE_STRIP)) {
        *gl_primitive_mode = primitive_mode == PRIM_TYPE_LINES ? GL_LINES :
                             primitive_mode == PRIM_TYPE_LINE_LOOP ? GL_LINE_LOOP : GL_LINE_STRIP;
        QString* w = qstring_from_str("#version 330\n"
            "layout(lines) in;\n"
            "layout(triangle_strip, max_vertices = 4) out;\n"
            STRUCT_VERTEX_DATA
            "noperspective in VertexData v_vtx[];\n"
            "noperspective out VertexData g_vtx;\n"
            "uniform vec2 surfaceSize;\n"
            "uniform float lineWidth;\n"
            "void emit_offset(int index, vec2 offset_px) {\n"
            "  vec4 p = gl_in[index].gl_Position;\n"
            "  p.xy += offset_px * 2.0 / surfaceSize * p.w;\n"
            "  gl_Position = p;\n"
            "  g_vtx = v_vtx[index];\n"
            "  EmitVertex();\n"
            "}\n"
            "void main() {\n"
            "  vec2 a = gl_in[0].gl_Position.xy / gl_in[0].gl_Position.w * surfaceSize;\n"
            "  vec2 b = gl_in[1].gl_Position.xy / gl_in[1].gl_Position.w * surfaceSize;\n"
            "  vec2 d = abs(b - a);\n"
            "  vec2 offset = (d.x >= d.y) ? vec2(0.0, 0.5 * lineWidth) : vec2(0.5 * lineWidth, 0.0);\n"
            "  emit_offset(0, -offset);\n"
            "  emit_offset(0, offset);\n"
            "  emit_offset(1, -offset);\n"
            "  emit_offset(1, offset);\n"
            "  EndPrimitive();\n"
            "}\n");
        return w;
    }

    /* FIXME: Missing support for 2-sided-poly mode */
    assert(polygon_front_mode == polygon_back_mode);
    enum ShaderPolygonMode polygon_mode = polygon_front_mode;

    /* POINT mode shouldn't require any special work */
    if (polygon_mode == POLY_MODE_POINT) {
        *gl_primitive_mode = GL_POINTS;
        return NULL;
    }

    /* Handle LINE and FILL mode */
    const char *layout_in = NULL;
    const char *layout_out = NULL;
    const char *body = NULL;
    switch (primitive_mode) {
    case PRIM_TYPE_POINTS: *gl_primitive_mode = GL_POINTS; return NULL;
    case PRIM_TYPE_LINES: *gl_primitive_mode = GL_LINES; return NULL;
    case PRIM_TYPE_LINE_LOOP: *gl_primitive_mode = GL_LINE_LOOP; return NULL;
    case PRIM_TYPE_LINE_STRIP: *gl_primitive_mode = GL_LINE_STRIP; return NULL;
    case PRIM_TYPE_TRIANGLES:
        *gl_primitive_mode = GL_TRIANGLES;
        if (polygon_mode == POLY_MODE_FILL) { return NULL; }
        assert(polygon_mode == POLY_MODE_LINE);
        layout_in = "layout(triangles) in;\n";
        layout_out = "layout(line_strip, max_vertices = 4) out;\n";
        body = "  emit_vertex(0);\n"
               "  emit_vertex(1);\n"
               "  emit_vertex(2);\n"
               "  emit_vertex(0);\n"
               "  EndPrimitive();\n";
        break;
    case PRIM_TYPE_TRIANGLE_STRIP:
        *gl_primitive_mode = GL_TRIANGLE_STRIP;
        if (polygon_mode == POLY_MODE_FILL) { return NULL; }
        assert(polygon_mode == POLY_MODE_LINE);
        layout_in = "layout(triangles) in;\n";
        layout_out = "layout(line_strip, max_vertices = 4) out;\n";
        /* Imagine a quad made of a tristrip, the comments tell you which
         * vertex we are using */
        body = "  if ((gl_PrimitiveIDIn & 1) == 0) {\n"
               "    if (gl_PrimitiveIDIn == 0) {\n"
               "      emit_vertex(0);\n" /* bottom right */
               "    }\n"
               "    emit_vertex(1);\n" /* top right */
               "    emit_vertex(2);\n" /* bottom left */
               "    emit_vertex(0);\n" /* bottom right */
               "  } else {\n"
               "    emit_vertex(2);\n" /* bottom left */
               "    emit_vertex(1);\n" /* top left */
               "    emit_vertex(0);\n" /* top right */
               "  }\n"
               "  EndPrimitive();\n";
        break;
    case PRIM_TYPE_TRIANGLE_FAN:
        *gl_primitive_mode = GL_TRIANGLE_FAN;
        if (polygon_mode == POLY_MODE_FILL) { return NULL; }
        assert(polygon_mode == POLY_MODE_LINE);
        layout_in = "layout(triangles) in;\n";
        layout_out = "layout(line_strip, max_vertices = 4) out;\n";
        body = "  if (gl_PrimitiveIDIn == 0) {\n"
               "    emit_vertex(0);\n"
               "  }\n"
               "  emit_vertex(1);\n"
               "  emit_vertex(2);\n"
               "  emit_vertex(0);\n"
               "  EndPrimitive();\n";
        break;
    case PRIM_TYPE_QUADS:
        *gl_primitive_mode = GL_LINES_ADJACENCY;
        layout_in = "layout(lines_adjacency) in;\n";
        if (polygon_mode == POLY_MODE_LINE) {
            layout_out = "layout(line_strip, max_vertices = 5) out;\n";
            body = "  emit_vertex(0);\n"
                   "  emit_vertex(1);\n"
                   "  emit_vertex(2);\n"
                   "  emit_vertex(3);\n"
                   "  emit_vertex(0);\n"
                   "  EndPrimitive();\n";
        } else if (polygon_mode == POLY_MODE_FILL) {
            layout_out = "layout(triangle_strip, max_vertices = 4) out;\n";
            body = "  emit_vertex(0);\n"
                   "  emit_vertex(1);\n"
                   "  emit_vertex(3);\n"
                   "  emit_vertex(2);\n"
                   "  EndPrimitive();\n";
        } else {
            assert(false);
            return NULL;
        }
        break;
    case PRIM_TYPE_QUAD_STRIP:
        *gl_primitive_mode = GL_LINE_STRIP_ADJACENCY;
        layout_in = "layout(lines_adjacency) in;\n";
        if (polygon_mode == POLY_MODE_LINE) {
            layout_out = "layout(line_strip, max_vertices = 5) out;\n";
            body = "  if ((gl_PrimitiveIDIn & 1) != 0) { return; }\n"
                   "  if (gl_PrimitiveIDIn == 0) {\n"
                   "    emit_vertex(0);\n"
                   "  }\n"
                   "  emit_vertex(1);\n"
                   "  emit_vertex(3);\n"
                   "  emit_vertex(2);\n"
                   "  emit_vertex(0);\n"
                   "  EndPrimitive();\n";
        } else if (polygon_mode == POLY_MODE_FILL) {
            layout_out = "layout(triangle_strip, max_vertices = 4) out;\n";
            body = "  if ((gl_PrimitiveIDIn & 1) != 0) { return; }\n"
                   "  emit_vertex(0);\n"
                   "  emit_vertex(1);\n"
                   "  emit_vertex(2);\n"
                   "  emit_vertex(3);\n"
                   "  EndPrimitive();\n";
        } else {
            assert(false);
            return NULL;
        }
        break;
    case PRIM_TYPE_POLYGON:
        if (polygon_mode == POLY_MODE_LINE) {
            *gl_primitive_mode = GL_LINE_LOOP;
        } else if (polygon_mode == POLY_MODE_FILL) {
            *gl_primitive_mode = GL_TRIANGLE_FAN;
        } else {
            assert(false);
        }
        return NULL;
    default:
        assert(false);
        return NULL;
    }

    /* generate a geometry shader to support deprecated primitive types */
    assert(layout_in);
    assert(layout_out);
    assert(body);
    QString* s = qstring_from_str("#version 330\n"
                                  "\n");
    qstring_append(s, layout_in);
    qstring_append(s, layout_out);
    qstring_append(s, "\n"
                      STRUCT_VERTEX_DATA
                      "noperspective in VertexData v_vtx[];\n"
                      "noperspective out VertexData g_vtx;\n"
                      "\n"
                      "void emit_vertex(int index) {\n"
                      "  gl_Position = gl_in[index].gl_Position;\n"
                      "  gl_PointSize = gl_in[index].gl_PointSize;\n"
                      "  g_vtx = v_vtx[index];\n"
                      "  EmitVertex();\n"
                      "}\n"
                      "\n"
                      "void main() {\n");
    qstring_append(s, body);
    qstring_append(s, "}\n");

    return s;
}

static void append_skinning_code(QString* str, bool mix,
                                 unsigned int count, const char* type,
                                 const char* output, const char* input,
                                 const char* matrix, const char* swizzle)
{

    if (count == 0) {
        qstring_append_fmt(str, "%s %s = (%s * %s0).%s;\n",
                           type, output, input, matrix, swizzle);
    } else {
        qstring_append_fmt(str, "%s %s = %s(0.0);\n", type, output, type);
        if (mix) {
            /* Tweening */
            if (count == 2) {
                qstring_append_fmt(str,
                                   "%s += mix((%s * %s1).%s,\n"
                                   "          (%s * %s0).%s, weight.x);\n",
                                   output,
                                   input, matrix, swizzle,
                                   input, matrix, swizzle);
            } else {
                /* FIXME: Not sure how blend weights are calculated */
                assert(false);
            }
        } else {
            /* Individual matrices */
            unsigned int i;
            for (i = 0; i < count; i++) {
                char c = "xyzw"[i];
                qstring_append_fmt(str, "%s += (%s * %s%d * weight.%c).%s;\n",
                                   output, input, matrix, i, c,
                                   swizzle);
            }
            assert(false); /* FIXME: Untested */
        }
    }
}

#define GLSL_C(idx) "c[" stringify(idx) "]"
#define GLSL_LTCTXA(idx) "ltctxa[" stringify(idx) "]"

#define GLSL_C_MAT4(idx) \
    "mat4(" GLSL_C(idx) ", " GLSL_C(idx+1) ", " \
            GLSL_C(idx+2) ", " GLSL_C(idx+3) ")"

#define GLSL_DEFINE(a, b) "#define " stringify(a) " " b "\n"

static void generate_fixed_function(const ShaderState state,
                                    QString *header, QString *body)
{
    int i, j;

    /* generate vertex shader mimicking fixed function */
    qstring_append(header,
"#define position      v0\n"
"#define weight        v1\n"
"#define normal        v2.xyz\n"
"#define diffuse       v3\n"
"#define specular      v4\n"
"#define fogCoord      v5.x\n"
"#define pointSize     v6\n"
"#define backDiffuse   v7\n"
"#define backSpecular  v8\n"
"#define texture0      v9\n"
"#define texture1      v10\n"
"#define texture2      v11\n"
"#define texture3      v12\n"
"#define reserved1     v13\n"
"#define reserved2     v14\n"
"#define reserved3     v15\n"
"\n"
"uniform vec4 ltctxa[" stringify(NV2A_LTCTXA_COUNT) "];\n"
"uniform vec4 ltctxb[" stringify(NV2A_LTCTXB_COUNT) "];\n"
"uniform vec4 ltc1[" stringify(NV2A_LTC1_COUNT) "];\n"
"\n"
GLSL_DEFINE(projectionMat, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_PMAT0))
GLSL_DEFINE(compositeMat, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_CMAT0))
"\n"
GLSL_DEFINE(texPlaneS0, GLSL_C(NV_IGRAPH_XF_XFCTX_TG0MAT + 0))
GLSL_DEFINE(texPlaneT0, GLSL_C(NV_IGRAPH_XF_XFCTX_TG0MAT + 1))
GLSL_DEFINE(texPlaneQ0, GLSL_C(NV_IGRAPH_XF_XFCTX_TG0MAT + 2))
GLSL_DEFINE(texPlaneR0, GLSL_C(NV_IGRAPH_XF_XFCTX_TG0MAT + 3))
"\n"
GLSL_DEFINE(texPlaneS1, GLSL_C(NV_IGRAPH_XF_XFCTX_TG1MAT + 0))
GLSL_DEFINE(texPlaneT1, GLSL_C(NV_IGRAPH_XF_XFCTX_TG1MAT + 1))
GLSL_DEFINE(texPlaneQ1, GLSL_C(NV_IGRAPH_XF_XFCTX_TG1MAT + 2))
GLSL_DEFINE(texPlaneR1, GLSL_C(NV_IGRAPH_XF_XFCTX_TG1MAT + 3))
"\n"
GLSL_DEFINE(texPlaneS2, GLSL_C(NV_IGRAPH_XF_XFCTX_TG2MAT + 0))
GLSL_DEFINE(texPlaneT2, GLSL_C(NV_IGRAPH_XF_XFCTX_TG2MAT + 1))
GLSL_DEFINE(texPlaneQ2, GLSL_C(NV_IGRAPH_XF_XFCTX_TG2MAT + 2))
GLSL_DEFINE(texPlaneR2, GLSL_C(NV_IGRAPH_XF_XFCTX_TG2MAT + 3))
"\n"
GLSL_DEFINE(texPlaneS3, GLSL_C(NV_IGRAPH_XF_XFCTX_TG3MAT + 0))
GLSL_DEFINE(texPlaneT3, GLSL_C(NV_IGRAPH_XF_XFCTX_TG3MAT + 1))
GLSL_DEFINE(texPlaneQ3, GLSL_C(NV_IGRAPH_XF_XFCTX_TG3MAT + 2))
GLSL_DEFINE(texPlaneR3, GLSL_C(NV_IGRAPH_XF_XFCTX_TG3MAT + 3))
"\n"
GLSL_DEFINE(modelViewMat0, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_MMAT0))
GLSL_DEFINE(modelViewMat1, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_MMAT1))
GLSL_DEFINE(modelViewMat2, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_MMAT2))
GLSL_DEFINE(modelViewMat3, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_MMAT3))
"\n"
GLSL_DEFINE(invModelViewMat0, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_IMMAT0))
GLSL_DEFINE(invModelViewMat1, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_IMMAT1))
GLSL_DEFINE(invModelViewMat2, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_IMMAT2))
GLSL_DEFINE(invModelViewMat3, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_IMMAT3))
"\n"
GLSL_DEFINE(eyePosition, GLSL_C(NV_IGRAPH_XF_XFCTX_EYEP))
"\n"
"#define lightAmbientColor(i) "
    "ltctxb[" stringify(NV_IGRAPH_XF_LTCTXB_L0_AMB) " + (i)*6].xyz\n"
"#define lightDiffuseColor(i) "
    "ltctxb[" stringify(NV_IGRAPH_XF_LTCTXB_L0_DIF) " + (i)*6].xyz\n"
"#define lightSpecularColor(i) "
    "ltctxb[" stringify(NV_IGRAPH_XF_LTCTXB_L0_SPC) " + (i)*6].xyz\n"
"\n"
"#define lightSpotFalloff(i) "
    "ltctxa[" stringify(NV_IGRAPH_XF_LTCTXA_L0_K) " + (i)*2].xyz\n"
"#define lightSpotDirection(i) "
    "ltctxa[" stringify(NV_IGRAPH_XF_LTCTXA_L0_SPT) " + (i)*2]\n"
"\n"
"#define lightLocalRange(i) "
    "ltc1[" stringify(NV_IGRAPH_XF_LTC1_r0) " + (i)].x\n"
"\n"
GLSL_DEFINE(sceneAmbientColor, GLSL_LTCTXA(NV_IGRAPH_XF_LTCTXA_FR_AMB) ".xyz")
"\n"
"uniform mat4 invViewport;\n"
"\n");

    /* Skinning */
    unsigned int count;
    bool mix;
    switch (state.skinning) {
    case SKINNING_OFF:
        mix = false; count = 0; break;
    case SKINNING_1WEIGHTS:
        mix = true; count = 2; break;
    case SKINNING_2WEIGHTS:
        mix = true; count = 3; break;
    case SKINNING_3WEIGHTS:
        mix = true; count = 4; break;
    case SKINNING_2WEIGHTS2MATRICES:
        mix = false; count = 2; break;
    case SKINNING_3WEIGHTS3MATRICES:
        mix = false; count = 3; break;
    case SKINNING_4WEIGHTS4MATRICES:
        mix = false; count = 4; break;
    default:
        assert(false);
        break;
    }
    qstring_append_fmt(body, "/* Skinning mode %d */\n",
                       state.skinning);

    append_skinning_code(body, mix, count, "vec4",
                         "tPosition", "position",
                         "modelViewMat", "xyzw");
    append_skinning_code(body, mix, count, "vec3",
                         "tNormal", "vec4(normal, 0.0)",
                         "invModelViewMat", "xyz");

    /* Normalization */
    if (state.normalization) {
        qstring_append(body, "tNormal = normalize(tNormal);\n");
    }

    /* Texgen */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        qstring_append_fmt(body, "/* Texgen for stage %d */\n",
                           i);
        /* Set each component individually */
        /* FIXME: could be nicer if some channels share the same texgen */
        for (j = 0; j < 4; j++) {
            /* TODO: TexGen View Model missing! */
            char c = "xyzw"[j];
            char cSuffix = "STRQ"[j];
            switch (state.texgen[i][j]) {
            case TEXGEN_DISABLE:
                qstring_append_fmt(body, "oT%d.%c = texture%d.%c;\n",
                                   i, c, i, c);
                break;
            case TEXGEN_EYE_LINEAR:
                qstring_append_fmt(body, "oT%d.%c = dot(texPlane%c%d, tPosition);\n",
                                   i, c, cSuffix, i);
                break;
            case TEXGEN_OBJECT_LINEAR:
                qstring_append_fmt(body, "oT%d.%c = dot(texPlane%c%d, position);\n",
                                   i, c, cSuffix, i);
                assert(false); /* Untested */
                break;
            case TEXGEN_SPHERE_MAP:
                assert(i < 2);  /* Channels S,T only! */
                qstring_append(body, "{\n");
                /* FIXME: u, r and m only have to be calculated once */
                qstring_append(body, "  vec3 u = normalize(tPosition.xyz);\n");
                //FIXME: tNormal before or after normalization? Always normalize?
                qstring_append(body, "  vec3 r = reflect(u, tNormal);\n");

                /* FIXME: This would consume 1 division fewer and *might* be
                 *        faster than length:
                 *   // [z=1/(2*x) => z=1/x*0.5]
                 *   vec3 ro = r + vec3(0.0, 0.0, 1.0);
                 *   float m = inversesqrt(dot(ro,ro))*0.5;
                 */

                qstring_append(body, "  float invM = 1.0 / (2.0 * length(r + vec3(0.0, 0.0, 1.0)));\n");
                qstring_append_fmt(body, "  oT%d.%c = r.%c * invM + 0.5;\n",
                                   i, c, c);
                qstring_append(body, "}\n");
                assert(false); /* Untested */
                break;
            case TEXGEN_REFLECTION_MAP:
                assert(i < 3); /* Channels S,T,R only! */
                qstring_append(body, "{\n");
                /* FIXME: u and r only have to be calculated once, can share the one from SPHERE_MAP */
                qstring_append(body, "  vec3 u = normalize(tPosition.xyz);\n");
                qstring_append(body, "  vec3 r = reflect(u, tNormal);\n");
                qstring_append_fmt(body, "  oT%d.%c = r.%c;\n",
                                   i, c, c);
                qstring_append(body, "}\n");
                break;
            case TEXGEN_NORMAL_MAP:
                assert(i < 3); /* Channels S,T,R only! */
                qstring_append_fmt(body, "oT%d.%c = tNormal.%c;\n",
                                   i, c, c);
                break;
            default:
                assert(false);
                break;
            }
        }
    }

    /* Apply texture matrices */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (state.texture_matrix_enable[i]) {
            qstring_append_fmt(body,
                               "oT%d = oT%d * texMat%d;\n",
                               i, i, i);
        }
    }

    /* Lighting */
    if (state.lighting) {

        //FIXME: Do 2 passes if we want 2 sided-lighting?
        qstring_append(body, "oD0 = vec4(sceneAmbientColor, diffuse.a);\n");
        qstring_append(body, "oD1 = vec4(0.0, 0.0, 0.0, specular.a);\n");

        for (i = 0; i < NV2A_MAX_LIGHTS; i++) {
            if (state.light[i] == LIGHT_OFF) {
                continue;
            }

            /* FIXME: It seems that we only have to handle the surface colors if
             *        they are not part of the material [= vertex colors].
             *        If they are material the cpu will premultiply light
             *        colors
             */

            qstring_append_fmt(body, "/* Light %d */ {\n", i);

            if (state.light[i] == LIGHT_LOCAL
                    || state.light[i] == LIGHT_SPOT) {

                qstring_append_fmt(header,
                    "uniform vec3 lightLocalPosition%d;\n"
                    "uniform vec3 lightLocalAttenuation%d;\n",
                    i, i);
                qstring_append_fmt(body,
                    "  vec3 VP = lightLocalPosition%d - tPosition.xyz/tPosition.w;\n"
                    "  float d = length(VP);\n"
//FIXME: if (d > lightLocalRange) { .. don't process this light .. } /* inclusive?! */ - what about directional lights?
                    "  VP = normalize(VP);\n"
                    "  float attenuation = 1.0 / (lightLocalAttenuation%d.x\n"
                    "                               + lightLocalAttenuation%d.y * d\n"
                    "                               + lightLocalAttenuation%d.z * d * d);\n"
                    "  vec3 halfVector = normalize(VP + eyePosition.xyz / eyePosition.w);\n" /* FIXME: Not sure if eyePosition is correct */
                    "  float nDotVP = max(0.0, dot(tNormal, VP));\n"
                    "  float nDotHV = max(0.0, dot(tNormal, halfVector));\n",
                    i, i, i, i);

            }

            switch(state.light[i]) {
            case LIGHT_INFINITE:

                /* lightLocalRange will be 1e+30 here */

                qstring_append_fmt(header,
                    "uniform vec3 lightInfiniteHalfVector%d;\n"
                    "uniform vec3 lightInfiniteDirection%d;\n",
                    i, i);
                qstring_append_fmt(body,
                    "  float attenuation = 1.0;\n"
                    "  float nDotVP = max(0.0, dot(tNormal, normalize(vec3(lightInfiniteDirection%d))));\n"
                    "  float nDotHV = max(0.0, dot(tNormal, vec3(lightInfiniteHalfVector%d)));\n",
                    i, i);

                /* FIXME: Do specular */

                /* FIXME: tBackDiffuse */

                break;
            case LIGHT_LOCAL:
                /* Everything done already */
                break;
            case LIGHT_SPOT:
                assert(false);
                /*FIXME: calculate falloff */
                break;
            default:
                assert(false);
                break;
            }

            qstring_append_fmt(body,
                "  float pf;\n"
                "  if (nDotVP == 0.0) {\n"
                "    pf = 0.0;\n"
                "  } else {\n"
                "    pf = pow(nDotHV, /* specular(l, m, n, l1, m1, n1) */ 0.001);\n"
                "  }\n"
                "  vec3 lightAmbient = lightAmbientColor(%d) * attenuation;\n"
                "  vec3 lightDiffuse = lightDiffuseColor(%d) * attenuation * nDotVP;\n"
                "  vec3 lightSpecular = lightSpecularColor(%d) * pf;\n",
                i, i, i);

            qstring_append(body,
                "  oD0.xyz += lightAmbient;\n");

            qstring_append(body,
                "  oD0.xyz += diffuse.xyz * lightDiffuse;\n");

            qstring_append(body,
                "  oD1.xyz += specular.xyz * lightSpecular;\n");

            qstring_append(body, "}\n");
        }
    } else {
        qstring_append(body, "  oD0 = diffuse;\n");
        qstring_append(body, "  oD1 = specular;\n");
    }
    qstring_append(body, "  oB0 = backDiffuse;\n");
    qstring_append(body, "  oB1 = backSpecular;\n");

    /* Fog */
    if (state.fog_enable) {

        /* From: https://www.opengl.org/registry/specs/NV/fog_distance.txt */
        switch(state.foggen) {
        case FOGGEN_SPEC_ALPHA:
            /* FIXME: Do we have to clamp here? */
            qstring_append(body, "  float fogDistance = clamp(specular.a, 0.0, 1.0);\n");
            break;
        case FOGGEN_RADIAL:
            qstring_append(body, "  float fogDistance = length(tPosition.xyz);\n");
            break;
        case FOGGEN_PLANAR:
        case FOGGEN_ABS_PLANAR:
            qstring_append(body, "  float fogDistance = dot(fogPlane.xyz, tPosition.xyz) + fogPlane.w;\n");
            if (state.foggen == FOGGEN_ABS_PLANAR) {
                qstring_append(body, "  fogDistance = abs(fogDistance);\n");
            }
            break;
        case FOGGEN_FOG_X:
            qstring_append(body, "  float fogDistance = fogCoord;\n");
            break;
        default:
            assert(false);
            break;
        }

    }

    /* If skinning is off the composite matrix already includes the MV matrix */
    if (state.skinning == SKINNING_OFF) {
        qstring_append(body, "  tPosition = position;\n");
    }

    qstring_append(body,
    // NV2A fixed-function composite coordinates precede viewport translation.
    // Apply VPOFF after perspective division, expressed here in homogeneous
    // coordinates before the host inverse viewport. See xemu 36334d95
    // glsl/vsh-ff.c; programmable shaders apply their own viewport operations.
    "   oPos = tPosition * compositeMat;\n"
    "   oPos.xy += c[" stringify(NV_IGRAPH_XF_XFCTX_VPOFF) "].xy * oPos.w;\n"
    "   oPos = invViewport * oPos;\n"
    "   oPos.z = oPos.z * 2.0 - oPos.w;\n");

    qstring_append(body, "  vtx.inv_w = 1.0 / oPos.w;\n");

}

static QString *generate_vertex_shader(const ShaderState state,
                                       char vtx_prefix)
{
    int i;
    QString *header = qstring_from_str(
"#version 330\n"
"\n"
"uniform vec2 clipRange;\n"
"uniform vec2 surfaceSize;\n"
"\n"
/* All constants in 1 array declaration */
"uniform vec4 c[" stringify(NV2A_VERTEXSHADER_CONSTANTS) "];\n"
"\n"
"uniform vec4 fogColor;\n"
"uniform float fogParam[2];\n"
"\n"

GLSL_DEFINE(fogPlane, GLSL_C(NV_IGRAPH_XF_XFCTX_FOG))
GLSL_DEFINE(texMat0, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_T0MAT))
GLSL_DEFINE(texMat1, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_T1MAT))
GLSL_DEFINE(texMat2, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_T2MAT))
GLSL_DEFINE(texMat3, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_T3MAT))

"\n"
"vec4 oPos = vec4(0.0,0.0,0.0,1.0);\n"
"vec4 oD0 = vec4(0.0,0.0,0.0,1.0);\n"
"vec4 oD1 = vec4(0.0,0.0,0.0,1.0);\n"
"vec4 oB0 = vec4(0.0,0.0,0.0,1.0);\n"
"vec4 oB1 = vec4(0.0,0.0,0.0,1.0);\n"
"vec4 oPts = vec4(0.0,0.0,0.0,1.0);\n"
/* FIXME: NV_vertex_program says: "FOGC is the transformed vertex's fog
 * coordinate. The register's first floating-point component is interpolated
 * across the assembled primitive during rasterization and used as the fog
 * distance to compute per-fragment the fog factor when fog is enabled.
 * However, if both fog and vertex program mode are enabled, but the FOGC
 * vertex result register is not written, the fog factor is overridden to
 * 1.0. The register's other three components are ignored."
 *
 * That probably means it will read back as vec4(0.0, 0.0, 0.0, 1.0) but
 * will be set to 1.0 AFTER the VP if it was never written?
 * We should test on real hardware..
 *
 * We'll force 1.0 for oFog.x for now.
 */
"vec4 oFog = vec4(1.0,0.0,0.0,1.0);\n"
"vec4 oT0 = vec4(0.0,0.0,0.0,1.0);\n"
"vec4 oT1 = vec4(0.0,0.0,0.0,1.0);\n"
"vec4 oT2 = vec4(0.0,0.0,0.0,1.0);\n"
"vec4 oT3 = vec4(0.0,0.0,0.0,1.0);\n"
"\n"
STRUCT_VERTEX_DATA);

    qstring_append_fmt(header, "noperspective out VertexData %c_vtx;\n",
                       vtx_prefix);
    qstring_append_fmt(header, "#define vtx %c_vtx\n",
                       vtx_prefix);
    qstring_append(header, "\n");
    for(i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        qstring_append_fmt(header, "in vec4 v%d;\n", i);
    }
    qstring_append(header, "\n");

    QString *body = qstring_from_str("void main() {\n");

    if (state.fixed_function) {
        generate_fixed_function(state, header, body);

    } else if (state.vertex_program) {
        vsh_translate(VSH_VERSION_XVS,
                      (uint32_t*)state.program_data,
                      state.program_length,
                      state.z_perspective,
                      header, body);
    } else {
        assert(false);
    }


    /* Fog */

    if (state.fog_enable) {

        if (state.vertex_program) {
            /* FIXME: Does foggen do something here? Let's do some tracking..
             *
             *   "RollerCoaster Tycoon" has
             *      state.vertex_program = true; state.foggen == FOGGEN_PLANAR
             *      but expects oFog.x as fogdistance?! Writes oFog.xyzw = v0.z
             */
            qstring_append(body, "  float fogDistance = oFog.x;\n");
        }

        /* FIXME: Do this per pixel? */

        switch (state.fog_mode) {
        case FOG_MODE_LINEAR:
        case FOG_MODE_LINEAR_ABS:

            /* f = (end - d) / (end - start)
             *    fogParam[1] = 1 / (end - start)
             *    fogParam[0] = 1 + end * fogParam[1];
             */

            qstring_append(body, "  float fogFactor = fogParam[0] + fogDistance * fogParam[1];\n");
            qstring_append(body, "  fogFactor -= 1.0;\n"); /* FIXME: WHHYYY?!! */
            break;
        case FOG_MODE_EXP:
        case FOG_MODE_EXP_ABS:

            /* f = 1 / (e^(d * density))
             *    fogParam[1] = -density / (2 * ln(256))
             *    fogParam[0] = 1.5
             */

            qstring_append(body, "  float fogFactor = fogParam[0] + exp2(fogDistance * fogParam[1] * 16.0);\n");
            qstring_append(body, "  fogFactor -= 1.5;\n"); /* FIXME: WHHYYY?!! */
            break;
        case FOG_MODE_EXP2:
        case FOG_MODE_EXP2_ABS:

            /* f = 1 / (e^((d * density)^2))
             *    fogParam[1] = -density / (2 * sqrt(ln(256)))
             *    fogParam[0] = 1.5
             */

            qstring_append(body, "  float fogFactor = fogParam[0] + exp2(-fogDistance * fogDistance * fogParam[1] * fogParam[1] * 32.0);\n");
            qstring_append(body, "  fogFactor -= 1.5;\n"); /* FIXME: WHHYYY?!! */
            break;
        default:
            assert(false);
            break;
        }
        /* Calculate absolute for the modes which need it */
        switch (state.fog_mode) {
        case FOG_MODE_LINEAR_ABS:
        case FOG_MODE_EXP_ABS:
        case FOG_MODE_EXP2_ABS:
            qstring_append(body, "  fogFactor = abs(fogFactor);\n");
            break;
        default:
            break;
        }
        /* FIXME: What about fog alpha?! */
        qstring_append(body, "  oFog.xyzw = vec4(fogFactor);\n");
    } else {
        /* FIXME: Is the fog still calculated / passed somehow?!
         */
        qstring_append(body, "  oFog.xyzw = vec4(1.0);\n");
    }

    /* Set outputs */
    qstring_append(body, "\n"
                      "  vtx.D0 = clamp(oD0, 0.0, 1.0) * vtx.inv_w;\n"
                      "  vtx.B0 = clamp(oB0, 0.0, 1.0) * vtx.inv_w;\n");
    /* Native port: with SET_SPECULAR_ENABLE off the specular outputs are
     * (0,0,0,1), as xemu 36334d95 vsh.c does for both pipelines. */
    if (state.specular_enable) {
        qstring_append(body,
                      "  vtx.D1 = clamp(oD1, 0.0, 1.0) * vtx.inv_w;\n"
                      "  vtx.B1 = clamp(oB1, 0.0, 1.0) * vtx.inv_w;\n");
    } else {
        qstring_append(body,
                      "  vtx.D1 = vec4(0.0, 0.0, 0.0, 1.0) * vtx.inv_w;\n"
                      "  vtx.B1 = vec4(0.0, 0.0, 0.0, 1.0) * vtx.inv_w;\n");
    }
    qstring_append(body,
                      "  vtx.Fog = oFog.x * vtx.inv_w;\n"
                      "  vtx.T0 = oT0 * vtx.inv_w;\n"
                      "  vtx.T1 = oT1 * vtx.inv_w;\n"
                      "  vtx.T2 = oT2 * vtx.inv_w;\n"
                      "  vtx.T3 = oT3 * vtx.inv_w;\n"
                      "  gl_Position = oPos;\n"
                      "  gl_PointSize = oPts.x;\n");
    qstring_append_fmt(body, "  gl_PointSize *= %u.0;\n}\n", Nv2aGlRenderScale());


    /* Return combined header + source */
    qstring_append(header, qstring_get_str(body));
    qobject_unref(body);
    return header;

}

/*static*/ GLuint create_gl_shader(GLenum gl_shader_type,
                               const char *code,
                               const char *name)
{
    GLint compiled = 0;

    NV2A_GL_DGROUP_BEGIN("Creating new %s", name);

    NV2A_DPRINTF("compile new %s, code:\n%s\n", name, code);

    GLuint shader = glCreateShader(gl_shader_type);
    glShaderSource(shader, 1, &code, NULL);
    glCompileShader(shader);

    /* Check it compiled */
    compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLchar* log;
        GLint log_length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        log = (GLchar*)malloc(log_length * sizeof(GLchar));
        glGetShaderInfoLog(shader, log_length, NULL, log);
        fprintf(stderr, "nv2a: %s compilation failed: %s\n", name, log);
        free(log);

        NV2A_GL_DGROUP_END();
        abort();
    }

    NV2A_GL_DGROUP_END();

    return shader;
}

extern "C" void Nv2aGlDumpShader(const char* kind, const char* code);
ShaderBinding* generate_shaders(const ShaderState state)
{
    int i, j;
    char tmp[64];

    char vtx_prefix;
    GLuint program = glCreateProgram();

    /* Create an option geometry shader and find primitive type */

    GLenum gl_primitive_mode;
    QString* geometry_shader_code =
        generate_geometry_shader(state.polygon_front_mode,
                                 state.polygon_back_mode,
                                 state.primitive_mode,
                                 &gl_primitive_mode,
                                 state.wide_lines);
    if (geometry_shader_code) {
        const char* geometry_shader_code_str =
             qstring_get_str(geometry_shader_code);

        GLuint geometry_shader = create_gl_shader(GL_GEOMETRY_SHADER,
                                                  geometry_shader_code_str,
                                                  "geometry shader");
        glAttachShader(program, geometry_shader);

        qobject_unref(geometry_shader_code);

        vtx_prefix = 'v';
    } else {
        vtx_prefix = 'g';
    }

    /* create the vertex shader */

    QString *vertex_shader_code = generate_vertex_shader(state, vtx_prefix);
    GLuint vertex_shader = create_gl_shader(GL_VERTEX_SHADER,
                                            qstring_get_str(vertex_shader_code),
                                            "vertex shader");
    { char kind[32]; snprintf(kind, sizeof kind, "vert program %u", program); Nv2aGlDumpShader(kind, qstring_get_str(vertex_shader_code)); }
    glAttachShader(program, vertex_shader);
    qobject_unref(vertex_shader_code);


    /* Bind attributes for vertices */
    for(i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        snprintf(tmp, sizeof(tmp), "v%d", i);
        glBindAttribLocation(program, i, tmp);
    }


    /* generate a fragment shader from register combiners */

    QString *fragment_shader_code = psh_translate(state.psh);

    const char *fragment_shader_code_str = qstring_get_str(fragment_shader_code);
    { char kind[32]; snprintf(kind, sizeof kind, "frag program %u", program); Nv2aGlDumpShader(kind, fragment_shader_code_str); }

    GLuint fragment_shader = create_gl_shader(GL_FRAGMENT_SHADER,
                                              fragment_shader_code_str,
                                              "fragment shader");
    glAttachShader(program, fragment_shader);

    qobject_unref(fragment_shader_code);


    /* link the program */
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if(!linked) {
        GLchar log[2048];
        glGetProgramInfoLog(program, 2048, NULL, log);
        fprintf(stderr, "nv2a: shader linking failed: %s\n", log);
        abort();
    }

    glUseProgram(program);

    /* set texture samplers */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        char samplerName[16];
        snprintf(samplerName, sizeof(samplerName), "texSamp%d", i);
        GLint texSampLoc = glGetUniformLocation(program, samplerName);
        if (texSampLoc >= 0) {
            glUniform1i(texSampLoc, i);
        }
    }

    /* validate the program */
    glValidateProgram(program);
    GLint valid = 0;
    glGetProgramiv(program, GL_VALIDATE_STATUS, &valid);
    if (!valid) {
        GLchar log[1024];
        glGetProgramInfoLog(program, 1024, NULL, log);
        fprintf(stderr, "nv2a: shader validation failed: %s\n", log);
        abort();
    }

    ShaderBinding* ret = (ShaderBinding*)malloc(sizeof(ShaderBinding));
    ret->uniform_cache = new Nv2aUniformCache{};
    ret->gl_program = program;

    ret->gl_primitive_mode = gl_primitive_mode;

    /* lookup fragment shader uniforms */
    for (i=0; i<=8; i++) {
        for (j=0; j<2; j++) {
            snprintf(tmp, sizeof(tmp), "c_%d_%d", i, j);
            ret->psh_constant_loc[i][j] = glGetUniformLocation(program, tmp);
        }
    }
    ret->alpha_ref_loc = glGetUniformLocation(program, "alphaRef");
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) { snprintf(tmp, sizeof(tmp), "texScale%d", i); ret->tex_scale_loc[i] = glGetUniformLocation(program, tmp); }
    for (i = 1; i < NV2A_MAX_TEXTURES; i++) {
        snprintf(tmp, sizeof(tmp), "bumpMat%d", i);
        ret->bump_mat_loc[i] = glGetUniformLocation(program, tmp);
        snprintf(tmp, sizeof(tmp), "bumpScale%d", i);
        ret->bump_scale_loc[i] = glGetUniformLocation(program, tmp);
        snprintf(tmp, sizeof(tmp), "bumpOffset%d", i);
        ret->bump_offset_loc[i] = glGetUniformLocation(program, tmp);
    }

    /* lookup vertex shader uniforms */
    for(i = 0; i < NV2A_VERTEXSHADER_CONSTANTS; i++) {
        snprintf(tmp, sizeof(tmp), "c[%d]", i);
        ret->vsh_constant_loc[i] = glGetUniformLocation(program, tmp);
    }
    ret->surface_size_loc = glGetUniformLocation(program, "surfaceSize");
    ret->clip_range_loc = glGetUniformLocation(program, "clipRange");
    ret->line_width_loc = glGetUniformLocation(program, "lineWidth");
    ret->fog_color_loc = glGetUniformLocation(program, "fogColor");
    ret->fog_param_loc[0] = glGetUniformLocation(program, "fogParam[0]");
    ret->fog_param_loc[1] = glGetUniformLocation(program, "fogParam[1]");

    ret->inv_viewport_loc = glGetUniformLocation(program, "invViewport");
    for (i = 0; i < NV2A_LTCTXA_COUNT; i++) {
        snprintf(tmp, sizeof(tmp), "ltctxa[%d]", i);
        ret->ltctxa_loc[i] = glGetUniformLocation(program, tmp);
    }
    for (i = 0; i < NV2A_LTCTXB_COUNT; i++) {
        snprintf(tmp, sizeof(tmp), "ltctxb[%d]", i);
        ret->ltctxb_loc[i] = glGetUniformLocation(program, tmp);
    }
    for (i = 0; i < NV2A_LTC1_COUNT; i++) {
        snprintf(tmp, sizeof(tmp), "ltc1[%d]", i);
        ret->ltc1_loc[i] = glGetUniformLocation(program, tmp);
    }
    for (i = 0; i < NV2A_MAX_LIGHTS; i++) {
        snprintf(tmp, sizeof(tmp), "lightInfiniteHalfVector%d", i);
        ret->light_infinite_half_vector_loc[i] = glGetUniformLocation(program, tmp);
        snprintf(tmp, sizeof(tmp), "lightInfiniteDirection%d", i);
        ret->light_infinite_direction_loc[i] = glGetUniformLocation(program, tmp);

        snprintf(tmp, sizeof(tmp), "lightLocalPosition%d", i);
        ret->light_local_position_loc[i] = glGetUniformLocation(program, tmp);
        snprintf(tmp, sizeof(tmp), "lightLocalAttenuation%d", i);
        ret->light_local_attenuation_loc[i] = glGetUniformLocation(program, tmp);
    }
    for (i = 0; i < 8; i++) {
        snprintf(tmp, sizeof(tmp), "clipRegion[%d]", i);
        ret->clip_region_loc[i] = glGetUniformLocation(program, tmp);
    }

    return ret;
}

// Native port: GLSL-level deduplication. ShaderState includes the whole
// vertex-program buffer and every combiner field, so states that differ only
// in bytes the generator ignores produced identical sources and paid a full
// compile each (bursts of compiles during e.g. ghoulie death effects). The
// sources are generated first (cheap); identical text reuses the program.
#include <string>
#include <unordered_map>
extern "C" void Nv2aGlLog(const char* fmt, ...);
unsigned long long g_shader_dedup_hits = 0;
ShaderBinding* generate_shaders_dedup(const ShaderState& state, bool* compiled)
{
    GLenum prim = 0;
    QString* g = generate_geometry_shader(state.polygon_front_mode, state.polygon_back_mode, state.primitive_mode, &prim, state.wide_lines);
    QString* v = generate_vertex_shader(state, g ? 'v' : 'g');
    QString* f = psh_translate(state.psh);
    std::string key;   // the GL primitive mode is not part of the program
    key.reserve(65536);
    if (g) key += qstring_get_str(g);
    key += '\x01'; key += qstring_get_str(v); key += '\x01'; key += qstring_get_str(f);
    if (g) qobject_unref(g);
    qobject_unref(v); qobject_unref(f);
    static std::unordered_map<std::string, std::vector<ShaderBinding*>> by_source;
    auto it = by_source.find(key);
    if (it != by_source.end()) {
        *compiled = false; ++g_shader_dedup_hits;
        for (ShaderBinding* b : it->second) if (b->gl_primitive_mode == prim) return b;
        // Same program, other primitive mode: share the GL program and uniform locations.
        ShaderBinding* clone = (ShaderBinding*)malloc(sizeof(ShaderBinding));
        memcpy(clone, it->second.front(), sizeof(ShaderBinding));
        clone->gl_primitive_mode = prim;
        it->second.push_back(clone);
        return clone;
    }
    LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0);
    ShaderBinding* binding = generate_shaders(state);
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
    { static double total_ms = 0; static unsigned n = 0; double ms = 1000.0 * (t1.QuadPart - t0.QuadPart) / fq.QuadPart; total_ms += ms; ++n;
      if (ms > 8.0 || n % 50 == 0) Nv2aGlLog("shader compile %u: %.1f ms (total %.0f ms)", n, ms, total_ms); }
    by_source[std::move(key)].push_back(binding);
    *compiled = true;
    return binding;
}
