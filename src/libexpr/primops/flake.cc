#include "flake.hh"
#include "primops.hh"
#include "eval-inline.hh"
#include "fetchGit.hh"
#include "download.hh"
#include "args.hh"

#include <iostream>
#include <queue>
#include <regex>
#include <nlohmann/json.hpp>

namespace nix {

/* Read the registry or a lock file. (Currently they have an identical
   format. */
std::shared_ptr<FlakeRegistry> readRegistry(const Path & path)
{
    auto registry = std::make_shared<FlakeRegistry>();

    if (!pathExists(path))
        return std::make_shared<FlakeRegistry>();

    auto json = nlohmann::json::parse(readFile(path));

    auto version = json.value("version", 0);
    if (version != 1)
        throw Error("flake registry '%s' has unsupported version %d", path, version);

    auto flakes = json["flakes"];
    for (auto i = flakes.begin(); i != flakes.end(); ++i)
        registry->entries.emplace(i.key(), FlakeRef(i->value("uri", "")));

    return registry;
}

/* Write the registry or lock file to a file. */
void writeRegistry(FlakeRegistry registry, Path path)
{
    nlohmann::json json;
    json["version"] = 1;
    for (auto elem : registry.entries)
        json["flakes"][elem.first.to_string()] = { {"uri", elem.second.to_string()} };
    createDirs(dirOf(path));
    writeFile(path, json.dump(4)); // The '4' is the number of spaces used in the indentation in the json file.
}

LockFile::FlakeEntry readFlakeEntry(nlohmann::json json)
{
    FlakeRef flakeRef(json["uri"]);
    if (!flakeRef.isImmutable())
        throw Error("requested to fetch FlakeRef '%s' purely, which is mutable", flakeRef.to_string());

    LockFile::FlakeEntry entry(flakeRef);

    auto nonFlakeRequires = json["nonFlakeRequires"];

    for (auto i = nonFlakeRequires.begin(); i != nonFlakeRequires.end(); ++i) {
        FlakeRef flakeRef(i->value("uri", ""));
        if (!flakeRef.isImmutable())
            throw Error("requested to fetch FlakeRef '%s' purely, which is mutable", flakeRef.to_string());
        entry.nonFlakeEntries.insert_or_assign(i.key(), flakeRef);
    }

    auto requires = json["requires"];

    for (auto i = requires.begin(); i != requires.end(); ++i)
        entry.flakeEntries.insert_or_assign(i.key(), readFlakeEntry(*i));

    return entry;
}

LockFile readLockFile(const Path & path)
{
    LockFile lockFile;

    if (!pathExists(path))
        return lockFile;

    auto json = nlohmann::json::parse(readFile(path));

    auto version = json.value("version", 0);
    if (version != 1)
        throw Error("lock file '%s' has unsupported version %d", path, version);

    auto nonFlakeRequires = json["nonFlakeRequires"];

    for (auto i = nonFlakeRequires.begin(); i != nonFlakeRequires.end(); ++i) {
        FlakeRef flakeRef(i->value("uri", ""));
        if (!flakeRef.isImmutable())
            throw Error("requested to fetch FlakeRef '%s' purely, which is mutable", flakeRef.to_string());
        lockFile.nonFlakeEntries.insert_or_assign(i.key(), flakeRef);
    }

    auto requires = json["requires"];

    for (auto i = requires.begin(); i != requires.end(); ++i)
        lockFile.flakeEntries.insert_or_assign(i.key(), readFlakeEntry(*i));

    return lockFile;
}

nlohmann::json flakeEntryToJson(LockFile::FlakeEntry & entry)
{
    nlohmann::json json;
    json["uri"] = entry.ref.to_string();
    for (auto & x : entry.nonFlakeEntries)
        json["nonFlakeRequires"][x.first]["uri"] = x.second.to_string();
    for (auto & x : entry.flakeEntries)
        json["requires"][x.first] = flakeEntryToJson(x.second);
    return json;
}

void writeLockFile(LockFile lockFile, Path path)
{
    nlohmann::json json;
    json["version"] = 1;
    json["nonFlakeRequires"];
    for (auto & x : lockFile.nonFlakeEntries)
        json["nonFlakeRequires"][x.first]["uri"] = x.second.to_string();
    for (auto & x : lockFile.flakeEntries)
        json["requires"][x.first] = flakeEntryToJson(x.second);
    createDirs(dirOf(path));
    writeFile(path, json.dump(4)); // '4' = indentation in json file
}

std::shared_ptr<FlakeRegistry> getGlobalRegistry()
{
    return std::make_shared<FlakeRegistry>();
}

Path getUserRegistryPath()
{
    return getHome() + "/.config/nix/registry.json";
}

std::shared_ptr<FlakeRegistry> getUserRegistry()
{
    return readRegistry(getUserRegistryPath());
}

std::shared_ptr<FlakeRegistry> getLocalRegistry()
{
    Path registryFile = settings.nixDataDir + "/nix/flake-registry.json";
    return readRegistry(registryFile);
}

std::shared_ptr<FlakeRegistry> getFlagRegistry()
{
    // TODO (Nick): Implement this.
    return std::make_shared<FlakeRegistry>();
}

const std::vector<std::shared_ptr<FlakeRegistry>> EvalState::getFlakeRegistries()
{
    std::vector<std::shared_ptr<FlakeRegistry>> registries;
    if (evalSettings.pureEval) {
        registries.push_back(std::make_shared<FlakeRegistry>()); // global
        registries.push_back(std::make_shared<FlakeRegistry>()); // user
        registries.push_back(std::make_shared<FlakeRegistry>()); // local
    } else {
        registries.push_back(getGlobalRegistry());
        registries.push_back(getUserRegistry());
        registries.push_back(getLocalRegistry());
    }
    registries.push_back(getFlagRegistry());
    return registries;
}

// Creates a Nix attribute set value listing all dependencies, so they can be used in `provides`.
Value * makeFlakeRegistryValue(EvalState & state)
{
    auto v = state.allocValue();

    auto registries = state.getFlakeRegistries();

    int size = 0;
    for (auto registry : registries)
        size += registry->entries.size();
    state.mkAttrs(*v, size);

    for (auto & registry : registries) {
        for (auto & entry : registry->entries) {
            auto vEntry = state.allocAttr(*v, entry.first.to_string());
            state.mkAttrs(*vEntry, 2);
            mkString(*state.allocAttr(*vEntry, state.symbols.create("uri")), entry.second.to_string());
            vEntry->attrs->sort();
        }
    }

    v->attrs->sort();

    return v;
}

static FlakeRef lookupFlake(EvalState & state, const FlakeRef & flakeRef,
    std::vector<std::shared_ptr<FlakeRegistry>> registries, std::vector<FlakeRef> pastSearches = {})
{
    for (std::shared_ptr<FlakeRegistry> registry : registries) {
        auto i = registry->entries.find(flakeRef);
        if (i != registry->entries.end()) {
            auto newRef = i->second;
            if (std::get_if<FlakeRef::IsAlias>(&flakeRef.data)) {
                if (flakeRef.ref) newRef.ref = flakeRef.ref;
                if (flakeRef.rev) newRef.rev = flakeRef.rev;
            }
            std::string errorMsg = "found cycle in flake registries: ";
            for (FlakeRef oldRef : pastSearches) {
                errorMsg += oldRef.to_string();
                if (oldRef == newRef)
                    throw Error(errorMsg);
                errorMsg += " - ";
            }
            pastSearches.push_back(newRef);
            return lookupFlake(state, newRef, registries, pastSearches);
        }
    }
    if (!flakeRef.isDirect())
        throw Error("indirect flake URI '%s' is the result of a lookup", flakeRef.to_string());
    return flakeRef;
}

struct FlakeSourceInfo
{
    Path storePath;
    std::optional<Hash> rev;
    std::optional<uint64_t> revCount;
};

static FlakeSourceInfo fetchFlake(EvalState & state, const FlakeRef flakeRef, bool impureIsAllowed = false)
{
    FlakeRef fRef = lookupFlake(state, flakeRef, state.getFlakeRegistries());

    // This only downloads only one revision of the repo, not the entire history.
    if (auto refData = std::get_if<FlakeRef::IsGitHub>(&fRef.data)) {
        if (evalSettings.pureEval && !impureIsAllowed && !fRef.isImmutable())
            throw Error("requested to fetch FlakeRef '%s' purely, which is mutable", fRef.to_string());

        // FIXME: use regular /archive URLs instead? api.github.com
        // might have stricter rate limits.

        // FIXME: support passing auth tokens for private repos.

        auto url = fmt("https://api.github.com/repos/%s/%s/tarball/%s",
            refData->owner, refData->repo,
            fRef.rev ? fRef.rev->to_string(Base16, false)
                : fRef.ref ? *fRef.ref : "master");

        auto result = getDownloader()->downloadCached(state.store, url, true, "source",
            Hash(), nullptr, fRef.rev ? 1000000000 : settings.tarballTtl);

        if (!result.etag)
            throw Error("did not receive an ETag header from '%s'", url);

        if (result.etag->size() != 42 || (*result.etag)[0] != '"' || (*result.etag)[41] != '"')
            throw Error("ETag header '%s' from '%s' is not a Git revision", *result.etag, url);

        FlakeSourceInfo info;
        info.storePath = result.path;
        info.rev = Hash(std::string(*result.etag, 1, result.etag->size() - 2), htSHA1);

        return info;
    }

    // This downloads the entire git history
    else if (auto refData = std::get_if<FlakeRef::IsGit>(&fRef.data)) {
        auto gitInfo = exportGit(state.store, refData->uri, fRef.ref,
            fRef.rev ? fRef.rev->to_string(Base16, false) : "", "source");
        FlakeSourceInfo info;
        info.storePath = gitInfo.storePath;
        info.rev = Hash(gitInfo.rev, htSHA1);
        info.revCount = gitInfo.revCount;
        return info;
    }

    else if (auto refData = std::get_if<FlakeRef::IsPath>(&fRef.data)) {
        if (!pathExists(refData->path + "/.git"))
            throw Error("flake '%s' does not reference a Git repository", refData->path);
        auto gitInfo = exportGit(state.store, refData->path, {}, "", "source");
        FlakeSourceInfo info;
        info.storePath = gitInfo.storePath;
        info.rev = Hash(gitInfo.rev, htSHA1);
        info.revCount = gitInfo.revCount;
        return info;
    }

    else abort();
}

// This will return the flake which corresponds to a given FlakeRef. The lookupFlake is done within this function.
Flake getFlake(EvalState & state, const FlakeRef & flakeRef, bool impureIsAllowed = false)
{
    FlakeSourceInfo sourceInfo = fetchFlake(state, flakeRef);
    debug("got flake source '%s' with revision %s",
        sourceInfo.storePath, sourceInfo.rev.value_or(Hash(htSHA1)).to_string(Base16, false));

    auto flakePath = sourceInfo.storePath;
    state.store->assertStorePath(flakePath);

    if (state.allowedPaths)
        state.allowedPaths->insert(flakePath);

    Flake flake(flakeRef);
    if (std::get_if<FlakeRef::IsGitHub>(&flakeRef.data)) {
        if (sourceInfo.rev)
            flake.ref = FlakeRef(flakeRef.baseRef().to_string()
                + "/" + sourceInfo.rev->to_string(Base16, false));
    }

    flake.path = flakePath;
    flake.revCount = sourceInfo.revCount;

    Value vInfo;
    state.evalFile(flakePath + "/flake.nix", vInfo); // FIXME: symlink attack

    state.forceAttrs(vInfo);

    if (auto name = vInfo.attrs->get(state.sName))
        flake.id = state.forceStringNoCtx(*(**name).value, *(**name).pos);
    else
        throw Error("flake lacks attribute 'name'");

    if (auto description = vInfo.attrs->get(state.sDescription))
        flake.description = state.forceStringNoCtx(*(**description).value, *(**description).pos);

    if (auto requires = vInfo.attrs->get(state.symbols.create("requires"))) {
        state.forceList(*(**requires).value, *(**requires).pos);
        for (unsigned int n = 0; n < (**requires).value->listSize(); ++n)
            flake.requires.push_back(FlakeRef(state.forceStringNoCtx(
                *(**requires).value->listElems()[n], *(**requires).pos)));
    }

    if (std::optional<Attr *> nonFlakeRequires = vInfo.attrs->get(state.symbols.create("nonFlakeRequires"))) {
        state.forceAttrs(*(**nonFlakeRequires).value, *(**nonFlakeRequires).pos);
        for (Attr attr : *(*(**nonFlakeRequires).value).attrs) {
            std::string myNonFlakeUri = state.forceStringNoCtx(*attr.value, *attr.pos);
            FlakeRef nonFlakeRef = FlakeRef(myNonFlakeUri);
            flake.nonFlakeRequires.insert_or_assign(attr.name, nonFlakeRef);
        }
    }

    if (auto provides = vInfo.attrs->get(state.symbols.create("provides"))) {
        state.forceFunction(*(**provides).value, *(**provides).pos);
        flake.vProvides = (**provides).value;
    } else
        throw Error("flake lacks attribute 'provides'");

    const Path lockFile = flakePath + "/flake.lock"; // FIXME: symlink attack

    flake.lockFile = readLockFile(lockFile);

    return flake;
}

// Get the `NonFlake` corresponding to a `FlakeRef`.
NonFlake getNonFlake(EvalState & state, const FlakeRef & flakeRef, FlakeAlias alias)
{
    FlakeSourceInfo sourceInfo = fetchFlake(state, flakeRef);
    debug("got non-flake source '%s' with revision %s",
        sourceInfo.storePath, sourceInfo.rev.value_or(Hash(htSHA1)).to_string(Base16, false));

    auto flakePath = sourceInfo.storePath;
    state.store->assertStorePath(flakePath);

    if (state.allowedPaths)
        state.allowedPaths->insert(flakePath);

    NonFlake nonFlake(flakeRef);
    if (std::get_if<FlakeRef::IsGitHub>(&flakeRef.data)) {
        if (sourceInfo.rev)
            nonFlake.ref = FlakeRef(flakeRef.baseRef().to_string()
                + "/" + sourceInfo.rev->to_string(Base16, false));
    }

    nonFlake.path = flakePath;

    nonFlake.alias = alias;

    return nonFlake;
}

/* Given a flake reference, recursively fetch it and its
   dependencies.
   FIXME: this should return a graph of flakes.
*/
Dependencies resolveFlake(EvalState & state, const FlakeRef & topRef, bool impureTopRef, bool isTopFlake)
{
    Flake flake = getFlake(state, topRef, isTopFlake && impureTopRef);
    Dependencies deps(flake);

    for (auto & nonFlakeInfo : flake.nonFlakeRequires)
        deps.nonFlakeDeps.push_back(getNonFlake(state, nonFlakeInfo.second, nonFlakeInfo.first));

    for (auto & newFlakeRef : flake.requires)
        deps.flakeDeps.push_back(resolveFlake(state, newFlakeRef, false));

    return deps;
}

LockFile::FlakeEntry dependenciesToFlakeEntry(Dependencies & deps)
{
    LockFile::FlakeEntry entry(deps.flake.ref);

    for (Dependencies & deps : deps.flakeDeps)
        entry.flakeEntries.insert_or_assign(deps.flake.id, dependenciesToFlakeEntry(deps));

    for (NonFlake & nonFlake : deps.nonFlakeDeps)
        entry.nonFlakeEntries.insert_or_assign(nonFlake.alias, nonFlake.ref);

    return entry;
}

LockFile getLockFile(EvalState & evalState, FlakeRef & flakeRef)
{
    Dependencies deps = resolveFlake(evalState, flakeRef, true);
    LockFile::FlakeEntry entry = dependenciesToFlakeEntry(deps);
    LockFile lockFile;
    lockFile.flakeEntries = entry.flakeEntries;
    lockFile.nonFlakeEntries = entry.nonFlakeEntries;
    return lockFile;
}

void updateLockFile(EvalState & state, Path path)
{
    // 'path' is the path to the local flake repo.
    FlakeRef flakeRef = FlakeRef("file://" + path);
    if (std::get_if<FlakeRef::IsGit>(&flakeRef.data)) {
        LockFile lockFile = getLockFile(state, flakeRef);
        writeLockFile(lockFile, path + "/flake.lock");
    } else if (std::get_if<FlakeRef::IsGitHub>(&flakeRef.data)) {
        throw UsageError("you can only update local flakes, not flakes on GitHub");
    } else {
        throw UsageError("you can only update local flakes, not flakes through their FlakeAlias");
    }
}

// Return the `provides` of the top flake, while assigning to `v` the provides
// of the dependencies as well.
Value * makeFlakeValue(EvalState & state, const FlakeRef & flakeRef, bool impureTopRef, Value & v)
{
    Dependencies deps = resolveFlake(state, flakeRef, impureTopRef);

    // FIXME: we should call each flake with only its dependencies
    // (rather than the closure of the top-level flake).

    auto vResult = state.allocValue();
    // This will store the attribute set of the `nonFlakeRequires` and the `requires.provides`.

    state.mkAttrs(*vResult, deps.flakeDeps.size());

    Value * vTop = state.allocAttr(*vResult, deps.flake.id);

    for (auto & dep : deps.flakeDeps) {
        Flake flake = dep.flake;
        auto vFlake = state.allocAttr(*vResult, flake.id);

        state.mkAttrs(*vFlake, 4);

        mkString(*state.allocAttr(*vFlake, state.sDescription), flake.description);

        state.store->assertStorePath(flake.path);
        mkString(*state.allocAttr(*vFlake, state.sOutPath), flake.path, {flake.path});

        if (flake.revCount)
            mkInt(*state.allocAttr(*vFlake, state.symbols.create("revCount")), *flake.revCount);

        auto vProvides = state.allocAttr(*vFlake, state.symbols.create("provides"));
        mkApp(*vProvides, *flake.vProvides, *vResult);

        vFlake->attrs->sort();
    }

    vResult->attrs->sort();

    v = *vResult;

    assert(vTop);
    return vTop;
}

// This function is exposed to be used in nix files.
static void prim_getFlake(EvalState & state, const Pos & pos, Value * * args, Value & v)
{
    makeFlakeValue(state, state.forceStringNoCtx(*args[0], pos), false, v);
}

static RegisterPrimOp r2("getFlake", 1, prim_getFlake);

}
