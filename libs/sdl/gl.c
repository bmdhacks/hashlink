#define HL_NAME(n) sdl_##n
#include <hl.h>
#include "hlsystem.h"

#if defined(HL_IOS) || defined (HL_TVOS)
#	include <SDL.h>
#	include <SDL_syswm.h>
#	include <OpenGLES/ES3/gl.h>
#	define HL_GLES
#elif defined(HL_MAC)
#	include <SDL.h>
#	include <OpenGL/gl3.h>
#	define glBindImageTexture(...) hl_error("Not supported on OSX")
#	define glDispatchCompute(...) hl_error("Not supported on OSX")
#	define glMemoryBarrier(...) hl_error("Not supported on OSX")
#elif defined(_WIN32)
#	include <SDL.h>
#	include <GL/gl.h>
#	include <GL/glext.h>
#elif defined(HL_CONSOLE)
#	include <graphic/glapi.h>
#elif defined(HL_MESA)
# 	include <GLES3/gl3.h>
#	include <GL/osmesa.h>
#	define HL_GLES
#elif defined(HL_ANDROID)
#	include <SDL.h>
#	include <GLES3/gl32.h>
#	include <GLES3/gl3ext.h>
#	define HL_GLES
#elif defined(HL_GLES31)
// ARM Linux with OpenGL ES 3.1 (e.g., Asahi, Raspberry Pi)
#	include <SDL.h>
#	include <GLES3/gl31.h>
#	include <GLES2/gl2ext.h>
#	define HL_GLES
#else
#	include <SDL.h>
#	include <GL/glcorearb.h>
#endif

#ifdef HL_GLES
#	define GL_IMPORT(fun, t)
#	define ES_NOT_SUPPORTED hl_error("Not supported by GLES")
// Tier 1: Not available in any GLES version
#	define glBindFragDataLocation(...) ES_NOT_SUPPORTED
#	define glGetBufferSubData(...) ES_NOT_SUPPORTED
#	define glPolygonMode(face,mode) if( mode != 0x1B02 ) ES_NOT_SUPPORTED
#	define glGetQueryObjectiv glGetQueryObjectuiv
#	define glClearDepth glClearDepthf
#endif

// Tier 2: Available in GLES 3.1+ but not in GLES 3.0
#if defined(HL_GLES) && !defined(HL_GLES31)
#	define glDispatchCompute(...) ES_NOT_SUPPORTED
#	define glMemoryBarrier(...) ES_NOT_SUPPORTED
#	define glBindImageTexture(...) ES_NOT_SUPPORTED
#	define glGetProgramResourceIndex(...) ES_NOT_SUPPORTED
#endif

// Not in any GLES version (use layout qualifiers in shaders instead)
#if defined(HL_GLES)
#	define glTexImage2DMultisample(...) ES_NOT_SUPPORTED
#	define glShaderStorageBlockBinding(...) ES_NOT_SUPPORTED
#endif

// glFramebufferTexture is GLES 3.2 only
// For GLES 3.1, we'll handle this in the wrapper function instead of a macro
#if defined(HL_GLES31)
// Don't define a macro - we'll use glFramebufferTexture2D in the wrapper
#elif defined(HL_GLES)
#	define glFramebufferTexture(...) ES_NOT_SUPPORTED
#endif

// glColorMaski is GLES 3.2 only
#if defined(HL_GLES31)
#	define glColorMaski(...) ES_NOT_SUPPORTED
#endif

#if !defined(HL_CONSOLE) && !defined(GL_IMPORT)
#define GL_IMPORT(fun, t) PFNGL##t##PROC fun
#include "GLImports.h"
#undef GL_IMPORT
#define GL_IMPORT(fun,t)	fun = (PFNGL##t##PROC)SDL_GL_GetProcAddress(#fun); if( fun == NULL ) return 1
#ifndef __APPLE__
#define GL_IMPORT_OPT(fun, t) PFNGL##t##PROC fun = NULL; if ( !fun ) { fun = (PFNGL##t##PROC)SDL_GL_GetProcAddress(#fun); if( fun == NULL ) hl_error("function not resolved"); }
#endif
#endif

#if !defined GL_IMPORT_OPT
#define GL_IMPORT_OPT(fun, t)
#define glMultiDrawElementsIndirectCountARB(...) hl_error("function not resolved");
#endif

static int GLLoadAPI() {
#	include "GLImports.h"
	return 0;
}

#ifdef GL_VERSION_4_3
static void APIENTRY debug_message_callback( GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam ) {
	fprintf(stderr, "GL %s: type = 0x%x, severity = 0x%x, message = %s\n",
		( type == GL_DEBUG_TYPE_ERROR ? "** ERROR **" : "DEBUG" ),
		type, severity, message);
}
#endif

#define ZIDX(val) ((val)?(val)->v.i:0)

// Debug logging - set to 0 to disable
#define GL_DEBUG_LOG 0

#if GL_DEBUG_LOG
static int gl_debug_frame_count = 0;
static int gl_debug_call_count = 0;

#define GL_CHECK_ERROR(name) do { \
	GLenum err = glGetError(); \
	if (err != GL_NO_ERROR) { \
		fprintf(stderr, "[GL ERROR] %s: 0x%04X\n", name, err); \
	} \
} while(0)

#define GL_LOG(fmt, ...) fprintf(stderr, "[GL] " fmt "\n", ##__VA_ARGS__)
#define GL_LOG_TEX(fmt, ...) fprintf(stderr, "[GL TEX] " fmt "\n", ##__VA_ARGS__)
#else
#define GL_CHECK_ERROR(name)
#define GL_LOG(fmt, ...)
#define GL_LOG_TEX(fmt, ...)
#endif

// globals
HL_PRIM bool HL_NAME(gl_init)() {
	return GLLoadAPI() == 0;
}

HL_PRIM bool HL_NAME(gl_set_debug)( bool enable ) {
#ifdef GL_VERSION_4_3
	if( enable ) {
		glEnable(GL_DEBUG_OUTPUT);
		glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_PERFORMANCE, GL_DONT_CARE, 0, NULL, GL_FALSE);
		glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_OTHER, GL_DONT_CARE, 0, NULL, GL_FALSE);
		glDebugMessageCallback(debug_message_callback, 0);
	} else {
		glDisable(GL_DEBUG_OUTPUT);
	}
	return true;
