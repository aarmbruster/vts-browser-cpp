#include <unordered_map>
#include <unordered_set>
#include <cstdio>

#include <boost/filesystem.hpp>
#include <dbglog/dbglog.hpp>

#include <renderer/fetcher.h>
#include <renderer/statistics.h>

#include "cache.h"
#include "buffer.h"
#include "map.h"

namespace melown
{

class CacheImpl : public Cache
{
public:
    enum class Status
    {
        initialized,
        downloading,
        ready,
        done,
        error,
    };

    CacheImpl(MapImpl *map, Fetcher *fetcher) : fetcher(fetcher), map(map),
        downloadingTasks(0)
    {
        if (!fetcher)
            return;
        Fetcher::Func func = std::bind(&CacheImpl::fetchedFile, this,
                                       std::placeholders::_1,
                                       std::placeholders::_2,
                                       std::placeholders::_3);
        fetcher->setCallback(func);
    }

    ~CacheImpl()
    {}

    const std::string convertNameToPath(std::string path, bool preserveSlashes)
    {
        path = boost::filesystem::path(path).normalize().string();
        std::string res;
        res.reserve(path.size());
        for (char it : path)
        {
            if ((it >= 'a' && it <= 'z')
             || (it >= 'A' && it <= 'Z')
             || (it >= '0' && it <= '9')
             || (it == '-' || it == '.'))
                res += it;
            else if (preserveSlashes && (it == '/' || it == '\\'))
                res += '/';
            else
                res += '_';
        }
        return res;
    }

    const std::string convertNameToCache(const std::string &path)
    {
        uint32 p = path.find("://");
        std::string a = p == std::string::npos ? path : path.substr(p + 3);
        std::string b = boost::filesystem::path(a).parent_path().string();
        std::string c = a.substr(b.length() + 1);
        return std::string("cache/")
                + convertNameToPath(b, false) + "/"
                + convertNameToPath(c, false);
    }

    Result result(Status status)
    {
        switch (status)
        {
        case Status::initialized: return Result::downloading;
        case Status::downloading: return Result::downloading;
        case Status::ready: return Result::ready;
        case Status::done: return Result::ready;
        case Status::error: return Result::error;
        default:
            throw "invalid cache data status";
        }
    }

    Buffer readLocalFileBuffer(const std::string &path)
    {
        FILE *f = fopen(path.c_str(), "rb");
        if (!f)
            throw "failed to read file";
        Buffer b;
        fseek(f, 0, SEEK_END);
        b.size = ftell(f);
        fseek(f, 0, SEEK_SET);
        b.data = malloc(b.size);
        if (!b.data)
            throw "out of memory";
        if (fread(b.data, b.size, 1, f) != 1)
            throw "failed to read file";
        fclose(f);
        return b;
    }

    void readLocalFile(const std::string &name, const std::string &path)
    {
        try
        {
            data[name] = readLocalFileBuffer(path);
            states[name] = Status::ready;
        }
        catch (...)
        {
            states[name] = Status::error;
        }
    }

    void writeLocalFile(const std::string &path, const Buffer &buffer)
    {
        boost::filesystem::create_directories(
                    boost::filesystem::path(path).parent_path());
        FILE *f = fopen(path.c_str(), "wb");
        if (!f)
            throw "failed to write file";
        if (fwrite(buffer.data, buffer.size, 1, f) != 1)
        {
            fclose(f);
            throw "failed to write file";
        }
        if (fclose(f) != 0)
            throw "failed to write file";
    }

    void fetchedFile(const std::string &name,
                     const char *buffer, uint32 size) override
    {
        downloadingTasks--;
        if (!buffer)
        {
            states[name] = Status::error;
            return;
        }
        Buffer b;
        b.size = size;
        b.data = malloc(size);
        memcpy(b.data, buffer, size);
        writeLocalFile(convertNameToCache(name), b);
        data[name] = std::move(b);
        states[name] = Status::ready;
    }

    Result read(const std::string &name, Buffer &buffer,
                bool allowDiskCache) override
    {
        if (states[name] == Status::initialized)
        {
            states[name] = Status::downloading;
            if (name.find("://") == std::string::npos)
                readLocalFile(name, name);
            else
            {
                std::string cachePath = convertNameToCache(name);
                if (allowDiskCache && boost::filesystem::exists(cachePath))
                {
                    readLocalFile(name, cachePath);
                    map->statistics->resourcesDiskLoaded++;
                }
                else if (downloadingTasks < 20)
                {
                    fetcher->fetch(name);
                    downloadingTasks++;
                    map->statistics->resourcesDownloaded++;
                }
                else
                    states[name] = Status::initialized;
            }
        }
        switch (states[name])
        {
        case Status::done:
        case Status::ready:
        {
            buffer = data[name];
            states[name] = Status::done;
        } break;
        }
        return result(states[name]);
    }

    std::unordered_map<std::string, Status> states;
    std::unordered_map<std::string, Buffer> data;
    Fetcher *fetcher;
    MapImpl *map;
    uint32 downloadingTasks;
};

Cache *Cache::create(MapImpl *map, Fetcher *fetcher)
{
    return new CacheImpl(map, fetcher);
}

Cache::~Cache()
{}

} // namespace melown