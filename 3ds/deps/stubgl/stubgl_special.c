/*
 * Stub OpenGL for the 3DS port. There is no OpenGL on the PICA200: OpenSceneGraph is used for its
 * scene graph, update and cull traversals only, and drawing is done by the port's citro3d backend.
 * OSG still references gl* functions, so every one of them exists here as a no-op; the generator in
 * deps/build-deps.sh adds a stub for each undefined gl* symbol found in the OSG libraries.
 *
 * This file holds the hand-written ones whose results OSG actually reads.
 */
#include <GL/gl.h>
#include <string.h>

const GLubyte* glGetString(GLenum name)
{
    switch (name)
    {
        case GL_VENDOR:
            return (const GLubyte*)"OpenMW 3DS port";
        case GL_RENDERER:
            return (const GLubyte*)"stub GL (citro3d backend)";
        case GL_VERSION:
            return (const GLubyte*)"2.1 stub";
        case 0x8B8C: /* GL_SHADING_LANGUAGE_VERSION */
            return (const GLubyte*)"1.20";
        case GL_EXTENSIONS:
            return (const GLubyte*)"";
    }
    return (const GLubyte*)"";
}

static GLint stubInteger(GLenum pname)
{
    switch (pname)
    {
        case GL_MAX_TEXTURE_SIZE:
            return 1024; /* PICA200 limit */
        case GL_MAX_TEXTURE_UNITS:
        case 0x8872: /* GL_MAX_TEXTURE_IMAGE_UNITS */
        case 0x8B4D: /* GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */
            return 3;
        case GL_MAX_LIGHTS:
            return 8;
        case GL_MAX_CLIP_PLANES:
            return 0;
        case 0x8869: /* GL_MAX_VERTEX_ATTRIBS */
            return 12;
        case 0x821B: /* GL_MAJOR_VERSION */
            return 2;
        default:
            return 0;
    }
}

void glGetIntegerv(GLenum pname, GLint* params)
{
    if (params)
        params[0] = stubInteger(pname);
}

void glGetFloatv(GLenum pname, GLfloat* params)
{
    if (params)
        params[0] = (GLfloat)stubInteger(pname);
}

void glGetDoublev(GLenum pname, GLdouble* params)
{
    if (params)
        params[0] = (GLdouble)stubInteger(pname);
}

void glGetBooleanv(GLenum pname, GLboolean* params)
{
    if (params)
        params[0] = stubInteger(pname) != 0;
}

GLenum glGetError(void)
{
    return GL_NO_ERROR;
}

GLenum glCheckFramebufferStatus(GLenum target)
{
    (void)target;
    return 0x8CD5; /* GL_FRAMEBUFFER_COMPLETE */
}

GLenum glCheckFramebufferStatusEXT(GLenum target)
{
    (void)target;
    return 0x8CD5;
}

/* Object-name generators hand out increasing ids so OSG's bookkeeping stays consistent. */
static GLuint gNextName = 1;
static void genNames(GLsizei n, GLuint* names)
{
    for (GLsizei i = 0; i < n; ++i)
        names[i] = gNextName++;
}
void glGenTextures(GLsizei n, GLuint* t) { genNames(n, t); }
void glGenBuffers(GLsizei n, GLuint* b) { genNames(n, b); }
void glGenBuffersARB(GLsizei n, GLuint* b) { genNames(n, b); }
void glGenFramebuffers(GLsizei n, GLuint* f) { genNames(n, f); }
void glGenFramebuffersEXT(GLsizei n, GLuint* f) { genNames(n, f); }
void glGenRenderbuffers(GLsizei n, GLuint* r) { genNames(n, r); }
void glGenRenderbuffersEXT(GLsizei n, GLuint* r) { genNames(n, r); }
void glGenQueries(GLsizei n, GLuint* q) { genNames(n, q); }
void glGenVertexArrays(GLsizei n, GLuint* a) { genNames(n, a); }
GLuint glGenLists(GLsizei range)
{
    GLuint first = gNextName;
    gNextName += (GLuint)range;
    return first;
}
GLuint glCreateProgram(void) { return gNextName++; }
GLuint glCreateShader(GLenum type) { (void)type; return gNextName++; }

/* Extension function pointers: none available. */
void* osg3ds_glGetProcAddress(const char* name)
{
    (void)name;
    return NULL;
}