#else
	return false;
#endif
}

HL_PRIM bool HL_NAME(gl_is_context_lost)() {
	// seems like a GL context is rarely lost on desktop
	// let's look at it again on mobile
	return false;
}

HL_PRIM void HL_NAME(gl_clear)( int bits ) {
	glClear(bits);
}

HL_PRIM int HL_NAME(gl_get_error)() {
	return glGetError();
}

HL_PRIM void HL_NAME(gl_scissor)( int x, int y, int width, int height ) {
	glScissor(x, y, width, height);
}

HL_PRIM void HL_NAME(gl_clear_color)( double r, double g, double b, double a ) {
	glClearColor((float)r, (float)g, (float)b, (float)a);
}

HL_PRIM void HL_NAME(gl_clear_depth)( double value ) {
	glClearDepth(value);
}

HL_PRIM void HL_NAME(gl_clear_stencil)( int value ) {
	glClearStencil(value);
}

HL_PRIM void HL_NAME(gl_viewport)( int x, int y, int width, int height ) {
	glViewport(x, y, width, height);
}

HL_PRIM void HL_NAME(gl_flush)() {
	glFlush();
}

HL_PRIM void HL_NAME(gl_finish)() {
	glFinish();
}

HL_PRIM void HL_NAME(gl_pixel_storei)( int key, int value ) {
	glPixelStorei(key, value);
}

HL_PRIM vbyte *HL_NAME(gl_get_string)(int name) {
	return (vbyte*)glGetString(name);
}

// state changes

HL_PRIM void HL_NAME(gl_polygon_mode)(int face, int mode) {
	glPolygonMode(face, mode);
}

HL_PRIM void HL_NAME(gl_polygon_offset)(float factor, float units) {
	glPolygonOffset(factor, units);
}

HL_PRIM void HL_NAME(gl_enable)( int feature ) {
	glEnable(feature);
}

HL_PRIM void HL_NAME(gl_disable)( int feature ) {
	glDisable(feature);
}

HL_PRIM void HL_NAME(gl_cull_face)( int face ) {
	glCullFace(face);
}

HL_PRIM void HL_NAME(gl_blend_func)( int src, int dst ) {
	glBlendFunc(src, dst);
}

HL_PRIM void HL_NAME(gl_blend_func_separate)( int src, int dst, int alphaSrc, int alphaDst ) {
	glBlendFuncSeparate(src, dst, alphaSrc, alphaDst);
}

HL_PRIM void HL_NAME(gl_blend_equation)( int op ) {
	glBlendEquation(op);
}

HL_PRIM void HL_NAME(gl_blend_equation_separate)( int op, int alphaOp ) {
	glBlendEquationSeparate(op, alphaOp);
}

HL_PRIM void HL_NAME(gl_depth_mask)( bool mask ) {
	glDepthMask(mask);
}

HL_PRIM void HL_NAME(gl_depth_func)( int f ) {
	glDepthFunc(f);
}

HL_PRIM void HL_NAME(gl_color_mask)( bool r, bool g, bool b, bool a ) {
	glColorMask(r, g, b, a);
}

HL_PRIM void HL_NAME(gl_color_maski)( int i, bool r, bool g, bool b, bool a ) {
	glColorMaski(i, r, g, b, a);
}

HL_PRIM void HL_NAME(gl_stencil_mask_separate)(int face, int mask) {
	glStencilMaskSeparate(face, mask);
}

HL_PRIM void HL_NAME(gl_stencil_func_separate)(int face, int func, int ref, int mask ) {
	glStencilFuncSeparate(face, func, ref, mask);
}

HL_PRIM void HL_NAME(gl_stencil_op_separate)(int face, int sfail, int dpfail, int dppass) {
	glStencilOpSeparate(face, sfail, dpfail, dppass);
}

// program

static vdynamic *alloc_i32(int v) {
	vdynamic *ret;
	ret = hl_alloc_dynamic(&hlt_i32);
	ret->v.i = v;
	return ret;
}

HL_PRIM vdynamic *HL_NAME(gl_create_program)() {
	int v = glCreateProgram();
	if( v == 0 ) return NULL;
	return alloc_i32(v);
}

HL_PRIM void HL_NAME(gl_delete_program)( vdynamic *s ) {
	glDeleteProgram(s->v.i);
}

HL_PRIM void HL_NAME(gl_bind_frag_data_location)( vdynamic *p, int colNum, vstring *name ) {
	char *cname = hl_to_utf8(name->bytes);
	glBindFragDataLocation(p->v.i, colNum, cname);
}

HL_PRIM void HL_NAME(gl_attach_shader)( vdynamic *p, vdynamic *s ) {
	glAttachShader(p->v.i, s->v.i);
}

HL_PRIM void HL_NAME(gl_link_program)( vdynamic *p ) {
	GL_LOG("glLinkProgram(%d)", p->v.i);
	glLinkProgram(p->v.i);
	int status = 0;
	glGetProgramiv(p->v.i, 0x8B82/*GL_LINK_STATUS*/, &status);
	if (!status) {
		char log[1024];
		glGetProgramInfoLog(p->v.i, sizeof(log), NULL, log);
		fprintf(stderr, "[GL ERROR] Program %d link failed: %s\n", p->v.i, log);
	}
	GL_CHECK_ERROR("glLinkProgram");
}

HL_PRIM vdynamic *HL_NAME(gl_get_program_parameter)( vdynamic *p, int param ) {
	switch( param ) {
	case 0x8B82 /*LINK_STATUS*/ : {
		int ret = 0;
		glGetProgramiv(p->v.i, param, &ret);
		return alloc_i32(ret);
	}
	default:
		hl_error("Unsupported param %d",param);
	}
	return NULL;
}

HL_PRIM vbyte *HL_NAME(gl_get_program_info_bytes)( vdynamic *p ) {
	char log[4096];
	*log = 0;
	glGetProgramInfoLog(p->v.i, 4096, NULL, log);
	return hl_copy_bytes((vbyte*)log,(int)strlen(log) + 1);
}

HL_PRIM vdynamic *HL_NAME(gl_get_uniform_location)( vdynamic *p, vstring *name ) {
	char *cname = hl_to_utf8(name->bytes);
	int u = glGetUniformLocation(p->v.i, cname);
	if( u < 0 ) return NULL;
	return alloc_i32(u);
}

