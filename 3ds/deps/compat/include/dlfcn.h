/*
 * 3DS compat: there is no dynamic loader. Code that merely *can* load shared objects (OSG's plugin
 * registry and GL extension lookup) compiles against these and gets "not available" at runtime.
 * all plugins are linked statically instead.
 */
#ifndef OPENMW3DS_COMPAT_DLFCN_H
#define OPENMW3DS_COMPAT_DLFCN_H

#define RTLD_LAZY 0x0001
#define RTLD_NOW 0x0002
#define RTLD_GLOBAL 0x0100
#define RTLD_LOCAL 0x0000
#define RTLD_DEFAULT ((void*)0)

#ifdef __cplusplus
extern "C" {
#endif

static inline void* dlopen(const char* file, int mode)
{
    (void)file;
    (void)mode;
    return (void*)0;
}

/* Provided by the stub GL (deps/stubgl): resolves every gl* name. Weak, so code that never links
 * the stub GL still builds. */
extern void* osg3ds_glGetProcAddress(const char* name) __attribute__((weak));

static inline void* dlsym(void* handle, const char* name)
{
    (void)handle;
    /* OSG looks GL 2.0+ entry points up with dlsym(0, "glCreateProgram") etc. */
    if (osg3ds_glGetProcAddress)
        return osg3ds_glGetProcAddress(name);
    return (void*)0;
}

static inline int dlclose(void* handle)
{
    (void)handle;
    return 0;
}

static inline char* dlerror(void)
{
    return (char*)"dynamic loading is not available on Nintendo 3DS";
}

#ifdef __cplusplus
}
#endif

#endif
