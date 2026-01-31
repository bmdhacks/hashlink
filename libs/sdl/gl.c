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

// Threaded swap support - ensure GL context is current before any GL call
// This function is defined in sdl.c
extern void sdl_gl_ensure_context(void);
#define GL_ENSURE_CONTEXT() sdl_gl_ensure_context()

static int GLLoadAPI() {
#	include "GLImports.h"
	return 0;
}

// KHR_debug constant portability for GLES 3.1
#if defined(HL_GLES31) && !defined(GL_DEBUG_OUTPUT)
#define GL_DEBUG_OUTPUT                GL_DEBUG_OUTPUT_KHR
#define GL_DEBUG_OUTPUT_SYNCHRONOUS    GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR
#define GL_DEBUG_SOURCE_APPLICATION    GL_DEBUG_SOURCE_APPLICATION_KHR
#define GL_DEBUG_TYPE_MARKER           GL_DEBUG_TYPE_MARKER_KHR
#define GL_DEBUG_SEVERITY_NOTIFICATION GL_DEBUG_SEVERITY_NOTIFICATION_KHR
#define GL_DEBUG_TYPE_PERFORMANCE      GL_DEBUG_TYPE_PERFORMANCE_KHR
#define GL_DEBUG_TYPE_OTHER            GL_DEBUG_TYPE_OTHER_KHR
#define GL_DEBUG_TYPE_ERROR            GL_DEBUG_TYPE_ERROR_KHR
#endif

// GLES 3.1 KHR_debug function pointers (loaded at runtime)
#if defined(HL_GLES31)
static PFNGLPUSHDEBUGGROUPKHRPROC _glPushDebugGroup = NULL;
static PFNGLPOPDEBUGGROUPKHRPROC _glPopDebugGroup = NULL;
static PFNGLOBJECTLABELKHRPROC _glObjectLabel = NULL;
static PFNGLDEBUGMESSAGEINSERTKHRPROC _glDebugMessageInsert = NULL;
static PFNGLDEBUGMESSAGECONTROLKHRPROC _glDebugMessageControl = NULL;
static PFNGLDEBUGMESSAGECALLBACKKHRPROC _glDebugMessageCallback = NULL;
#endif

static bool gl_debug_enabled = false;

#if defined(HL_GLES)
static void GL_APIENTRY debug_message_callback( GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam ) {
#else
static void APIENTRY debug_message_callback( GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam ) {
#endif
	fprintf(stderr, "GL %s: type = 0x%x, severity = 0x%x, message = %s\n",
		( type == GL_DEBUG_TYPE_ERROR ? "** ERROR **" : "DEBUG" ),
		type, severity, message);
}

#define ZIDX(val) ((val)?(val)->v.i:0)

// globals
HL_PRIM bool HL_NAME(gl_init)() {
	return GLLoadAPI() == 0;
}

HL_PRIM bool HL_NAME(gl_set_debug)( bool enable ) {
	if( enable ) {
#if defined(HL_GLES31)
		// GLES 3.1: load KHR_debug functions at runtime
		_glPushDebugGroup = (PFNGLPUSHDEBUGGROUPKHRPROC)SDL_GL_GetProcAddress("glPushDebugGroupKHR");
		_glPopDebugGroup = (PFNGLPOPDEBUGGROUPKHRPROC)SDL_GL_GetProcAddress("glPopDebugGroupKHR");
		_glObjectLabel = (PFNGLOBJECTLABELKHRPROC)SDL_GL_GetProcAddress("glObjectLabelKHR");
		_glDebugMessageInsert = (PFNGLDEBUGMESSAGEINSERTKHRPROC)SDL_GL_GetProcAddress("glDebugMessageInsertKHR");
		_glDebugMessageControl = (PFNGLDEBUGMESSAGECONTROLKHRPROC)SDL_GL_GetProcAddress("glDebugMessageControlKHR");
		_glDebugMessageCallback = (PFNGLDEBUGMESSAGECALLBACKKHRPROC)SDL_GL_GetProcAddress("glDebugMessageCallbackKHR");
		if( !_glDebugMessageCallback ) {
			gl_debug_enabled = false;
			return false;
		}
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		_glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_PERFORMANCE, GL_DONT_CARE, 0, NULL, GL_FALSE);
		_glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_OTHER, GL_DONT_CARE, 0, NULL, GL_FALSE);
		_glDebugMessageCallback(debug_message_callback, 0);
#elif defined(GL_VERSION_4_3) || defined(HL_ANDROID)
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_PERFORMANCE, GL_DONT_CARE, 0, NULL, GL_FALSE);
		glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_OTHER, GL_DONT_CARE, 0, NULL, GL_FALSE);
		glDebugMessageCallback(debug_message_callback, 0);