HL_PRIM int HL_NAME(gl_get_attrib_location)( vdynamic *p, vstring *name ) {
	char *cname = hl_to_utf8(name->bytes);
	return glGetAttribLocation(p->v.i, cname);
}

HL_PRIM void HL_NAME(gl_use_program)( vdynamic *p ) {
	glUseProgram(ZIDX(p));
}

// shader

HL_PRIM vdynamic *HL_NAME(gl_create_shader)( int type ) {
	int s = glCreateShader(type);
	if (s == 0) return NULL;
	return alloc_i32(s);
}

HL_PRIM void HL_NAME(gl_shader_source)( vdynamic *s, vstring *src ) {
	const GLchar *c = (GLchar*)hl_to_utf8(src->bytes);
	glShaderSource(s->v.i, 1, &c, NULL);
}

HL_PRIM void HL_NAME(gl_compile_shader)( vdynamic *s ) {
	GL_LOG("glCompileShader(%d)", s->v.i);
	glCompileShader(s->v.i);
	int status = 0;
	glGetShaderiv(s->v.i, 0x8B81/*GL_COMPILE_STATUS*/, &status);
	if (!status) {
		char log[1024];
		glGetShaderInfoLog(s->v.i, sizeof(log), NULL, log);
		fprintf(stderr, "[GL ERROR] Shader %d compile failed: %s\n", s->v.i, log);
	}
	GL_CHECK_ERROR("glCompileShader");
}

HL_PRIM vbyte *HL_NAME(gl_get_shader_info_bytes)( vdynamic *s ) {
	char log[4096];
	*log = 0;
	glGetShaderInfoLog(s->v.i, 4096, NULL, log);
	return hl_copy_bytes((vbyte*)log, (int)strlen(log)+1);
}

HL_PRIM vdynamic *HL_NAME(gl_get_shader_parameter)( vdynamic *s, int param ) {
	switch( param ) {
	case 0x8B81/*COMPILE_STATUS*/:
	case 0x8B4F/*SHADER_TYPE*/:
	case 0x8B80/*DELETE_STATUS*/:
	{
		int ret = 0;
		glGetShaderiv(s->v.i, param, &ret);
		return alloc_i32(ret);
	}
	default:
		hl_error("Unsupported param %d", param);
	}
	return NULL;
}

HL_PRIM void HL_NAME(gl_delete_shader)( vdynamic *s ) {
	glDeleteShader(s->v.i);
}

// texture

HL_PRIM vdynamic *HL_NAME(gl_create_texture)() {
	unsigned int t = 0;
	glGenTextures(1, &t);
	GL_LOG_TEX("glGenTextures -> %u", t);
	GL_CHECK_ERROR("glGenTextures");
	return alloc_i32(t);
}

HL_PRIM void HL_NAME(gl_active_texture)( int t ) {
	GL_LOG_TEX("glActiveTexture(0x%04X) [unit %d]", t, t - 0x84C0);
	glActiveTexture(t);
}

HL_PRIM void HL_NAME(gl_bind_texture)( int t, vdynamic *texture ) {
	GL_LOG_TEX("glBindTexture(target=0x%04X, tex=%d)", t, ZIDX(texture));
	glBindTexture(t, ZIDX(texture));
	GL_CHECK_ERROR("glBindTexture");
}

HL_PRIM void HL_NAME(gl_bind_image_texture)( int unit, int texture, int level, bool layered, int layer, int access, int format ) {
	glBindImageTexture(unit, texture, level, layered, layer, access, format);
}

HL_PRIM void HL_NAME(gl_tex_parameterf)( int t, int key, float value ) {
	glTexParameterf(t, key, value);
}

HL_PRIM void HL_NAME(gl_tex_parameteri)( int t, int key, int value ) {
	glTexParameteri(t, key, value);
}

HL_PRIM void HL_NAME(gl_tex_image2d)( int target, int level, int internalFormat, int width, int height, int border, int format, int type, vbyte *image ) {
	GL_LOG_TEX("glTexImage2D(target=0x%04X, level=%d, internalFmt=0x%04X, %dx%d, fmt=0x%04X, type=0x%04X, data=%p)",
		target, level, internalFormat, width, height, format, type, (void*)image);
	glTexImage2D(target, level, internalFormat, width, height, border, format, type, image);
	GL_CHECK_ERROR("glTexImage2D");
}

HL_PRIM void HL_NAME(gl_tex_image3d)( int target, int level, int internalFormat, int width, int height, int depth, int border, int format, int type, vbyte *image ) {
	GL_LOG_TEX("glTexImage3D(target=0x%04X, level=%d, internalFmt=0x%04X, %dx%dx%d, fmt=0x%04X, type=0x%04X, data=%p)",
		target, level, internalFormat, width, height, depth, format, type, (void*)image);
	glTexImage3D(target, level, internalFormat, width, height, depth, border, format, type, image);
	GL_CHECK_ERROR("glTexImage3D");
}

HL_PRIM void HL_NAME(gl_tex_storage2d)( int target, int levels, int internalFormat, int width, int height) {
#ifndef __APPLE__
	GL_LOG_TEX("glTexStorage2D(target=0x%04X, levels=%d, internalFmt=0x%04X, %dx%d)",
		target, levels, internalFormat, width, height);
	glTexStorage2D(target, levels, internalFormat, width, height);
	GL_CHECK_ERROR("glTexStorage2D");
#else
    hl_error("glTexStorage2d is not supported on Apple platforms");
#endif
}

HL_PRIM void HL_NAME(gl_tex_storage3d)( int target, int levels, int internalFormat, int width, int height, int depth) {
#ifndef __APPLE__
	GL_LOG_TEX("glTexStorage3D(target=0x%04X, levels=%d, internalFmt=0x%04X, %dx%dx%d)",
		target, levels, internalFormat, width, height, depth);
	glTexStorage3D(target, levels, internalFormat, width, height, depth);
	GL_CHECK_ERROR("glTexStorage3D");
#else
	hl_error("glTexStorage3d is not supported on Apple platforms");
#endif
}

HL_PRIM void HL_NAME(gl_tex_image2d_multisample)( int target, int samples, int internalFormat, int width, int height, bool fixedsamplelocations) {
	glTexImage2DMultisample(target, samples, internalFormat, width, height, fixedsamplelocations);
}

