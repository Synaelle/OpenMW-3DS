#ifndef OPENMW_COMPONENTS_MISC_OMW3DSDETAIL_H
#define OPENMW_COMPONENTS_MISC_OMW3DSDETAIL_H

// 3DS: OMW3DS_DETAIL("label") times the rest of the enclosing scope into the open detail capture (see
// platform3ds/memprobe.hpp), e.g. the steps of one slow object insertion. Expands to nothing without the 3DS
// debug tools (OPENMW3DS_DEBUG) and on other platforms.
#ifdef OPENMW3DS_DEBUG
#include <platform3ds/memprobe.hpp>
#define OMW3DS_DETAIL_CAT2(a, b) a##b
#define OMW3DS_DETAIL_CAT(a, b) OMW3DS_DETAIL_CAT2(a, b)
#define OMW3DS_DETAIL(label) p3ds::perf::detail::Scope OMW3DS_DETAIL_CAT(omw3dsDetail, __LINE__)(label)
// Logs a resource cache miss while p3ds::perf::detail::setLogMisses is on (needs components/debug/debuglog.hpp).
#define OMW3DS_LOG_MISS(kind, name)                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (p3ds::perf::detail::logMisses())                                                                           \
            Log(Debug::Info) << "3ds miss " << kind << " " << name;                                                    \
    } while (false)
#else
#define OMW3DS_DETAIL(label)
#define OMW3DS_LOG_MISS(kind, name)
#endif

#endif