#else
		return false;
#endif
		gl_debug_enabled = true;
	} else {
#if defined(HL_GLES31) || defined(GL_VERSION_4_3) || defined(HL_ANDROID)
		glDisable(GL_DEBUG_OUTPUT);
#endif
		gl_debug_enabled = false;
	}
	return true;
}

HL_PRIM void HL_NAME(gl_push_debug_group)( vstring *label ) {
	if( !gl_debug_enabled ) return;
	const char *utf8 = hl_to_utf8(label->bytes);
#if defined(HL_GLES31)
	if( _glPushDebugGroup )
		_glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, utf8);
#elif defined(GL_VERSION_4_3) || defined(HL_ANDROID)
	glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, utf8);
#endif
}

HL_PRIM void HL_NAME(gl_pop_debug_group)() {
	if( !gl_debug_enabled ) return;
#if defined(HL_GLES31)
	if( _glPopDebugGroup )
		_glPopDebugGroup();
#elif defined(GL_VERSION_4_3) || defined(HL_ANDROID)
	glPopDebugGroup();
#endif
}

HL_PRIM void HL_NAME(gl_debug_message_insert)( vstring *message ) {
	if( !gl_debug_enabled ) return;
	const char *utf8 = hl_to_utf8(message->bytes);
#if defined(HL_GLES31)
	if( _glDebugMessageInsert )
		_glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, 0, GL_DEBUG_SEVERITY_NOTIFICATION, -1, utf8);
#elif defined(GL_VERSION_4_3) || defined(HL_ANDROID)
	glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, 0, GL_DEBUG_SEVERITY_NOTIFICATION, -1, utf8);
#endif
}

HL_PRIM void HL_NAME(gl_object_label)( int identifier, vdynamic *obj, vstring *label ) {
	if( !gl_debug_enabled ) return;
	const char *utf8 = hl_to_utf8(label->bytes);
#if defined(HL_GLES31)
	if( _glObjectLabel )
		_glObjectLabel(identifier, ZIDX(obj), -1, utf8);
#elif defined(GL_VERSION_4_3) || defined(HL_ANDROID)
	glObjectLabel(identifier, ZIDX(obj), -1, utf8);
#endif
}

HL_PRIM bool HL_NAME(gl_is_context_lost)() {
	// seems like a GL context is rarely lost on desktop
	// let's look at it again on mobile
	return false;
}

HL_PRIM void HL_NAME(gl_clear)( int bits ) {
	GL_ENSURE_CONTEXT();
	glClear(bits);
}

HL_PRIM int HL_NAME(gl_get_error)() {
	GL_ENSURE_CONTEXT();
	return glGetError();
}

HL_PRIM void HL_NAME(gl_scissor)( int x, int y, int width, int height ) {
	GL_ENSURE_CONTEXT();
	glScissor(x, y, width, height);
}

HL_PRIM void HL_NAME(gl_clear_color)( double r, double g, double b, double a ) {
	GL_ENSURE_CONTEXT();
	glClearColor((float)r, (float)g, (float)b, (float)a);
}

HL_PRIM void HL_NAME(gl_clear_depth)( double value ) {
	GL_ENSURE_CONTEXT();
	glClearDepth(value);
}

HL_PRIM void HL_NAME(gl_clear_stencil)( int value ) {
	GL_ENSURE_CONTEXT();
	glClearStencil(value);
}

HL_PRIM void HL_NAME(gl_viewport)( int x, int y, int width, int height ) {
	GL_ENSURE_CONTEXT();
	glViewport(x, y, width, height);
}

HL_PRIM void HL_NAME(gl_flush)() {
	GL_ENSURE_CONTEXT();
	glFlush();
}

HL_PRIM void HL_NAME(gl_finish)() {
	GL_ENSURE_CONTEXT();
	glFinish();
}

HL_PRIM void HL_NAME(gl_pixel_storei)( int key, int value ) {
	GL_ENSURE_CONTEXT();
	glPixelStorei(key, value);
}

HL_PRIM vbyte *HL_NAME(gl_get_string)(int name) {
	GL_ENSURE_CONTEXT();
	return (vbyte*)glGetString(name);
}

// state changes

HL_PRIM void HL_NAME(gl_polygon_mode)(int face, int mode) {
	GL_ENSURE_CONTEXT();
	glPolygonMode(face, mode);
}