HL_PRIM void HL_NAME(gl_compressed_tex_image2d)( int target, int level, int internalFormat, int width, int height, int border, int imageSize, vbyte *image ) {
	GL_LOG_TEX("glCompressedTexImage2D(target=0x%04X, level=%d, internalFmt=0x%04X, %dx%d, size=%d, data=%p)",
		target, level, internalFormat, width, height, imageSize, (void*)image);
	glCompressedTexImage2D(target,level,internalFormat,width,height,border,imageSize,image);
	GL_CHECK_ERROR("glCompressedTexImage2D");
}

HL_PRIM void HL_NAME(gl_compressed_tex_image3d)( int target, int level, int internalFormat, int width, int height, int depth, int border, int imageSize, vbyte *image ) {
	GL_LOG_TEX("glCompressedTexImage3D(target=0x%04X, level=%d, internalFmt=0x%04X, %dx%dx%d, size=%d, data=%p)",
		target, level, internalFormat, width, height, depth, imageSize, (void*)image);
	glCompressedTexImage3D(target,level,internalFormat,width,height,depth,border,imageSize,image);
	GL_CHECK_ERROR("glCompressedTexImage3D");
}

HL_PRIM void HL_NAME(gl_tex_sub_image2d)(int target, int level, int xoffset, int yoffset, int width, int height, int format, int type, vbyte *image) {
	GL_LOG_TEX("glTexSubImage2D(target=0x%04X, level=%d, offset=%d,%d, %dx%d, fmt=0x%04X, type=0x%04X, data=%p)",
		target, level, xoffset, yoffset, width, height, format, type, (void*)image);
	glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, image);
	GL_CHECK_ERROR("glTexSubImage2D");
}

HL_PRIM void HL_NAME(gl_tex_sub_image3d)(int target, int level, int xoffset, int yoffset, int zoffset, int width, int height, int depth, int format, int type, vbyte *image) {
	GL_LOG_TEX("glTexSubImage3D(target=0x%04X, level=%d, offset=%d,%d,%d, %dx%dx%d, fmt=0x%04X, type=0x%04X, data=%p)",
		target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, (void*)image);
	glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, image);
	GL_CHECK_ERROR("glTexSubImage3D");
}

HL_PRIM void HL_NAME(gl_compressed_tex_sub_image2d)(int target, int level, int xoffset, int yoffset, int width, int height, int format, int type, vbyte *image) {
	GL_LOG_TEX("glCompressedTexSubImage2D(target=0x%04X, level=%d, offset=%d,%d, %dx%d, fmt=0x%04X, size=%d, data=%p)",
		target, level, xoffset, yoffset, width, height, format, type, (void*)image);
	glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, image);
	GL_CHECK_ERROR("glCompressedTexSubImage2D");
}

HL_PRIM void HL_NAME(gl_compressed_tex_sub_image3d)(int target, int level, int xoffset, int yoffset, int zoffset, int width, int height, int depth, int format, int type, vbyte *image) {
	GL_LOG_TEX("glCompressedTexSubImage3D(target=0x%04X, level=%d, offset=%d,%d,%d, %dx%dx%d, fmt=0x%04X, size=%d, data=%p)",
		target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, (void*)image);
	glCompressedTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, image);
	GL_CHECK_ERROR("glCompressedTexSubImage3D");
}

HL_PRIM void HL_NAME(gl_generate_mipmap)( int t ) {
	GL_LOG_TEX("glGenerateMipmap(target=0x%04X)", t);
	glGenerateMipmap(t);
	GL_CHECK_ERROR("glGenerateMipmap");
}

HL_PRIM void HL_NAME(gl_delete_texture)( vdynamic *t ) {
	unsigned int tt = t->v.i;
	glDeleteTextures(1, &tt);
}

// framebuffer

HL_PRIM void HL_NAME(gl_blit_framebuffer)(int src_x0, int src_y0, int src_x1, int src_y1, int dst_x0, int dst_y0, int dst_x1, int dst_y1, int mask, int filter) {
	glBlitFramebuffer(src_x0, src_y0, src_x1, src_y1, dst_x0, dst_y0, dst_x1, dst_y1, mask, filter);
}

HL_PRIM vdynamic *HL_NAME(gl_create_framebuffer)() {
	unsigned int f = 0;
	glGenFramebuffers(1, &f);
	GL_LOG("glGenFramebuffers -> %u", f);
	return alloc_i32(f);
}

HL_PRIM void HL_NAME(gl_bind_framebuffer)( int target, vdynamic *f ) {
	unsigned int id = ZIDX(f);
#if	defined(HL_IOS) || defined(HL_TVOS)
	if ( id==0 ) {
		SDL_SysWMinfo info;
		SDL_VERSION(&info.version);
		SDL_GetWindowWMInfo(SDL_GL_GetCurrentWindow(), &info);
		id = info.info.uikit.framebuffer;
	}
#endif
	GL_LOG("glBindFramebuffer(target=0x%04X, fb=%u)", target, id);
	glBindFramebuffer(target, id);
#if GL_DEBUG_LOG
	if (id != 0) {
		GLenum status = glCheckFramebufferStatus(target);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			fprintf(stderr, "[GL ERROR] Framebuffer %u incomplete: 0x%04X\n", id, status);
		}
	}
#endif
}

HL_PRIM void HL_NAME(gl_framebuffer_texture)( int target, int attach, vdynamic *t, int level ) {
	GL_LOG("glFramebufferTexture(target=0x%04X, attach=0x%04X, tex=%d, level=%d)", target, attach, ZIDX(t), level);
#if defined(HL_GLES31)
	// GLES 3.1 doesn't have glFramebufferTexture, use glFramebufferTexture2D for 2D textures
	// This assumes the texture is GL_TEXTURE_2D which is the common case
	glFramebufferTexture2D(target, attach, GL_TEXTURE_2D, ZIDX(t), level);
	GL_CHECK_ERROR("glFramebufferTexture2D (GLES31 fallback)");
#else
	glFramebufferTexture(target, attach, ZIDX(t), level);
	GL_CHECK_ERROR("glFramebufferTexture");
#endif
}

HL_PRIM void HL_NAME(gl_framebuffer_texture2d)( int target, int attach, int texTarget, vdynamic *t, int level ) {
	GL_LOG("glFramebufferTexture2D(target=0x%04X, attach=0x%04X, texTarget=0x%04X, tex=%d, level=%d)", target, attach, texTarget, ZIDX(t), level);
	glFramebufferTexture2D(target, attach, texTarget, ZIDX(t), level);
	GL_CHECK_ERROR("glFramebufferTexture2D");
}

