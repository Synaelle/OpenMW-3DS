#include "niffilemanager.hpp"

#include <iostream>

#include <components/vfs/manager.hpp>

#include "objectcache.hpp"

#include <components/misc/omw3dsdetail.hpp>

namespace Resource
{

    NifFileManager::NifFileManager(const VFS::Manager* vfs, const ToUTF8::StatelessUtf8Encoder* encoder)
        // NIF files aren't needed any more once the converted objects are cached in SceneManager / BulletShapeManager,
        // so no point in using an expiry delay.
        : ResourceManager(vfs, 0)
        , mEncoder(encoder)
    {
    }

    NifFileManager::~NifFileManager() = default;

    Nif::NIFFilePtr NifFileManager::get(VFS::Path::NormalizedView name)
    {
        if (Nif::NIFFilePtr cached = mCache->getRefFromObjectCache(name))
            return cached;

        auto file = std::make_shared<Nif::NIFFile>(name);
        Nif::Reader reader(*file, mEncoder);
        Files::IStreamPtr stream;
        {
            OMW3DS_DETAIL("vfs.get");
            stream = mVFS->get(name);
        }
        reader.parse(std::move(stream));
        mCache->addEntryToObjectCache(name.value(), file);
        return file;
    }

    void NifFileManager::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Nif", frameNumber, mCache->getStats(), *stats);
    }

}