HL_PRIM void HL_NAME(gl_polygon_offset)(float factor, float units) {
	GL_ENSURE_CONTEXT();
	glPolygonOffset(factor, units);
}

HL_PRIM void HL_NAME(gl_enable)( int feature ) {
	GL_ENSURE_CONTEXT();
	glEnable(feature);
}

HL_PRIM void HL_NAME(gl_disable)( int feature ) {
	GL_ENSURE_CONTEXT();
	glDisable(feature);
}

HL_PRIM void HL_NAME(gl_cull_face)( int face ) {
	GL_ENSURE_CONTEXT();
	glCullFace(face);
}

HL_PRIM void HL_NAME(gl_blend_func)( int src, int dst ) {
	GL_ENSURE_CONTEXT();
	glBlendFunc(src, dst);
}

HL_PRIM void HL_NAME(gl_blend_func_separate)( int src, int dst, int alphaSrc, int alphaDst ) {
	GL_ENSURE_CONTEXT();
	glBlendFuncSeparate(src, dst, alphaSrc, alphaDst);
}

HL_PRIM void HL_NAME(gl_blend_equation)( int op ) {
	GL_ENSURE_CONTEXT();
	glBlendEquation(op);
}

HL_PRIM void HL_NAME(gl_blend_equation_separate)( int op, int alphaOp ) {
	GL_ENSURE_CONTEXT();
	glBlendEquationSeparate(op, alphaOp);
}

HL_PRIM void HL_NAME(gl_depth_mask)( bool mask ) {
	GL_ENSURE_CONTEXT();
	glDepthMask(mask);
}

HL_PRIM void HL_NAME(gl_depth_func)( int f ) {
	GL_ENSURE_CONTEXT();
	glDepthFunc(f);
}

HL_PRIM void HL_NAME(gl_color_mask)( bool r, bool g, bool b, bool a ) {
	GL_ENSURE_CONTEXT();
	glColorMask(r, g, b, a);
}

HL_PRIM void HL_NAME(gl_color_maski)( int i, bool r, bool g, bool b, bool a ) {
	GL_ENSURE_CONTEXT();
	glColorMaski(i, r, g, b, a);
}

HL_PRIM void HL_NAME(gl_stencil_mask_separate)(int face, int mask) {
	GL_ENSURE_CONTEXT();
	glStencilMaskSeparate(face, mask);
}

HL_PRIM void HL_NAME(gl_stencil_func_separate)(int face, int func, int ref, int mask ) {
	GL_ENSURE_CONTEXT();
	glStencilFuncSeparate(face, func, ref, mask);
}

HL_PRIM void HL_NAME(gl_stencil_op_separate)(int face, int sfail, int dpfail, int dppass) {
	GL_ENSURE_CONTEXT();
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
	GL_ENSURE_CONTEXT();
	int v = glCreateProgram();
	if( v == 0 ) return NULL;
	return alloc_i32(v);
}

HL_PRIM void HL_NAME(gl_delete_program)( vdynamic *s ) {
	GL_ENSURE_CONTEXT();
	glDeleteProgram(s->v.i);
}

HL_PRIM void HL_NAME(gl_bind_frag_data_location)( vdynamic *p, int colNum, vstring *name ) {
	GL_ENSURE_CONTEXT();
	char *cname = hl_to_utf8(name->bytes);
	glBindFragDataLocation(p->v.i, colNum, cname);
}

HL_PRIM void HL_NAME(gl_attach_shader)( vdynamic *p, vdynamic *s ) {
	GL_ENSURE_CONTEXT();
	glAttachShader(p->v.i, s->v.i);
}

HL_PRIM void HL_NAME(gl_link_program)( vdynamic *p ) {
	GL_ENSURE_CONTEXT();
	glLinkProgram(p->v.i);
	int status = 0;
	glGetProgramiv(p->v.i, 0x8B82/*GL_LINK_STATUS*/, &status);
	if (!status) {
		char log[1024];
		glGetProgramInfoLog(p->v.i, sizeof(log), NULL, log);
		fprintf(stderr, "[GL ERROR] Program %d link failed: %s\n", p->v.i, log);
	}
}

HL_PRIM vdynamic *HL_NAME(gl_get_program_parameter)( vdynamic *p, int param ) {
	GL_ENSURE_CONTEXT();
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
	GL_ENSURE_CONTEXT();
	char log[4096];
	*log = 0;
	glGetProgramInfoLog(p->v.i, 4096, NULL, log);
	return hl_copy_bytes((vbyte*)log,(int)strlen(log) + 1);
}