HL_PRIM void HL_NAME(gl_framebuffer_texture_layer)( int target, int attach, vdynamic *t, int level, int layer ) {
	GL_LOG("glFramebufferTextureLayer(target=0x%04X, attach=0x%04X, tex=%d, level=%d, layer=%d)", target, attach, ZIDX(t), level, layer);
	glFramebufferTextureLayer(target, attach, ZIDX(t), level, layer);
	GL_CHECK_ERROR("glFramebufferTextureLayer");
}

HL_PRIM void HL_NAME(gl_delete_framebuffer)( vdynamic *f ) {
	unsigned int ff = (unsigned)f->v.i;
	glDeleteFramebuffers(1, &ff);
}

HL_PRIM void HL_NAME(gl_read_pixels)( int x, int y, int width, int height, int format, int type, vbyte *data ) {
	glReadPixels(x, y, width, height, format, type, data);
}

HL_PRIM void HL_NAME(gl_read_buffer)( int mode ) {
	glReadBuffer(mode);
}

HL_PRIM void HL_NAME(gl_draw_buffers)( int count, unsigned int *buffers) {
	glDrawBuffers(count, buffers);
}

// renderbuffer

HL_PRIM vdynamic *HL_NAME(gl_create_renderbuffer)() {
	unsigned int buf = 0;
	glGenRenderbuffers(1, &buf);
	GL_LOG("glGenRenderbuffers -> %u", buf);
	return alloc_i32(buf);
}

HL_PRIM void HL_NAME(gl_bind_renderbuffer)( int target, vdynamic *r ) {
	unsigned int id = ZIDX(r);
#if	defined(HL_IOS) || defined(HL_TVOS)
	if ( id==0 ) {
		SDL_SysWMinfo info;
		SDL_VERSION(&info.version);
		SDL_GetWindowWMInfo(SDL_GL_GetCurrentWindow(), &info);
		id = info.info.uikit.colorbuffer;
	}
#endif
	glBindRenderbuffer(GL_RENDERBUFFER, id);
}

HL_PRIM void HL_NAME(gl_renderbuffer_storage)( int target, int format, int width, int height ) {
#if defined(HL_GLES)
	// GLES requires sized internal formats - translate unsized ones
	if (format == GL_DEPTH_STENCIL) {
		format = GL_DEPTH24_STENCIL8;
		GL_LOG("glRenderbufferStorage(target=0x%04X, format=0x%04X [translated from GL_DEPTH_STENCIL], %dx%d)", target, format, width, height);
	} else if (format == GL_DEPTH_COMPONENT) {
		format = GL_DEPTH_COMPONENT24;
		GL_LOG("glRenderbufferStorage(target=0x%04X, format=0x%04X [translated from GL_DEPTH_COMPONENT], %dx%d)", target, format, width, height);
	} else {
		GL_LOG("glRenderbufferStorage(target=0x%04X, format=0x%04X, %dx%d)", target, format, width, height);
	}
#else
	GL_LOG("glRenderbufferStorage(target=0x%04X, format=0x%04X, %dx%d)", target, format, width, height);
#endif
	glRenderbufferStorage(target, format, width, height);
	GL_CHECK_ERROR("glRenderbufferStorage");
}


HL_PRIM void HL_NAME(gl_renderbuffer_storage_multisample)( int target, int samples, int format, int width, int height ) {
#if defined(HL_GLES)
	// GLES requires sized internal formats - translate unsized ones
	if (format == GL_DEPTH_STENCIL) {
		format = GL_DEPTH24_STENCIL8;
	} else if (format == GL_DEPTH_COMPONENT) {
		format = GL_DEPTH_COMPONENT24;
	}
#endif
	GL_LOG("glRenderbufferStorageMultisample(target=0x%04X, samples=%d, format=0x%04X, %dx%d)", target, samples, format, width, height);
	glRenderbufferStorageMultisample(target, samples, format, width, height);
	GL_CHECK_ERROR("glRenderbufferStorageMultisample");
}

HL_PRIM void HL_NAME(gl_framebuffer_renderbuffer)( int frameTarget, int attach, int renderTarget, vdynamic *b ) {
	GL_LOG("glFramebufferRenderbuffer(target=0x%04X, attach=0x%04X, rb=%d)", frameTarget, attach, ZIDX(b));
	glFramebufferRenderbuffer(frameTarget, attach, renderTarget, ZIDX(b));
	GL_CHECK_ERROR("glFramebufferRenderbuffer");
}

HL_PRIM void HL_NAME(gl_delete_renderbuffer)( vdynamic *b ) {
	unsigned int bb = (unsigned)b->v.i;
	glDeleteRenderbuffers(1, &bb);
}

// buffer

HL_PRIM vdynamic *HL_NAME(gl_create_buffer)() {
	unsigned int b = 0;
	glGenBuffers(1, &b);
	return alloc_i32(b);
}

HL_PRIM void HL_NAME(gl_bind_buffer)( int target, vdynamic *b ) {
	glBindBuffer(target, ZIDX(b));
}

HL_PRIM void HL_NAME(gl_bind_buffer_base)( int target, int index, vdynamic *b ) {
	glBindBufferBase(target, index, ZIDX(b));
}

HL_PRIM void HL_NAME(gl_buffer_data_size)( int target, int size, int param ) {
	glBufferData(target, size, NULL, param);
}

HL_PRIM void HL_NAME(gl_buffer_data)( int target, int size, vbyte *data, int param ) {
	glBufferData(target, size, data, param);
}

HL_PRIM void HL_NAME(gl_buffer_sub_data)( int target, int offset, vbyte *data, int srcOffset, int srcLength ) {
	glBufferSubData(target, offset, srcLength, data + srcOffset);
}

HL_PRIM void HL_NAME(gl_get_buffer_sub_data)( int target, int offset, vbyte *data, int srcOffset, int srcLength ) {
	glGetBufferSubData(target, srcOffset, srcLength, data + offset);
}

HL_PRIM void HL_NAME(gl_enable_vertex_attrib_array)( int attrib ) {
	glEnableVertexAttribArray(attrib);
}

