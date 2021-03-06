/**
 * Copyright (c) 2017 Melown Technologies SE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * *  Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "../include/vts-browser/search.hpp"

#include "../utilities/json.hpp"
#include "../searchTask.hpp"
#include "../coordsManip.hpp"
#include "../fetchTask.hpp"
#include "../mapConfig.hpp"
#include "../map.hpp"

#include <optick.h>

namespace vts
{

namespace
{

std::string generateSearchUrl(MapImpl *impl, const std::string &query)
{
    std::string url = impl->mapconfig->browserOptions.searchUrl;
    static const std::string rep = "{value}";
    auto s = url.find(rep);
    if (s != std::string::npos)
        url.replace(s, rep.length(), utility::urlEncode(query));
    return url;
}

void searchToNav(MapImpl *map, double point[3])
{
    vec3 p = rawToVec3(point);
    p = map->convertor->searchToNav(p);
    vecToRaw(p, point);
}

// both points are in navigation srs
double distance(MapImpl *map, const double a[], const double b[],
                double def = std::numeric_limits<double>::quiet_NaN())
{
    for (int i = 0; i < 3; i++)
        if (a[i] != a[i] || b[i] != b[i])
            return def;
    vec3 va(a[0], a[1], a[2]);
    vec3 vb(b[0], b[1], b[2]);
    switch (map->mapconfig->srs.get(
                map->mapconfig->referenceFrame.model.navigationSrs).type)
    {
    case vtslibs::registry::Srs::Type::cartesian:
    case vtslibs::registry::Srs::Type::projected:
        return length(vec3(vb - va));
    case vtslibs::registry::Srs::Type::geographic:
        return map->convertor->geoDistance(va, vb);
    }
    return def;
}

bool populated(const SearchItem &a)
{
    return a.type == "hamlet"
        || a.type == "village"
        || a.type == "town"
        || a.type == "city";
}

void filterSearchResults(MapImpl *map, const std::shared_ptr<SearchTask> &task)
{
    // dedupe
    if (task->results.size() > 1)
    {
        std::stable_sort(task->results.begin(), task->results.end(),
            [](const SearchItem &a, const SearchItem &b){
            if (a.displayName == b.displayName)
                return populated(a) > populated(b);
            return false;
        });
        task->results.erase(
                    std::unique(task->results.begin(), task->results.end(),
                                [](const SearchItem &a, const SearchItem &b){
            return a.displayName == b.displayName;
        }), task->results.end());
    }

    // filter results, that are close to each other
    if (task->results.size() > 1)
    {
        task->results.erase(
                    std::unique(task->results.begin(), task->results.end(),
                                [map](SearchItem &a, SearchItem &b){
            return distance(map, a.position, b.position) < 1e4;
        }), task->results.end());
    }

    // update some fields
    for (SearchItem &it : task->results)
    {
        // title
        {
            auto s = it.displayName.find(",");
            if (s != std::string::npos)
                it.title = it.displayName.substr(0, s);
            else
                it.title = it.displayName;
        }
        // region
        {
            if (!it.county.empty())
                it.region = it.county;
            else if (!it.stateDistrict.empty())
                it.region = it.stateDistrict;
            else
                it.region = it.state;
            auto s = it.region.find(" - ");
            if (s != std::string::npos)
                it.region = it.region.substr(0, s);
        }
        // street name queries
        if (it.title == it.houseNumber && !it.road.empty())
            it.title = it.road;
    }

    // reshake hits by distance
    std::stable_sort(task->results.begin(), task->results.end(),
                     [](const SearchItem &a, const SearchItem &b){
        return a.importance < 0.4 && b.importance < 0.4
                && std::abs(a.importance - b.importance) < 0.06
                && a.distance < b.distance;
    });
}

double vtod(Json::Value &v)
{
    if (v.type() == Json::ValueType::realValue)
        return v.asDouble();
    std::stringstream ss(v.asString());
    double f = nan1();
    ss >> std::noskipws >> f;
    if ((ss.rdstate() ^ std::ios_base::eofbit))
        return nan1();
    return f;
}

} // namespace

void MapImpl::parseSearchResults(const std::shared_ptr<SearchTask> &task)
{
    assert(!task->done);
    try
    {
        Json::Value root;
        try
        {
            root = stringToJson(task->impl->data.str());
        }
        catch(const std::exception &e)
        {
            LOGTHROW(err2, std::runtime_error)
                    << "Failed to parse search result json, url: <"
                    << task->impl->name << ">, error: <"
                    << e.what() << ">";
        }
        for (Json::Value &it : root)
        {
            SearchItem t;
            t.displayName = it["display_name"].asString();
            t.type = it["type"].asString();
            Json::Value addr = it["address"];
            if (!addr.empty())
            {
                t.houseNumber = addr["house_number"].asString();
                t.road = addr["road"].asString();
                t.city = addr["city"].asString();
                t.county = addr["county"].asString();
                t.state = addr["state"].asString();
                t.stateDistrict = addr["state_district"].asString();
                t.country = addr["country"].asString();
                t.countryCode = addr["country_code"].asString();
            }
            t.position[0] = vtod(it["lon"]);
            t.position[1] = vtod(it["lat"]);
            t.position[2] = 0;
            searchToNav(this, t.position);
            t.distance = distance(this, task->position, t.position);
            Json::Value bj = it["boundingbox"];
            if (bj.size() == 4)
            {
                double r[4];
                int i = 0;
                for (auto it : bj)
                    r[i++] = vtod(it);
                double bbs[4][3] = {
                    { r[2], r[0], 0 },
                    { r[2], r[1], 0 },
                    { r[3], r[0], 0 },
                    { r[3], r[1], 0 }
                };
                t.radius = 0;
                for (int i = 0; i < 4; i++)
                {
                    searchToNav(this, bbs[i]);
                    t.radius = std::max(t.radius,
                                        distance(this, t.position, bbs[i]));
                }
            }
            t.importance = vtod(it["importance"]);
            task->results.push_back(t);
        }
        if (options.searchResultsFiltering)
            filterSearchResults(this, task);
    }
    catch (const std::exception &e)
    {
        LOG(err3) << "Failed to process search results, url: <"
                  << task->impl->name << ">, query: <"
                  << task->query << ">, error: <"
                  << e.what() << ">";
    }
}

SearchTaskImpl::SearchTaskImpl(MapImpl *map, const std::string &name) :
    Resource(map, name),
    validityUrl(map->mapconfig->browserOptions.searchUrl),
    validitySrs(map->mapconfig->browserOptions.searchSrs)
{}

void SearchTaskImpl::decode()
{
    data = std::move(fetch->reply.content);
}

FetchTask::ResourceType SearchTaskImpl::resourceType() const
{
    return FetchTask::ResourceType::Search;
}

SearchItem::SearchItem() :
    position{
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN()
    },
    radius(std::numeric_limits<double>::quiet_NaN()),
    distance(std::numeric_limits<double>::quiet_NaN()),
    importance(-1)
{}

SearchItem::SearchItem(const std::string &json) :
    SearchItem()
{
    Json::Value v = stringToJson(json);
    AJ(displayName, asString);
    AJ(title, asString);
    AJ(type, asString);
    AJ(region, asString);
    AJ(road, asString);
    AJ(city, asString);
    AJ(county, asString);
    AJ(state, asString);
    AJ(houseNumber, asString);
    AJ(stateDistrict, asString);
    AJ(country, asString);
    AJ(countryCode, asString);
    AJ(radius, asDouble);
    AJ(distance, asDouble);
    AJ(importance, asDouble);
    Json::Value p = v["position"];
    position[0] = p["x"].asDouble();
    position[1] = p["y"].asDouble();
    position[2] = p["z"].asDouble();
}

std::string SearchItem::toJson() const
{
    Json::Value p;
    p["x"] = position[0];
    p["y"] = position[1];
    p["z"] = position[2];
    Json::Value v;
    TJ(displayName, asString);
    TJ(title, asString);
    TJ(type, asString);
    TJ(region, asString);
    TJ(road, asString);
    TJ(city, asString);
    TJ(county, asString);
    TJ(state, asString);
    TJ(houseNumber, asString);
    TJ(stateDistrict, asString);
    TJ(country, asString);
    TJ(countryCode, asString);
    TJ(radius, asDouble);
    TJ(distance, asDouble);
    TJ(importance, asDouble);
    v["position"] = p;
    return jsonToString(v);
}

SearchTask::SearchTask(const std::string &query, const double point[3]) :
    query(query), position{point[0], point[1], point[2]}, done(false)
{}

SearchTask::~SearchTask()
{}

void SearchTask::updateDistances(const double point[3])
{
    if (!impl || !impl->map->mapconfig || !impl->map->mapconfigReady
        || impl->map->mapconfig->browserOptions.searchUrl != impl->validityUrl
        || impl->map->mapconfig->browserOptions.searchSrs != impl->validitySrs)
    {
        LOGTHROW(err1, std::runtime_error) << "Search is no longer valid";
    }
    for (auto &it : results)
    {
        it.distance = distance(impl->map, it.position, point);
    }
}

std::shared_ptr<SearchTask> MapImpl::search(const std::string &query,
                                            const double point[3])
{
    auto t = std::make_shared<SearchTask>(query, point);
    t->impl = getSearchTask(generateSearchUrl(this, query));
    t->impl->priority = std::numeric_limits<float>::infinity();
    if (!t->impl->fetch)
        t->impl->fetch = std::make_shared<FetchTaskImpl>(t->impl);
    t->impl->fetch->query.headers["Accept-Language"] = "en-US,en";
    resources.searchTasks.push_back(t);
    return t;
}

void MapImpl::updateSearch()
{
    OPTICK_EVENT();
    auto it = resources.searchTasks.begin();
    while (it != resources.searchTasks.end())
    {
        std::shared_ptr<SearchTask> t = it->lock();
        if (t)
        {
            switch (getResourceValidity(t->impl))
            {
            case Validity::Indeterminate:
                touchResource(t->impl);
                it++;
                continue;
            case Validity::Invalid:
                break;
            case Validity::Valid:
                parseSearchResults(t);
                break;
            }
            t->done = true;
        }
        it = resources.searchTasks.erase(it);
    }
}

} // namespace vts