HL_PRIM vdynamic *HL_NAME(gl_get_uniform_location)( vdynamic *p, vstring *name ) {
	GL_ENSURE_CONTEXT();
	char *cname = hl_to_utf8(name->bytes);
	int u = glGetUniformLocation(p->v.i, cname);
	if( u < 0 ) return NULL;
	return alloc_i32(u);
}

HL_PRIM int HL_NAME(gl_get_attrib_location)( vdynamic *p, vstring *name ) {
	GL_ENSURE_CONTEXT();
	char *cname = hl_to_utf8(name->bytes);
	return glGetAttribLocation(p->v.i, cname);
}

HL_PRIM void HL_NAME(gl_use_program)( vdynamic *p ) {
	GL_ENSURE_CONTEXT();
	glUseProgram(ZIDX(p));
}

// shader

HL_PRIM vdynamic *HL_NAME(gl_create_shader)( int type ) {
	GL_ENSURE_CONTEXT();
	int s = glCreateShader(type);
	if (s == 0) return NULL;
	return alloc_i32(s);
}

HL_PRIM void HL_NAME(gl_shader_source)( vdynamic *s, vstring *src ) {
	GL_ENSURE_CONTEXT();
	const GLchar *c = (GLchar*)hl_to_utf8(src->bytes);
	glShaderSource(s->v.i, 1, &c, NULL);
}

HL_PRIM void HL_NAME(gl_compile_shader)( vdynamic *s ) {
	GL_ENSURE_CONTEXT();
	glCompileShader(s->v.i);
	int status = 0;
	glGetShaderiv(s->v.i, 0x8B81/*GL_COMPILE_STATUS*/, &status);
	if (!status) {
		char log[1024];
		glGetShaderInfoLog(s->v.i, sizeof(log), NULL, log);
		fprintf(stderr, "[GL ERROR] Shader %d compile failed: %s\n", s->v.i, log);
	}
}

HL_PRIM vbyte *HL_NAME(gl_get_shader_info_bytes)( vdynamic *s ) {
	GL_ENSURE_CONTEXT();
	char log[4096];
	*log = 0;
	glGetShaderInfoLog(s->v.i, 4096, NULL, log);
	return hl_copy_bytes((vbyte*)log, (int)strlen(log)+1);
}

HL_PRIM vdynamic *HL_NAME(gl_get_shader_parameter)( vdynamic *s, int param ) {
	GL_ENSURE_CONTEXT();
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
	GL_ENSURE_CONTEXT();
	glDeleteShader(s->v.i);
}

// texture

HL_PRIM vdynamic *HL_NAME(gl_create_texture)() {
	GL_ENSURE_CONTEXT();
	unsigned int t = 0;
	glGenTextures(1, &t);
	return alloc_i32(t);
}

HL_PRIM void HL_NAME(gl_active_texture)( int t ) {
	GL_ENSURE_CONTEXT();
	glActiveTexture(t);
}

HL_PRIM void HL_NAME(gl_bind_texture)( int t, vdynamic *texture ) {
	GL_ENSURE_CONTEXT();
	glBindTexture(t, ZIDX(texture));
}

HL_PRIM void HL_NAME(gl_bind_image_texture)( int unit, int texture, int level, bool layered, int layer, int access, int format ) {
	GL_ENSURE_CONTEXT();
	glBindImageTexture(unit, texture, level, layered, layer, access, format);
}

HL_PRIM void HL_NAME(gl_tex_parameterf)( int t, int key, float value ) {
	GL_ENSURE_CONTEXT();
	glTexParameterf(t, key, value);
}

HL_PRIM void HL_NAME(gl_tex_parameteri)( int t, int key, int value ) {
	GL_ENSURE_CONTEXT();
	glTexParameteri(t, key, value);
}

HL_PRIM void HL_NAME(gl_tex_image2d)( int target, int level, int internalFormat, int width, int height, int border, int format, int type, vbyte *image ) {
	GL_ENSURE_CONTEXT();
	glTexImage2D(target, level, internalFormat, width, height, border, format, type, image);
}

HL_PRIM void HL_NAME(gl_tex_image3d)( int target, int level, int internalFormat, int width, int height, int depth, int border, int format, int type, vbyte *image ) {
	GL_ENSURE_CONTEXT();
	glTexImage3D(target, level, internalFormat, width, height, depth, border, format, type, image);
}

HL_PRIM void HL_NAME(gl_tex_storage2d)( int target, int levels, int internalFormat, int width, int height) {
	GL_ENSURE_CONTEXT();
#ifndef __APPLE__
	glTexStorage2D(target, levels, internalFormat, width, height);
#else
    hl_error("glTexStorage2d is not supported on Apple platforms");
#endif
}