HL_PRIM void HL_NAME(gl_disable_vertex_attrib_array)( int attrib ) {
	glDisableVertexAttribArray(attrib);
}

HL_PRIM void HL_NAME(gl_vertex_attrib_pointer)( int index, int size, int type, bool normalized, int stride, int position ) {
	glVertexAttribPointer(index, size, type, normalized, stride, (void*)(int_val)position);
}

HL_PRIM void HL_NAME(gl_vertex_attrib_ipointer)( int index, int size, int type, int stride, int position ) {
	glVertexAttribIPointer(index, size, type, stride, (void*)(int_val)position);
}

HL_PRIM void HL_NAME(gl_vertex_attrib_divisor)( int index, int divisor ) {
	glVertexAttribDivisor(index, divisor);
}

HL_PRIM void HL_NAME(gl_delete_buffer)( vdynamic *b ) {
	unsigned int bb = (unsigned)b->v.i;
	glDeleteBuffers(1, &bb);
}

// uniforms

HL_PRIM void HL_NAME(gl_uniform1i)( vdynamic *u, int i ) {
	glUniform1i(u->v.i, i);
}

HL_PRIM void HL_NAME(gl_uniform4fv)( vdynamic *u, vbyte *buffer, int bufPos, int count ) {
	glUniform4fv(u->v.i, count, (float*)buffer + bufPos);
}

HL_PRIM void HL_NAME(gl_uniform_matrix4fv)( vdynamic *u, bool transpose, vbyte *buffer, int bufPos, int count ) {
	glUniformMatrix4fv(u->v.i, count, transpose ? GL_TRUE : GL_FALSE, (float*)buffer + bufPos);
}

// compute
HL_PRIM void HL_NAME(gl_dispatch_compute)( int num_groups_x, int num_groups_y, int num_groups_z ) {
	glDispatchCompute(num_groups_x, num_groups_y, num_groups_z);
}

HL_PRIM void HL_NAME(gl_memory_barrier)( int barriers ) {
	glMemoryBarrier(barriers);
}

// draw

HL_PRIM void HL_NAME(gl_draw_elements)( int mode, int count, int type, int start ) {
	glDrawElements(mode, count, type, (void*)(int_val)start);
#if GL_DEBUG_LOG
	gl_debug_call_count++;
	GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		fprintf(stderr, "[GL ERROR] glDrawElements(mode=0x%04X, count=%d): 0x%04X\n", mode, count, err);
	}
	if ((gl_debug_call_count % 1000) == 0) {
		GL_LOG("glDrawElements [call #%d]", gl_debug_call_count);
	}
#endif
}

HL_PRIM void HL_NAME(gl_draw_arrays)( int mode, int first, int count, int start ) {
	glDrawArrays(mode,first,count);
	GL_CHECK_ERROR("glDrawArrays");
}

HL_PRIM void HL_NAME(gl_draw_elements_instanced)( int mode, int count, int type, int start, int primcount ) {
	glDrawElementsInstanced(mode,count,type,(void*)(int_val)start,primcount);
	GL_CHECK_ERROR("glDrawElementsInstanced");
}

HL_PRIM void HL_NAME(gl_draw_arrays_instanced)( int mode, int first, int count, int primcount ) {
	glDrawArraysInstanced(mode,first,count,primcount);
	GL_CHECK_ERROR("glDrawArraysInstanced");
}

HL_PRIM void HL_NAME(gl_multi_draw_elements_indirect)( int mode, int type, vbyte *data, int count, int stride ) {
#	ifdef GL_VERSION_4_3
	glMultiDrawElementsIndirect(mode, type, data, count, stride);
#	endif
}

HL_PRIM void HL_NAME(gl_multi_draw_elements_indirect_count)(int mode, int type, vbyte* data, vbyte* drawcount, int maxdrawcount, int stride) {
	GL_IMPORT_OPT(glMultiDrawElementsIndirectCountARB, MULTIDRAWELEMENTSINDIRECTCOUNTARB)
	glMultiDrawElementsIndirectCountARB(mode, type, data, (GLintptr)drawcount, maxdrawcount, stride);
}

HL_PRIM int HL_NAME(gl_get_config_parameter)( int feature ) {
	switch( feature ) {
	case 0:
#		ifdef GL_VERSION_4_3
		return 1;
#		else
		return 0;
#		endif
	default:
		{
			int r = -1;
			glGetIntegerv(feature, &r);
			return r;
		}
		break;
	}
	return -1;
}

HL_PRIM bool HL_NAME(gl_has_extension)(vstring *name) {
	const char* cname = hl_to_utf8(name->bytes);
	GLint numExtensions = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);
	for (int i = 0; i < numExtensions; i++) {
		const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
		if (ext && strcmp(cname, ext) == 0) {
			return true;
		}
	}
	return false;
}

// queries

HL_PRIM vdynamic *HL_NAME(gl_create_query)() {
	unsigned int t = 0;
	glGenQueries(1, &t);
	return alloc_i32(t);
}

HL_PRIM void HL_NAME(gl_delete_query)( vdynamic *q ) {
	glDeleteQueries(1, (const GLuint *) &q->v.i);
}

HL_PRIM void HL_NAME(gl_begin_query)( int target, vdynamic *q ) {
	glBeginQuery(target,q->v.i);
}

HL_PRIM void HL_NAME(gl_end_query)( int target ) {
	glEndQuery(target);
}

HL_PRIM bool HL_NAME(gl_query_result_available)( vdynamic *q ) {
	int v = 0;
	glGetQueryObjectiv(q->v.i, GL_QUERY_RESULT_AVAILABLE, &v);
	return v == GL_TRUE;
}

HL_PRIM double HL_NAME(gl_query_result)( vdynamic *q ) {
	GLuint64 v = -1;
#	if !defined(HL_MESA) && !defined(HL_MOBILE) && !defined(HL_GLES31)
	glGetQueryObjectui64v(q->v.i, GL_QUERY_RESULT, &v);
#	endif
	return (double)v;
}

HL_PRIM void HL_NAME(gl_query_counter)( vdynamic *q, int target ) {
#	if !defined(HL_MESA) && !defined(HL_MOBILE) && !defined(HL_GLES31)
	glQueryCounter(q->v.i, target);
#	endif
}

// vertex array

HL_PRIM vdynamic *HL_NAME(gl_create_vertex_array)() {
	unsigned int f = 0;
	glGenVertexArrays(1, &f);
	return alloc_i32(f);
}

