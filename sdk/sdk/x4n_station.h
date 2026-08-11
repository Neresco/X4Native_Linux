// ---------------------------------------------------------------------------
// x4n_station.h — Station typed wrapper
// ---------------------------------------------------------------------------
// Part of the X4Native SDK. Included by x4native.h.
//
// Provides:
//   x4n::station::Station — lightweight handle for station/defensible entities
//
// Usage:
//   x4n::station::Station station(station_id);
//   for (auto& mod : station.modules()) {
//       if (mod.is_production() && mod.ware_id() == "energycells") {
//           mod.set_paused(true);
//       }
//   }
//   // Or all-in-one:
//   uint32_t affected = station.set_ware_paused("energycells", true);
//
// Thin handle: UniverseID + cached X4Component*. No heap allocation
// except the module vector from enumeration.
// All functions require on_game_loaded to have fired.
// ---------------------------------------------------------------------------
#pragma once

#include "x4n_core.h"
#include "x4n_entity.h"
#include "x4n_module.h"
#include <cstdint>
#include <string>
#include <vector>

namespace x4n { namespace station {

/// Lightweight typed handle for a station/defensible entity.
/// Constructor validates the entity is actually a Station class.
class Station {
    UniverseID id_;
    X4Component* comp_;
public:
    explicit Station(UniverseID id)
        : id_(id), comp_(entity::find_component(id)) {
        if (comp_ && !entity::is_a(comp_, GameClass::Station))
            comp_ = nullptr;
    }

    bool valid() const { return comp_ != nullptr; }
    UniverseID id() const { return id_; }

    const char* name() const {
        auto* g = game();
        return (valid() && g) ? g->GetComponentName(id_) : "";
    }
    const char* macro() const {
        return valid() ? entity::get_component_macro(comp_) : nullptr;
    }

    /// Enumerate all modules on this station.
    std::vector<module::Module> modules(bool include_construction = false,
                                        bool include_wrecks = false) const {
        std::vector<module::Module> result;
        auto* g = game();
        if (!valid() || !g) return result;

        uint32_t count = g->GetNumStationModules(id_, include_construction, include_wrecks);
        if (count == 0) return result;

        auto buf = std::vector<UniverseID>(count);
        uint32_t actual = g->GetStationModules(buf.data(), count, id_,
                                                include_construction, include_wrecks);
        result.reserve(actual);
        for (uint32_t i = 0; i < actual; ++i) {
            result.emplace_back(buf[i]);
        }
        return result;
    }

    /// Check if this station produces a given ware (fast, station-level check).
    /// Uses ignorestate=true so it returns > 0 even when modules are paused.
    bool produces(const char* ware_id) const {
        auto* g = game();
        if (!valid() || !g) return false;
        return g->GetContainerWareProduction(id_, ware_id, true) > 0.0;
    }

    /// Pause or resume all production/processing modules for a specific ware.
    /// Returns the number of modules affected (0 if ware not found).
    uint32_t set_ware_paused(const std::string& ware_id, bool paused) {
        if (!produces(ware_id.c_str())) return 0;
        uint32_t affected = 0;
        for (auto& mod : modules()) {
            if (!mod.is_operational()) continue;
            if (!mod.is_production() && !mod.is_processing()) continue;
            if (mod.ware_id() != ware_id) continue;
            mod.set_paused(paused);
            ++affected;
        }
        return affected;
    }

    // -----------------------------------------------------------------------
    // Per-ware state (engine-canonical; folds workforce + production + build)
    // -----------------------------------------------------------------------

    /// Effective per-ware consumption rate (units/hour). Engine-canonical:
    /// sums recipe inputs from running production modules, workforce consumption
    /// from habitation modules, and build-resource draw from active builds.
    /// @param ignore_state When true, returns the max capacity rate regardless
    ///        of current operational state (paused/stalled/no-workforce).
    double consumption(const char* ware_id, bool ignore_state = false) const {
        auto* g = game();
        if (!valid() || !g) return 0.0;
        return g->GetContainerWareConsumption(id_, ware_id, ignore_state);
    }

    /// Effective per-ware production rate (units/hour).
    /// @param ignore_state true → theoretical max, false → live (modifiers applied).
    double production_rate(const char* ware_id, bool ignore_state = false) const {
        auto* g = game();
        if (!valid() || !g) return 0.0;
        return g->GetContainerWareProduction(id_, ware_id, ignore_state);
    }

    /// How many units of this ware this station currently wants to buy (trade-offer target).
    int32_t buy_limit(const char* ware_id) const {
        auto* g = game();
        if (!valid() || !g) return 0;
        return g->GetContainerBuyLimit(id_, ware_id);
    }

    /// How many units of this ware this station currently wants to sell (trade-offer target).
    int32_t sell_limit(const char* ware_id) const {
        auto* g = game();
        if (!valid() || !g) return 0;
        return g->GetContainerSellLimit(id_, ware_id);
    }

    /// Engine-canonical list of ware IDs critical for this container.
    /// Includes workforce consumables, recipe inputs, and build resources
    /// — whatever the engine knows the container requires.
    /// Safe to call even when the station currently holds zero stock of any ware.
    std::vector<std::string> critical_wares() const {
        std::vector<std::string> result;
        auto* g = game();
        if (!valid() || !g) return result;
        uint32_t n = g->GetNumContainerCriticalWares(id_);
        if (n == 0) return result;
        auto buf = std::vector<const char*>(n);
        uint32_t actual = g->GetContainerCriticalWares(buf.data(), n, id_);
        result.reserve(actual);
        for (uint32_t i = 0; i < actual; ++i) {
            if (buf[i]) result.emplace_back(buf[i]);
        }
        return result;
    }