HL_PRIM void HL_NAME(gl_tex_storage3d)( int target, int levels, int internalFormat, int width, int height, int depth) {
	GL_ENSURE_CONTEXT();
#ifndef __APPLE__
	glTexStorage3D(target, levels, internalFormat, width, height, depth);
#else
	hl_error("glTexStorage3d is not supported on Apple platforms");
#endif
}

HL_PRIM void HL_NAME(gl_tex_image2d_multisample)( int target, int samples, int internalFormat, int width, int height, bool fixedsamplelocations) {
	GL_ENSURE_CONTEXT();
	glTexImage2DMultisample(target, samples, internalFormat, width, height, fixedsamplelocations);
}

HL_PRIM void HL_NAME(gl_compressed_tex_image2d)( int target, int level, int internalFormat, int width, int height, int border, int imageSize, vbyte *image ) {
	GL_ENSURE_CONTEXT();
	glCompressedTexImage2D(target,level,internalFormat,width,height,border,imageSize,image);
}

HL_PRIM void HL_NAME(gl_compressed_tex_image3d)( int target, int level, int internalFormat, int width, int height, int depth, int border, int imageSize, vbyte *image ) {
	GL_ENSURE_CONTEXT();
	glCompressedTexImage3D(target,level,internalFormat,width,height,depth,border,imageSize,image);
}

HL_PRIM void HL_NAME(gl_tex_sub_image2d)(int target, int level, int xoffset, int yoffset, int width, int height, int format, int type, vbyte *image) {
	GL_ENSURE_CONTEXT();
	glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, image);
}

HL_PRIM void HL_NAME(gl_tex_sub_image3d)(int target, int level, int xoffset, int yoffset, int zoffset, int width, int height, int depth, int format, int type, vbyte *image) {
	GL_ENSURE_CONTEXT();
	glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, image);
}

HL_PRIM void HL_NAME(gl_compressed_tex_sub_image2d)(int target, int level, int xoffset, int yoffset, int width, int height, int format, int type, vbyte *image) {
	GL_ENSURE_CONTEXT();
	glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, image);
}

HL_PRIM void HL_NAME(gl_compressed_tex_sub_image3d)(int target, int level, int xoffset, int yoffset, int zoffset, int width, int height, int depth, int format, int type, vbyte *image) {
	GL_ENSURE_CONTEXT();
	glCompressedTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, image);
}

HL_PRIM void HL_NAME(gl_generate_mipmap)( int t ) {
	GL_ENSURE_CONTEXT();
	glGenerateMipmap(t);
}

HL_PRIM void HL_NAME(gl_delete_texture)( vdynamic *t ) {
	GL_ENSURE_CONTEXT();
	unsigned int tt = t->v.i;
	glDeleteTextures(1, &tt);
}

// framebuffer

HL_PRIM void HL_NAME(gl_blit_framebuffer)(int src_x0, int src_y0, int src_x1, int src_y1, int dst_x0, int dst_y0, int dst_x1, int dst_y1, int mask, int filter) {
	GL_ENSURE_CONTEXT();
	glBlitFramebuffer(src_x0, src_y0, src_x1, src_y1, dst_x0, dst_y0, dst_x1, dst_y1, mask, filter);
}

HL_PRIM vdynamic *HL_NAME(gl_create_framebuffer)() {
	GL_ENSURE_CONTEXT();
	unsigned int f = 0;
	glGenFramebuffers(1, &f);
	return alloc_i32(f);
}

HL_PRIM void HL_NAME(gl_bind_framebuffer)( int target, vdynamic *f ) {
	GL_ENSURE_CONTEXT();
	unsigned int id = ZIDX(f);
#if	defined(HL_IOS) || defined(HL_TVOS)
	if ( id==0 ) {
		SDL_SysWMinfo info;
		SDL_VERSION(&info.version);
		SDL_GetWindowWMInfo(SDL_GL_GetCurrentWindow(), &info);
		id = info.info.uikit.framebuffer;
	}
#endif
	glBindFramebuffer(target, id);
}

HL_PRIM void HL_NAME(gl_framebuffer_texture)( int target, int attach, vdynamic *t, int level ) {
	GL_ENSURE_CONTEXT();
#if defined(HL_GLES31)
	glFramebufferTexture2D(target, attach, GL_TEXTURE_2D, ZIDX(t), level);
#else
	glFramebufferTexture(target, attach, ZIDX(t), level);
#endif
}