HL_PRIM void HL_NAME(gl_bind_vertex_array)( vdynamic *b ) {
	unsigned int bb = (unsigned)b->v.i;
	glBindVertexArray(bb);
}

HL_PRIM void HL_NAME(gl_delete_vertex_array)( vdynamic *b ) {
	unsigned int bb = (unsigned)b->v.i;
	glDeleteVertexArrays(1, &bb);
}

// uniform buffer

HL_PRIM int HL_NAME(gl_get_uniform_block_index)( vdynamic *p, vstring *name ) {
	char *cname = hl_to_utf8(name->bytes);
	return (int)glGetUniformBlockIndex(p->v.i, cname);
}

HL_PRIM void HL_NAME(gl_uniform_block_binding)( vdynamic *p, int index, int binding ) {
	glUniformBlockBinding(p->v.i, index, binding);
}

// SSBOs

HL_PRIM int HL_NAME(gl_get_program_resource_index)( vdynamic *p, int type, vstring *name ) {
#ifndef __APPLE__
	char *cname = hl_to_utf8(name->bytes);
	return (int)glGetProgramResourceIndex(p->v.i, type, cname);
#else
	hl_error("glGetProgramResourceIndex is not supported on Apple platforms");
#endif
}

HL_PRIM void HL_NAME(gl_shader_storage_block_binding)( vdynamic *p, int index, int binding ) {
#ifndef __APPLE__
	glShaderStorageBlockBinding(p->v.i, index, binding);
#else
	hl_error("glShaderStorageBlockBinding is not supported on Apple platforms");
#endif
}

