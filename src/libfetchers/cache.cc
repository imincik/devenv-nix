#include "cache.hh"
#include "sqlite.hh"
#include "sync.hh"
#include "store-api.hh"

#include <nlohmann/json.hpp>

namespace nix::fetchers {

static const char * schema = R"sql(

create table if not exists Cache (
    input     text not null,
    info      text not null,
    path      text not null,
    immutable integer not null,
    timestamp integer not null,
    primary key (input)
);

create table if not exists Facts (
    name     text not null,
    value    text not null,
    primary key (name)
);
)sql";

// FIXME: we should periodically purge/nuke this cache to prevent it
// from growing too big.

struct CacheImpl : Cache
{
    struct State
    {
        SQLite db;
        SQLiteStmt add, lookup, upsertFact, queryFact;
    };

    Sync<State> _state;

    CacheImpl()
    {
        auto state(_state.lock());

        auto dbPath = getCacheDir() + "/nix/fetcher-cache-v2.sqlite";
        createDirs(dirOf(dbPath));

        state->db = SQLite(dbPath);
        state->db.isCache();
        state->db.exec(schema);

        state->add.create(state->db,
            "insert or replace into Cache(input, info, path, immutable, timestamp) values (?, ?, ?, ?, ?)");

        state->lookup.create(state->db,
            "select info, path, immutable, timestamp from Cache where input = ?");

        state->upsertFact.create(state->db,
            "insert or replace into Facts(name, value) values (?, ?)");

        state->queryFact.create(state->db,
            "select value from Facts where name = ?");
    }

    void add(
        const Attrs & inAttrs,
        const Attrs & infoAttrs) override
    {
        _state.lock()->add.use()
            (attrsToJSON(inAttrs).dump())
            (attrsToJSON(infoAttrs).dump())
            ("") // no path
            (false)
            (time(0)).exec();
    }

    std::optional<Attrs> lookup2(const Attrs & inAttrs) override
    {
        if (auto res = lookupExpired2(inAttrs)) {
            if (!res->expired)
                return std::move(res->infoAttrs);
            debug("ignoring expired cache entry '%s'",
                attrsToJSON(inAttrs).dump());
        }
        return {};
    }

    std::optional<Result2> lookupExpired2(const Attrs & inAttrs) override
    {
        auto state(_state.lock());

        auto inAttrsJSON = attrsToJSON(inAttrs).dump();

        auto stmt(state->lookup.use()(inAttrsJSON));
        if (!stmt.next()) {
            debug("did not find cache entry for '%s'", inAttrsJSON);
            return {};
        }

        auto infoJSON = stmt.getStr(0);
        auto locked = stmt.getInt(2) != 0;
        auto timestamp = stmt.getInt(3);

        debug("using cache entry '%s' -> '%s'", inAttrsJSON, infoJSON);

        return Result2 {
            .expired = !locked && (settings.tarballTtl.get() == 0 || timestamp + settings.tarballTtl < time(0)),
            .infoAttrs = jsonToAttrs(nlohmann::json::parse(infoJSON)),
        };
    }

    void add(
        ref<Store> store,
        const Attrs & inAttrs,
        const Attrs & infoAttrs,
        const StorePath & storePath,
        bool locked) override
    {
        _state.lock()->add.use()
            (attrsToJSON(inAttrs).dump())
            (attrsToJSON(infoAttrs).dump())
            (store->printStorePath(storePath))
            (locked)
            (time(0)).exec();
    }

    std::optional<std::pair<Attrs, StorePath>> lookup(
        ref<Store> store,
        const Attrs & inAttrs) override
    {
        if (auto res = lookupExpired(store, inAttrs)) {
            if (!res->expired)
                return std::make_pair(std::move(res->infoAttrs), std::move(res->storePath));
            debug("ignoring expired cache entry '%s'",
                attrsToJSON(inAttrs).dump());
        }
        return {};
    }

    std::optional<Result> lookupExpired(
        ref<Store> store,
        const Attrs & inAttrs) override
    {
        auto state(_state.lock());

        auto inAttrsJSON = attrsToJSON(inAttrs).dump();

        auto stmt(state->lookup.use()(inAttrsJSON));
        if (!stmt.next()) {
            debug("did not find cache entry for '%s'", inAttrsJSON);
            return {};
        }

        auto infoJSON = stmt.getStr(0);
        auto storePath = store->parseStorePath(stmt.getStr(1));
        auto locked = stmt.getInt(2) != 0;
        auto timestamp = stmt.getInt(3);

        store->addTempRoot(storePath);
        if (!store->isValidPath(storePath)) {
            // FIXME: we could try to substitute 'storePath'.
            debug("ignoring disappeared cache entry '%s'", inAttrsJSON);
            return {};
        }

        debug("using cache entry '%s' -> '%s', '%s'",
            inAttrsJSON, infoJSON, store->printStorePath(storePath));

        return Result {
            .expired = !locked && (settings.tarballTtl.get() == 0 || timestamp + settings.tarballTtl < time(0)),
            .infoAttrs = jsonToAttrs(nlohmann::json::parse(infoJSON)),
            .storePath = std::move(storePath)
        };
    }

    void upsertFact(
        std::string_view key,
        std::string_view value) override
    {
        debug("upserting fact '%s' -> '%s'", key, value);
        _state.lock()->upsertFact.use()
            (key)
            (value).exec();
    }

    std::optional<std::string> queryFact(std::string_view key) override
    {
        auto state(_state.lock());

        auto stmt(state->queryFact.use()(key));
        if (!stmt.next()) return {};

        return stmt.getStr(0);
    }
};

ref<Cache> getCache()
{
    static auto cache = std::make_shared<CacheImpl>();
    return ref<Cache>(cache);
}

}