HL_PRIM void HL_NAME(gl_framebuffer_texture2d)( int target, int attach, int texTarget, vdynamic *t, int level ) {
	GL_ENSURE_CONTEXT();
	glFramebufferTexture2D(target, attach, texTarget, ZIDX(t), level);
}

HL_PRIM void HL_NAME(gl_framebuffer_texture_layer)( int target, int attach, vdynamic *t, int level, int layer ) {
	GL_ENSURE_CONTEXT();
	glFramebufferTextureLayer(target, attach, ZIDX(t), level, layer);
}

HL_PRIM void HL_NAME(gl_delete_framebuffer)( vdynamic *f ) {
	GL_ENSURE_CONTEXT();
	unsigned int ff = (unsigned)f->v.i;
	glDeleteFramebuffers(1, &ff);
}

HL_PRIM void HL_NAME(gl_read_pixels)( int x, int y, int width, int height, int format, int type, vbyte *data ) {
	GL_ENSURE_CONTEXT();
	glReadPixels(x, y, width, height, format, type, data);
}

HL_PRIM void HL_NAME(gl_read_buffer)( int mode ) {
	GL_ENSURE_CONTEXT();
	glReadBuffer(mode);
}

HL_PRIM void HL_NAME(gl_draw_buffers)( int count, unsigned int *buffers) {
	GL_ENSURE_CONTEXT();
	glDrawBuffers(count, buffers);
}

// renderbuffer

HL_PRIM vdynamic *HL_NAME(gl_create_renderbuffer)() {
	GL_ENSURE_CONTEXT();
	unsigned int buf = 0;
	glGenRenderbuffers(1, &buf);
	return alloc_i32(buf);
}

HL_PRIM void HL_NAME(gl_bind_renderbuffer)( int target, vdynamic *r ) {
	GL_ENSURE_CONTEXT();
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
	GL_ENSURE_CONTEXT();
#if defined(HL_GLES)
	// GLES requires sized internal formats - translate unsized ones
	if (format == GL_DEPTH_STENCIL) {
		format = GL_DEPTH24_STENCIL8;
	} else if (format == GL_DEPTH_COMPONENT) {
		format = GL_DEPTH_COMPONENT24;
	}
#endif
	glRenderbufferStorage(target, format, width, height);
}

HL_PRIM void HL_NAME(gl_renderbuffer_storage_multisample)( int target, int samples, int format, int width, int height ) {
	GL_ENSURE_CONTEXT();
#if defined(HL_GLES)
	// GLES requires sized internal formats - translate unsized ones
	if (format == GL_DEPTH_STENCIL) {
		format = GL_DEPTH24_STENCIL8;
	} else if (format == GL_DEPTH_COMPONENT) {
		format = GL_DEPTH_COMPONENT24;
	}
#endif
	glRenderbufferStorageMultisample(target, samples, format, width, height);
}

HL_PRIM void HL_NAME(gl_framebuffer_renderbuffer)( int frameTarget, int attach, int renderTarget, vdynamic *b ) {
	GL_ENSURE_CONTEXT();
	glFramebufferRenderbuffer(frameTarget, attach, renderTarget, ZIDX(b));
}

HL_PRIM void HL_NAME(gl_delete_renderbuffer)( vdynamic *b ) {
	GL_ENSURE_CONTEXT();
	unsigned int bb = (unsigned)b->v.i;
	glDeleteRenderbuffers(1, &bb);
}

// buffer

HL_PRIM vdynamic *HL_NAME(gl_create_buffer)() {
	GL_ENSURE_CONTEXT();
	unsigned int b = 0;
	glGenBuffers(1, &b);
	return alloc_i32(b);
}

HL_PRIM void HL_NAME(gl_bind_buffer)( int target, vdynamic *b ) {
	GL_ENSURE_CONTEXT();
	glBindBuffer(target, ZIDX(b));
}

HL_PRIM void HL_NAME(gl_bind_buffer_base)( int target, int index, vdynamic *b ) {
	GL_ENSURE_CONTEXT();
	glBindBufferBase(target, index, ZIDX(b));
}

HL_PRIM void HL_NAME(gl_buffer_data_size)( int target, int size, int param ) {
	GL_ENSURE_CONTEXT();
	glBufferData(target, size, NULL, param);
}

HL_PRIM void HL_NAME(gl_buffer_data)( int target, int size, vbyte *data, int param ) {
	GL_ENSURE_CONTEXT();
	glBufferData(target, size, data, param);
}

HL_PRIM void HL_NAME(gl_buffer_sub_data)( int target, int offset, vbyte *data, int srcOffset, int srcLength ) {
	GL_ENSURE_CONTEXT();
	glBufferSubData(target, offset, srcLength, data + srcOffset);
}

