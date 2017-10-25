/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#include <entwine/tree/config-parser.hpp>

#include <cmath>
#include <limits>
#include <numeric>

#include <entwine/formats/cesium/settings.hpp>
#include <entwine/third/arbiter/arbiter.hpp>
#include <entwine/tree/builder.hpp>
#include <entwine/tree/config.hpp>
#include <entwine/tree/inference.hpp>
#include <entwine/tree/thread-pools.hpp>
#include <entwine/types/bounds.hpp>
#include <entwine/types/storage.hpp>
#include <entwine/types/manifest.hpp>
#include <entwine/types/metadata.hpp>
#include <entwine/types/reprojection.hpp>
#include <entwine/types/schema.hpp>
#include <entwine/types/subset.hpp>
#include <entwine/util/env.hpp>
#include <entwine/util/unique.hpp>

namespace entwine
{

namespace
{
    const bool shallow(
            env("TESTING_SHALLOW") &&
            *env("TESTING_SHALLOW") == "true");
}

namespace
{
    Json::Reader reader;
}

Json::Value ConfigParser::defaults()
{
    Json::Value json;

    json["tmp"] = arbiter::fs::getTempPath();
    json["threads"] = 8;
    json["trustHeaders"] = true;

    if (!shallow)
    {
        json["pointsPerChunk"] = std::pow(4, 9);
        json["nullDepth"] = 7;
        json["baseDepth"] = 10;
    }
    else
    {
        std::cout << "Using shallow test configuration" << std::endl;

        json["pointsPerChunk"] = std::pow(4, 5);
        json["nullDepth"] = 4;
        json["baseDepth"] = 6;
    }

    return json;
}

std::unique_ptr<Builder> ConfigParser::getBuilder(
        Json::Value j,
        std::shared_ptr<arbiter::Arbiter> arbiter)
{
    Config config(j);

    if (!arbiter)
    {
        arbiter = std::make_shared<arbiter::Arbiter>(config.arbiter());
    }

    const Json::Value d(defaults());
    for (const auto& k : d.getMemberNames())
    {
        if (!config.isMember(k)) config[k] = d[k];
    }

    /*
    const std::string out(json["output"].asString());
    const std::string tmp(json["tmp"].asString());
    const std::vector<std::string> preserveSpatial(
            extract<std::string>(json["preserveSpatial"]));

    const auto outType(arbiter::Arbiter::getType(out));
    if (outType == "s3" || outType == "gs") json["prefixIds"] = true;
    */

    normalizeInput(config, *arbiter);

    if (!config.force())
    {
        const auto input(extract<FileInfo>(config["input"]));

        OuterScope os;
        os.setArbiter(arbiter);

        if (auto builder = Builder::tryCreateExisting(config, os))
        {
            if (config.verbose())
            {
                std::cout << "Scanning for new files..." << std::endl;
            }

            // Only scan for files that aren't already in the index.
            auto diff(builder->metadata().manifest().diff(input));

            if (diff.size())
            {
                Inference inference(*builder, diff);
                inference.go();
                diff = inference.fileInfo();

                std::cout << "Adding " << diff.size() << " new files" <<
                    std::endl;
            }

            // If we have more paths to add, add them to the manifest.
            // Otherwise we might be continuing a partial build, in which case
            // the paths to be built are already outstanding in the manifest.
            //
            // It's plausible that the input field could be empty to continue
            // a previous build.
            if (config["input"].isArray()) builder->append(diff);
            return builder;
        }
    }

    if (config.absolute() && config.storage() == "laszip")
    {
        config["storage"] = "lazperf";
    }

    if (config.cesiumSettings()) config["reprojection"]["out"] = "EPSG:4978";

    const bool needsInference(
            !config.has("numPointsHint") ||
            !config.has("bounds") ||
            !config.has("schema") ||
            (!config.has("absolute") && !Delta::existsIn(config.get())));

    if (needsInference)
    {
        infer(config);
        /*
        if (config.verbose())
        {
            std::cout << "Performing dataset inference..." << std::endl;
        }

        Inference inference(
                fileInfo,
                reprojection.get(),
                trustHeaders,
                !absolute,
                tmp,
                workThreads + clipThreads,
                verbose,
                !!cesiumSettings,
                arbiter.get());

        if (transformation)
        {
            inference.transformation(*transformation);
        }

        inference.go();

        // Overwrite our initial fileInfo with the inferred version, which
        // contains details for each file instead of just paths.
        fileInfo = inference.fileInfo();

        if (!absolute && inference.delta())
        {
            if (!delta) delta = makeUnique<Delta>();

            if (!json.isMember("scale"))
            {
                delta->scale() = inference.delta()->scale();
            }

            if (!json.isMember("offset"))
            {
                delta->offset() = inference.delta()->offset();
            }
        }

        if (!boundsConforming)
        {
            boundsConforming = makeUnique<Bounds>(inference.bounds());

            if (verbose)
            {
                std::cout << "Inferred: " << inference.bounds() << std::endl;
            }
        }
        else if (delta)
        {
            // If we were passed a bounds initially, it might not match the
            // inference we just performed.  Make sure our offset is consistent
            // with what we'll use as our bounds later.
            delta->offset() = boundsConforming->mid().apply([](double d)
            {
                const int64_t v(d);
                if (static_cast<double>(v / 10 * 10) == d) return v;
                else return (v + 10) / 10 * 10;
            });
        }

        if (!schema)
        {
            auto dims(inference.schema().dims());
            if (delta)
            {
                const Bounds cube(
                        Metadata::makeScaledCube(
                            *boundsConforming,
                            delta.get()));
                dims = Schema::deltify(cube, *delta, inference.schema()).dims();
            }

            const std::size_t pointIdSize([&fileInfo]()
            {
                std::size_t max(0);
                for (const auto& f : fileInfo)
                {
                    max = std::max(max, f.numPoints());
                }

                if (max <= std::numeric_limits<uint32_t>::max()) return 4;
                else return 8;
            }());

            const std::size_t originSize([&fileInfo]()
            {
                if (fileInfo.size() <= std::numeric_limits<uint16_t>::max())
                    return 2;
                if (fileInfo.size() <= std::numeric_limits<uint32_t>::max())
                    return 4;
                else
                    return 8;
            }());

            for (const auto s : preserveSpatial)
            {
                if (std::none_of(
                            dims.begin(),
                            dims.end(),
                            [s](const DimInfo& d) { return d.name() == s; }))
                {
                    dims.emplace_back(s, "floating", 8);
                }
            }

            dims.emplace_back("OriginId", "unsigned", originSize);

            if (storePointId)
            {
                dims.emplace_back("PointId", "unsigned", pointIdSize);
            }

            schema = makeUnique<Schema>(dims);
        }

        if (!numPointsHint) numPointsHint = inference.numPoints();

        if (!transformation)
        {
            if (const std::vector<double>* t = inference.transformation())
            {
                transformation = makeUnique<std::vector<double>>(*t);
            }
        }
        */
    }

    /*
    auto subset(maybeAccommodateSubset(json, *boundsConforming, delta.get()));
    json["numPointsHint"] = static_cast<Json::UInt64>(numPointsHint);

    const double density(
            json.isMember("density") ?
                json["density"].asDouble() : densityLowerBound(fileInfo));

    Structure structure(json);
    const auto pre(structure.sparseDepthBegin());
    if (structure.applyDensity(density, boundsConforming->cubeify()))
    {
        const auto post(structure.sparseDepthBegin());
        if (post > pre)
        {
            std::cout << "Applied density " <<
                "(+" << (post - pre) << ")" << std::endl;
        }
    }

    Structure hierarchyStructure(Hierarchy::structure(structure, subset.get()));

    const auto ep(arbiter->getEndpoint(json["output"].asString()));
    const Manifest manifest(fileInfo, ep);

    const Metadata metadata(json);
            *boundsConforming,
            *schema,
            structure,
            hierarchyStructure,
            manifest,
            trustHeaders,
            storage,
            hierarchyCompression,
            density,
            reprojection.get(),
            subset.get(),
            delta.get(),
            transformation.get(),
            cesiumSettings.get(),
            preserveSpatial);

    OuterScope outerScope;
    outerScope.setArbiter(arbiter);

    auto builder = makeUnique<Builder>(
            metadata,
            out,
            tmp,
            workThreads,
            clipThreads,
            outerScope);
    */

    return std::unique_ptr<Builder>();
}

void ConfigParser::infer(Config& config)
{
    if (config.verbose()) std::cout << "Scanning files..." << std::endl;

    Inference inference(config);

    /*
    if (transformation)
    {
        inference.transformation(*transformation);
    }

    inference.go();

    // Overwrite our initial fileInfo with the inferred version, which
    // contains details for each file instead of just paths.
    fileInfo = inference.fileInfo();

    if (!absolute && inference.delta())
    {
        if (!delta) delta = makeUnique<Delta>();

        if (!json.isMember("scale"))
        {
            delta->scale() = inference.delta()->scale();
        }

        if (!json.isMember("offset"))
        {
            delta->offset() = inference.delta()->offset();
        }
    }

    if (!boundsConforming)
    {
        boundsConforming = makeUnique<Bounds>(inference.bounds());

        if (verbose)
        {
            std::cout << "Inferred: " << inference.bounds() << std::endl;
        }
    }
    else if (delta)
    {
        // If we were passed a bounds initially, it might not match the
        // inference we just performed.  Make sure our offset is consistent
        // with what we'll use as our bounds later.
        delta->offset() = boundsConforming->mid().apply([](double d)
        {
            const int64_t v(d);
            if (static_cast<double>(v / 10 * 10) == d) return v;
            else return (v + 10) / 10 * 10;
        });
    }

    if (!schema)
    {
        auto dims(inference.schema().dims());
        if (delta)
        {
            const Bounds cube(
                    Metadata::makeScaledCube(
                        *boundsConforming,
                        delta.get()));
            dims = Schema::deltify(cube, *delta, inference.schema()).dims();
        }

        const std::size_t pointIdSize([&fileInfo]()
        {
            std::size_t max(0);
            for (const auto& f : fileInfo)
            {
                max = std::max(max, f.numPoints());
            }

            if (max <= std::numeric_limits<uint32_t>::max()) return 4;
            else return 8;
        }());

        const std::size_t originSize([&fileInfo]()
        {
            if (fileInfo.size() <= std::numeric_limits<uint16_t>::max())
                return 2;
            if (fileInfo.size() <= std::numeric_limits<uint32_t>::max())
                return 4;
            else
                return 8;
        }());

        for (const auto s : preserveSpatial)
        {
            if (std::none_of(
                        dims.begin(),
                        dims.end(),
                        [s](const DimInfo& d) { return d.name() == s; }))
            {
                dims.emplace_back(s, "floating", 8);
            }
        }

        dims.emplace_back("OriginId", "unsigned", originSize);

        if (storePointId)
        {
            dims.emplace_back("PointId", "unsigned", pointIdSize);
        }

        schema = makeUnique<Schema>(dims);
    }

    if (!numPointsHint) numPointsHint = inference.numPoints();

    if (!transformation)
    {
        if (const std::vector<double>* t = inference.transformation())
        {
            transformation = makeUnique<std::vector<double>>(*t);
        }
    }
    */
}

void ConfigParser::normalizeInput(Config& c, const arbiter::Arbiter& arbiter)
{
    Json::Value& input(c["input"]);

    const std::string extension(
            input.isString() ?
                arbiter::Arbiter::getExtension(input.asString()) : "");

    const bool isInferencePath(extension == "entwine-inference");

    if (!isInferencePath)
    {
        // The input source is a path or array of paths.  First, we possibly
        // need to expand out directories into their containing files.
        FileInfoList fileInfo;

        auto insert([&c, &fileInfo, &arbiter](std::string in)
        {
            Paths current(arbiter.resolve(in, c.verbose()));
            std::sort(current.begin(), current.end());
            for (const auto& f : current) fileInfo.emplace_back(f);
        });

        if (input.isArray())
        {
            for (const auto& entry : input)
            {
                if (entry.isString())
                {
                    insert(directorify(entry.asString()));
                }
                else
                {
                    fileInfo.emplace_back(entry);
                }
            }
        }
        else if (input.isString())
        {
            insert(directorify(input.asString()));
        }
        else return;

        // Now, we have an array of files (no directories).
        //
        // Reset our input with our resolved paths.  config.input.fileInfo will
        // be an array of strings, containing only paths with no associated
        // information.
        input = Json::Value();
        input.resize(fileInfo.size());
        for (std::size_t i(0); i < fileInfo.size(); ++i)
        {
            input[Json::ArrayIndex(i)] = fileInfo[i].toJson();
        }
    }
    else if (isInferencePath)
    {
        const std::string path(input.asString());
        const Json::Value inference(parse(arbiter.get(path)));

        input = inference["fileInfo"];

        c.maybeSet("schema", inference["schema"]);
        c.maybeSet("bounds", inference["bounds"]);
        c.maybeSet("density", inference["density"]);
        c.maybeSet("numPointsHint", inference["numPointsHint"]);
        c.maybeSet("reprojection", inference["reprojection"]);

        if (Delta::existsIn(inference))
        {
            c.maybeSet("scale", inference["scale"]);
            c.maybeSet("offset", inference["offset"]);
        }
    }
}

std::string ConfigParser::directorify(const std::string rawPath)
{
    std::string s(rawPath);

    if (s.size() && s.back() != '*')
    {
        if (arbiter::util::isDirectory(s))
        {
            s += '*';
        }
        else if (
                arbiter::util::getBasename(s).find_first_of('.') ==
                std::string::npos)
        {
            s += "/*";
        }
    }

    return s;
}

std::unique_ptr<Subset> ConfigParser::maybeAccommodateSubset(
        Json::Value& json,
        const Bounds& boundsConforming,
        const Delta* delta)
{
    std::unique_ptr<Subset> subset;
    const bool verbose(json["verbose"].asBool());

    if (json.isMember("subset"))
    {
        Bounds cube(Metadata::makeNativeCube(boundsConforming, delta));
        subset = makeUnique<Subset>(cube, json["subset"]);
        const std::size_t configNullDepth(json["nullDepth"].asUInt64());
        const std::size_t minimumNullDepth(subset->minimumNullDepth());

        if (configNullDepth < minimumNullDepth)
        {
            if (verbose)
            {
                std::cout <<
                    "Bumping null depth to accomodate subset: " <<
                    minimumNullDepth << std::endl;
            }

            json["nullDepth"] = Json::UInt64(minimumNullDepth);
        }

        const std::size_t configBaseDepth(json["baseDepth"].asUInt64());
        const std::size_t ppc(json["pointsPerChunk"].asUInt64());
        const std::size_t minimumBaseDepth(subset->minimumBaseDepth(ppc));

        if (configBaseDepth < minimumBaseDepth)
        {
            if (verbose)
            {
                std::cout <<
                    "Bumping base depth to accomodate subset: " <<
                    minimumBaseDepth << std::endl;
            }

            json["baseDepth"] = Json::UInt64(minimumBaseDepth);
            json["bumpDepth"] = Json::UInt64(configBaseDepth);
        }
    }

    return subset;
}

} // namespace entwine