DEFINE_PRIM(_BOOL,gl_init,_NO_ARG);
DEFINE_PRIM(_BOOL,gl_set_debug,_BOOL);
DEFINE_PRIM(_BOOL,gl_is_context_lost,_NO_ARG);
DEFINE_PRIM(_VOID,gl_clear,_I32);
DEFINE_PRIM(_I32,gl_get_error,_NO_ARG);
DEFINE_PRIM(_VOID,gl_scissor,_I32 _I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_clear_color,_F64 _F64 _F64 _F64);
DEFINE_PRIM(_VOID,gl_clear_depth,_F64);
DEFINE_PRIM(_VOID,gl_clear_stencil,_I32);
DEFINE_PRIM(_VOID,gl_viewport,_I32 _I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_finish,_NO_ARG);
DEFINE_PRIM(_VOID,gl_flush,_NO_ARG);
DEFINE_PRIM(_VOID,gl_pixel_storei,_I32 _I32);
DEFINE_PRIM(_BYTES,gl_get_string,_I32);
DEFINE_PRIM(_VOID,gl_polygon_mode,_I32 _I32);
DEFINE_PRIM(_VOID,gl_polygon_offset,_F32 _F32);
DEFINE_PRIM(_VOID,gl_enable,_I32);
DEFINE_PRIM(_VOID,gl_disable,_I32);
DEFINE_PRIM(_VOID,gl_cull_face,_I32);
DEFINE_PRIM(_VOID,gl_blend_func,_I32 _I32);
DEFINE_PRIM(_VOID,gl_blend_func_separate,_I32 _I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_blend_equation,_I32);
DEFINE_PRIM(_VOID,gl_blend_equation_separate,_I32 _I32);
DEFINE_PRIM(_VOID,gl_depth_mask,_BOOL);
DEFINE_PRIM(_VOID,gl_depth_func,_I32);
DEFINE_PRIM(_VOID,gl_color_mask,_BOOL _BOOL _BOOL _BOOL);
DEFINE_PRIM(_VOID,gl_color_maski,_I32 _BOOL _BOOL _BOOL _BOOL);
DEFINE_PRIM(_VOID,gl_stencil_mask_separate,_I32 _I32);
DEFINE_PRIM(_VOID,gl_stencil_func_separate,_I32 _I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_stencil_op_separate,_I32  _I32 _I32 _I32);
DEFINE_PRIM(_NULL(_I32),gl_create_program,_NO_ARG);
DEFINE_PRIM(_VOID,gl_delete_program,_NULL(_I32));
DEFINE_PRIM(_VOID,gl_bind_frag_data_location,_NULL(_I32) _I32 _STRING);
DEFINE_PRIM(_VOID,gl_attach_shader,_NULL(_I32) _NULL(_I32));
DEFINE_PRIM(_VOID,gl_link_program,_NULL(_I32));
DEFINE_PRIM(_DYN,gl_get_program_parameter,_NULL(_I32) _I32);
DEFINE_PRIM(_BYTES,gl_get_program_info_bytes,_NULL(_I32));
DEFINE_PRIM(_NULL(_I32),gl_get_uniform_location,_NULL(_I32) _STRING);
DEFINE_PRIM(_I32,gl_get_attrib_location,_NULL(_I32) _STRING);
DEFINE_PRIM(_VOID,gl_use_program,_NULL(_I32));
DEFINE_PRIM(_NULL(_I32),gl_create_shader,_I32);
DEFINE_PRIM(_VOID,gl_shader_source,_NULL(_I32) _STRING);
DEFINE_PRIM(_VOID,gl_compile_shader,_NULL(_I32));
DEFINE_PRIM(_BYTES,gl_get_shader_info_bytes,_NULL(_I32));
DEFINE_PRIM(_DYN,gl_get_shader_parameter,_NULL(_I32) _I32);
DEFINE_PRIM(_VOID,gl_delete_shader,_NULL(_I32));
DEFINE_PRIM(_NULL(_I32),gl_create_texture,_NO_ARG);
DEFINE_PRIM(_VOID,gl_active_texture,_I32);
DEFINE_PRIM(_VOID,gl_bind_texture,_I32 _NULL(_I32));
DEFINE_PRIM(_VOID,gl_tex_parameteri,_I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_tex_parameterf,_I32 _I32 _F32);
DEFINE_PRIM(_VOID,gl_tex_image2d,_I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _BYTES);
DEFINE_PRIM(_VOID,gl_tex_image3d,_I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _BYTES);
DEFINE_PRIM(_VOID,gl_tex_storage2d,_I32 _I32 _I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_tex_storage3d,_I32 _I32 _I32 _I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_tex_image2d_multisample,_I32 _I32 _I32 _I32 _I32 _BOOL);
DEFINE_PRIM(_VOID,gl_compressed_tex_image2d,_I32 _I32 _I32 _I32 _I32 _I32 _I32 _BYTES);
DEFINE_PRIM(_VOID,gl_compressed_tex_image3d,_I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _BYTES);
DEFINE_PRIM(_VOID,gl_tex_sub_image2d, _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _BYTES);
DEFINE_PRIM(_VOID,gl_tex_sub_image3d, _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _BYTES);
DEFINE_PRIM(_VOID,gl_compressed_tex_sub_image2d, _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _BYTES);
DEFINE_PRIM(_VOID,gl_compressed_tex_sub_image3d, _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _BYTES);
DEFINE_PRIM(_VOID,gl_generate_mipmap,_I32);
DEFINE_PRIM(_VOID,gl_delete_texture,_NULL(_I32));
DEFINE_PRIM(_VOID,gl_blit_framebuffer,_I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32 _I32);
DEFINE_PRIM(_NULL(_I32),gl_create_framebuffer,_NO_ARG);
DEFINE_PRIM(_VOID,gl_bind_framebuffer,_I32 _NULL(_I32));
DEFINE_PRIM(_VOID,gl_framebuffer_texture,_I32 _I32 _NULL(_I32) _I32);
DEFINE_PRIM(_VOID,gl_framebuffer_texture2d,_I32 _I32 _I32 _NULL(_I32) _I32);
DEFINE_PRIM(_VOID,gl_framebuffer_texture_layer,_I32 _I32 _NULL(_I32) _I32 _I32);
DEFINE_PRIM(_VOID,gl_delete_framebuffer,_NULL(_I32));
DEFINE_PRIM(_VOID,gl_read_pixels,_I32 _I32 _I32 _I32 _I32 _I32 _BYTES);
DEFINE_PRIM(_VOID,gl_read_buffer,_I32);
DEFINE_PRIM(_VOID,gl_draw_buffers,_I32 _BYTES);
DEFINE_PRIM(_NULL(_I32),gl_create_renderbuffer,_NO_ARG);
DEFINE_PRIM(_VOID,gl_bind_renderbuffer,_I32 _NULL(_I32));
DEFINE_PRIM(_VOID,gl_renderbuffer_storage,_I32 _I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_renderbuffer_storage_multisample,_I32 _I32 _I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_framebuffer_renderbuffer,_I32 _I32 _I32 _NULL(_I32));
DEFINE_PRIM(_VOID,gl_delete_renderbuffer,_NULL(_I32));
DEFINE_PRIM(_NULL(_I32),gl_create_buffer,_NO_ARG);
DEFINE_PRIM(_VOID,gl_bind_buffer,_I32 _NULL(_I32));
DEFINE_PRIM(_VOID,gl_bind_buffer_base,_I32 _I32 _NULL(_I32));
DEFINE_PRIM(_VOID,gl_buffer_data_size,_I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_buffer_data,_I32 _I32 _BYTES _I32);
DEFINE_PRIM(_VOID,gl_buffer_sub_data,_I32 _I32 _BYTES _I32 _I32);
DEFINE_PRIM(_VOID,gl_get_buffer_sub_data,_I32 _I32 _BYTES _I32 _I32);
DEFINE_PRIM(_VOID,gl_enable_vertex_attrib_array,_I32);
DEFINE_PRIM(_VOID,gl_disable_vertex_attrib_array,_I32);
DEFINE_PRIM(_VOID,gl_vertex_attrib_pointer,_I32 _I32 _I32 _BOOL _I32 _I32);
DEFINE_PRIM(_VOID,gl_vertex_attrib_ipointer,_I32 _I32 _I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_delete_buffer,_NULL(_I32));
DEFINE_PRIM(_VOID,gl_uniform1i,_NULL(_I32) _I32);
DEFINE_PRIM(_VOID,gl_uniform4fv,_NULL(_I32) _BYTES _I32 _I32);
DEFINE_PRIM(_VOID,gl_uniform_matrix4fv,_NULL(_I32) _BOOL _BYTES _I32 _I32);
DEFINE_PRIM(_VOID,gl_bind_image_texture,_I32 _I32 _I32 _BOOL _I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_dispatch_compute,_I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_memory_barrier,_I32);
DEFINE_PRIM(_VOID,gl_draw_elements,_I32 _I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_draw_elements_instanced,_I32 _I32 _I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_draw_arrays,_I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_draw_arrays_instanced,_I32 _I32 _I32 _I32);
DEFINE_PRIM(_VOID,gl_multi_draw_elements_indirect, _I32 _I32 _BYTES _I32 _I32);
DEFINE_PRIM(_VOID,gl_multi_draw_elements_indirect_count, _I32 _I32 _BYTES _BYTES _I32 _I32);
DEFINE_PRIM(_NULL(_I32),gl_create_vertex_array,_NO_ARG);
DEFINE_PRIM(_VOID,gl_bind_vertex_array,_NULL(_I32));
DEFINE_PRIM(_VOID,gl_delete_vertex_array,_NULL(_I32));
DEFINE_PRIM(_VOID,gl_vertex_attrib_divisor,_I32 _I32);

DEFINE_PRIM(_NULL(_I32), gl_create_query, _NO_ARG);
DEFINE_PRIM(_VOID, gl_delete_query, _NULL(_I32));
DEFINE_PRIM(_VOID, gl_begin_query, _I32 _NULL(_I32));
DEFINE_PRIM(_VOID, gl_end_query, _I32);
DEFINE_PRIM(_BOOL, gl_query_result_available, _NULL(_I32));
DEFINE_PRIM(_VOID, gl_query_counter, _NULL(_I32) _I32);
DEFINE_PRIM(_F64, gl_query_result, _NULL(_I32));

DEFINE_PRIM(_I32, gl_get_uniform_block_index, _NULL(_I32) _STRING);
DEFINE_PRIM(_VOID, gl_uniform_block_binding, _NULL(_I32) _I32 _I32);
DEFINE_PRIM(_I32, gl_get_program_resource_index, _NULL(_I32) _I32 _STRING);
DEFINE_PRIM(_VOID, gl_shader_storage_block_binding, _NULL(_I32) _I32 _I32);

DEFINE_PRIM(_I32, gl_get_config_parameter, _I32);
DEFINE_PRIM(_BOOL, gl_has_extension, _STRING);