HL_PRIM void HL_NAME(gl_get_buffer_sub_data)( int target, int offset, vbyte *data, int srcOffset, int srcLength ) {
	GL_ENSURE_CONTEXT();
	glGetBufferSubData(target, srcOffset, srcLength, data + offset);
}

HL_PRIM void HL_NAME(gl_enable_vertex_attrib_array)( int attrib ) {
	GL_ENSURE_CONTEXT();
	glEnableVertexAttribArray(attrib);
}

HL_PRIM void HL_NAME(gl_disable_vertex_attrib_array)( int attrib ) {
	GL_ENSURE_CONTEXT();
	glDisableVertexAttribArray(attrib);
}

HL_PRIM void HL_NAME(gl_vertex_attrib_pointer)( int index, int size, int type, bool normalized, int stride, int position ) {
	GL_ENSURE_CONTEXT();
	glVertexAttribPointer(index, size, type, normalized, stride, (void*)(int_val)position);
}

HL_PRIM void HL_NAME(gl_vertex_attrib_ipointer)( int index, int size, int type, int stride, int position ) {
	GL_ENSURE_CONTEXT();
	glVertexAttribIPointer(index, size, type, stride, (void*)(int_val)position);
}

HL_PRIM void HL_NAME(gl_vertex_attrib_divisor)( int index, int divisor ) {
	GL_ENSURE_CONTEXT();
	glVertexAttribDivisor(index, divisor);
}

HL_PRIM void HL_NAME(gl_delete_buffer)( vdynamic *b ) {
	GL_ENSURE_CONTEXT();
	unsigned int bb = (unsigned)b->v.i;
	glDeleteBuffers(1, &bb);
}

// uniforms

HL_PRIM void HL_NAME(gl_uniform1i)( vdynamic *u, int i ) {
	GL_ENSURE_CONTEXT();
	glUniform1i(u->v.i, i);
}

HL_PRIM void HL_NAME(gl_uniform4fv)( vdynamic *u, vbyte *buffer, int bufPos, int count ) {
	GL_ENSURE_CONTEXT();
	glUniform4fv(u->v.i, count, (float*)buffer + bufPos);
}

HL_PRIM void HL_NAME(gl_uniform_matrix4fv)( vdynamic *u, bool transpose, vbyte *buffer, int bufPos, int count ) {
	GL_ENSURE_CONTEXT();
	glUniformMatrix4fv(u->v.i, count, transpose ? GL_TRUE : GL_FALSE, (float*)buffer + bufPos);
}

// compute
HL_PRIM void HL_NAME(gl_dispatch_compute)( int num_groups_x, int num_groups_y, int num_groups_z ) {
	GL_ENSURE_CONTEXT();
	glDispatchCompute(num_groups_x, num_groups_y, num_groups_z);
}

HL_PRIM void HL_NAME(gl_memory_barrier)( int barriers ) {
	GL_ENSURE_CONTEXT();
	glMemoryBarrier(barriers);
}

// draw

HL_PRIM void HL_NAME(gl_draw_elements)( int mode, int count, int type, int start ) {
	GL_ENSURE_CONTEXT();
	glDrawElements(mode, count, type, (void*)(int_val)start);
}

HL_PRIM void HL_NAME(gl_draw_arrays)( int mode, int first, int count, int start ) {
	GL_ENSURE_CONTEXT();
	glDrawArrays(mode,first,count);
}

HL_PRIM void HL_NAME(gl_draw_elements_instanced)( int mode, int count, int type, int start, int primcount ) {
	GL_ENSURE_CONTEXT();
	glDrawElementsInstanced(mode,count,type,(void*)(int_val)start,primcount);
}

HL_PRIM void HL_NAME(gl_draw_arrays_instanced)( int mode, int first, int count, int primcount ) {
	GL_ENSURE_CONTEXT();
	glDrawArraysInstanced(mode,first,count,primcount);
}

HL_PRIM void HL_NAME(gl_multi_draw_elements_indirect)( int mode, int type, vbyte *data, int count, int stride ) {
	GL_ENSURE_CONTEXT();
#	ifdef GL_VERSION_4_3
	glMultiDrawElementsIndirect(mode, type, data, count, stride);
#	endif
}

HL_PRIM void HL_NAME(gl_multi_draw_elements_indirect_count)(int mode, int type, vbyte* data, vbyte* drawcount, int maxdrawcount, int stride) {
	GL_ENSURE_CONTEXT();
	GL_IMPORT_OPT(glMultiDrawElementsIndirectCountARB, MULTIDRAWELEMENTSINDIRECTCOUNTARB)
	glMultiDrawElementsIndirectCountARB(mode, type, data, (GLintptr)drawcount, maxdrawcount, stride);
}