    /// Wares required by the container's active build (if any).
    /// For a regular station this returns the build-queue's required wares;
    /// for a buildstorage object it returns its own build_resources.
    std::vector<std::string> build_resources() const {
        std::vector<std::string> result;
        auto* g = game();
        if (!valid() || !g) return result;
        uint32_t n = g->GetNumContainerBuildResources(id_);
        if (n == 0) return result;
        auto buf = std::vector<const char*>(n);
        uint32_t actual = g->GetContainerBuildResources(buf.data(), n, id_);
        result.reserve(actual);
        for (uint32_t i = 0; i < actual; ++i) {
            if (buf[i]) result.emplace_back(buf[i]);
        }
        return result;
    }

    /// Per-ware cargo enumeration. Returns the station's current cargo entries
    /// (ware_id + amount) matching the given tag filter.
    /// @param tags Ware-tag filter; empty string = all wares.
    std::vector<UIWareInfo> cargo(const char* tags = "") const {
        std::vector<UIWareInfo> result;
        auto* g = game();
        if (!valid() || !g) return result;
        uint32_t n = g->GetNumCargo(id_, tags);
        if (n == 0) return result;
        result.resize(n);
        uint32_t actual = g->GetCargo(result.data(), n, id_, tags);
        result.resize(actual);
        return result;
    }

    /// Resolve this station's buildstorage sub-entity (if any).
    /// Buildstorage is a child container that holds build-resource cargo
    /// during station construction. Returns 0 if the station has none.
    UniverseID buildstorage_id() const {
        return valid() ? entity::find_ancestor(id_, GameClass::Buildstorage) : 0;
    }

    /// People capacity (workforce target) for this station.
    /// @param macro_name Macro filter (e.g. a specific module), or empty for whole station.
    /// @param include_pilot Whether to include pilot capacity (for ships); false for stations.
    uint32_t people_capacity(const char* macro_name = "", bool include_pilot = false) const {
        auto* g = game();
        if (!valid() || !g) return 0;
        return g->GetPeopleCapacity(id_, macro_name, include_pilot);
    }

    /// Workforce influence info for a race (capacity / growth / current / target / change).
    /// Wraps GetContainerWorkforceInfluence.
    WorkforceInfluenceInfo workforce_info(const char* race) const {
        WorkforceInfluenceInfo info{};
        auto* g = game();
        if (!valid() || !g) return info;
        g->GetContainerWorkforceInfluence(&info, id_, race);
        return info;
    }
};

/// Lightweight typed handle for a station's buildstorage sub-entity.
/// Constructor validates the entity is actually a Buildstorage class.
/// Use Station::buildstorage_id() to obtain the ID first.
class Buildstorage {
    UniverseID id_;
    X4Component* comp_;
public:
    explicit Buildstorage(UniverseID id)
        : id_(id), comp_(entity::find_component(id)) {
        if (comp_ && !entity::is_a(comp_, GameClass::Buildstorage))
            comp_ = nullptr;
    }

    bool valid() const { return comp_ != nullptr; }
    UniverseID id() const { return id_; }

    /// Wares critical for this buildstorage (engine-canonical, includes
    /// whatever the current build queue needs as inputs).
    std::vector<std::string> critical_wares() const {
        std::vector<std::string> result;
        auto* g = game();
        if (!valid() || !g) return result;
        uint32_t n = g->GetNumContainerCriticalWares(id_);
        if (n == 0) return result;
        auto buf = std::vector<const char*>(n);
        uint32_t actual = g->GetContainerCriticalWares(buf.data(), n, id_);
        result.reserve(actual);
        for (uint32_t i = 0; i < actual; ++i) {
            if (buf[i]) result.emplace_back(buf[i]);
        }
        return result;
    }

    /// Build resources this buildstorage needs.
    std::vector<std::string> build_resources() const {
        std::vector<std::string> result;
        auto* g = game();
        if (!valid() || !g) return result;
        uint32_t n = g->GetNumContainerBuildResources(id_);
        if (n == 0) return result;
        auto buf = std::vector<const char*>(n);
        uint32_t actual = g->GetContainerBuildResources(buf.data(), n, id_);
        result.reserve(actual);
        for (uint32_t i = 0; i < actual; ++i) {
            if (buf[i]) result.emplace_back(buf[i]);
        }
        return result;
    }

    /// Cargo currently held by this buildstorage (per-ware amounts).
    std::vector<UIWareInfo> cargo(const char* tags = "") const {
        std::vector<UIWareInfo> result;
        auto* g = game();
        if (!valid() || !g) return result;
        uint32_t n = g->GetNumCargo(id_, tags);
        if (n == 0) return result;
        result.resize(n);
        uint32_t actual = g->GetCargo(result.data(), n, id_, tags);
        result.resize(actual);
        return result;
    }

    /// Per-ware consumption rate in the buildstorage (build draw only).
    double consumption(const char* ware_id, bool ignore_state = false) const {
        auto* g = game();
        if (!valid() || !g) return 0.0;
        return g->GetContainerWareConsumption(id_, ware_id, ignore_state);
    }
};

}} // namespace x4n::station