HL_PRIM int HL_NAME(gl_get_config_parameter)( int feature ) {
	GL_ENSURE_CONTEXT();
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
	GL_ENSURE_CONTEXT();
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
	GL_ENSURE_CONTEXT();
	unsigned int t = 0;
	glGenQueries(1, &t);
	return alloc_i32(t);
}

HL_PRIM void HL_NAME(gl_delete_query)( vdynamic *q ) {
	GL_ENSURE_CONTEXT();
	glDeleteQueries(1, (const GLuint *) &q->v.i);
}

HL_PRIM void HL_NAME(gl_begin_query)( int target, vdynamic *q ) {
	GL_ENSURE_CONTEXT();
	glBeginQuery(target,q->v.i);
}

HL_PRIM void HL_NAME(gl_end_query)( int target ) {
	GL_ENSURE_CONTEXT();
	glEndQuery(target);
}

HL_PRIM bool HL_NAME(gl_query_result_available)( vdynamic *q ) {
	GL_ENSURE_CONTEXT();
	int v = 0;
	glGetQueryObjectiv(q->v.i, GL_QUERY_RESULT_AVAILABLE, &v);
	return v == GL_TRUE;
}

HL_PRIM double HL_NAME(gl_query_result)( vdynamic *q ) {
	GL_ENSURE_CONTEXT();
	GLuint64 v = -1;
#	if !defined(HL_MESA) && !defined(HL_MOBILE) && !defined(HL_GLES31)
	glGetQueryObjectui64v(q->v.i, GL_QUERY_RESULT, &v);
#	endif
	return (double)v;
}

HL_PRIM void HL_NAME(gl_query_counter)( vdynamic *q, int target ) {
	GL_ENSURE_CONTEXT();
#	if !defined(HL_MESA) && !defined(HL_MOBILE) && !defined(HL_GLES31)
	glQueryCounter(q->v.i, target);
#	endif
}

// vertex array

HL_PRIM vdynamic *HL_NAME(gl_create_vertex_array)() {
	GL_ENSURE_CONTEXT();
	unsigned int f = 0;
	glGenVertexArrays(1, &f);
	return alloc_i32(f);
}

HL_PRIM void HL_NAME(gl_bind_vertex_array)( vdynamic *b ) {
	GL_ENSURE_CONTEXT();
	unsigned int bb = (unsigned)b->v.i;
	glBindVertexArray(bb);
}

HL_PRIM void HL_NAME(gl_delete_vertex_array)( vdynamic *b ) {
	GL_ENSURE_CONTEXT();
	unsigned int bb = (unsigned)b->v.i;
	glDeleteVertexArrays(1, &bb);
}

// uniform buffer

HL_PRIM int HL_NAME(gl_get_uniform_block_index)( vdynamic *p, vstring *name ) {
	GL_ENSURE_CONTEXT();
	char *cname = hl_to_utf8(name->bytes);
	return (int)glGetUniformBlockIndex(p->v.i, cname);
}

HL_PRIM void HL_NAME(gl_uniform_block_binding)( vdynamic *p, int index, int binding ) {
	GL_ENSURE_CONTEXT();
	glUniformBlockBinding(p->v.i, index, binding);
}

// SSBOs

HL_PRIM int HL_NAME(gl_get_program_resource_index)( vdynamic *p, int type, vstring *name ) {
	GL_ENSURE_CONTEXT();
#ifndef __APPLE__
	char *cname = hl_to_utf8(name->bytes);
	return (int)glGetProgramResourceIndex(p->v.i, type, cname);
#else
	hl_error("glGetProgramResourceIndex is not supported on Apple platforms");
#endif
}

HL_PRIM void HL_NAME(gl_shader_storage_block_binding)( vdynamic *p, int index, int binding ) {
	GL_ENSURE_CONTEXT();
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

DEFINE_PRIM(_VOID, gl_push_debug_group, _STRING);
DEFINE_PRIM(_VOID, gl_pop_debug_group, _NO_ARG);
DEFINE_PRIM(_VOID, gl_debug_message_insert, _STRING);
DEFINE_PRIM(_VOID, gl_object_label, _I32 _NULL(_I32) _STRING);
DEFINE_PRIM(_I32, gl_get_config_parameter, _I32);
DEFINE_PRIM(_BOOL, gl_has_extension, _STRING);
