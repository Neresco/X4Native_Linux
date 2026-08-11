// ---------------------------------------------------------------------------
// x4n_module.h — Station Module typed wrapper
// ---------------------------------------------------------------------------
// Part of the X4Native SDK. Included by x4native.h.
//
// Provides:
//   x4n::module::Module — lightweight handle for station modules
//                          (production, processing, storage, etc.)
//
// Usage:
//   x4n::module::Module mod(module_id);
//   if (mod.is_production() && mod.ware_id() == "energycells") {
//       mod.set_paused(true);
//   }
//
// Thin handle: UniverseID + cached X4Component*. No heap allocation.
// All functions require on_game_loaded to have fired.
// ---------------------------------------------------------------------------
#pragma once

#include "x4n_core.h"
#include "x4n_entity.h"
#include <cstdint>
#include <string>

namespace x4n { namespace module {

/// Lightweight typed handle for a station module (production, processing, etc.).
/// Constructor validates the entity is actually a Module class.
class Module {
    UniverseID id_;
    X4Component* comp_;
public:
    explicit Module(UniverseID id)
        : id_(id), comp_(entity::find_component(id)) {
        if (comp_ && !entity::is_a(comp_, GameClass::Module))
            comp_ = nullptr;
    }

    bool valid() const { return comp_ != nullptr; }
    UniverseID id() const { return id_; }

    bool is_production() const {
        return valid() && entity::is_a(comp_, GameClass::Production);
    }
    bool is_processing() const {
        return valid() && entity::is_a(comp_, GameClass::Processingmodule);
    }
    bool is_habitation() const {
        return valid() && entity::is_a(comp_, GameClass::Habitation);
    }
    bool is_buildmodule() const {
        return valid() && entity::is_a(comp_, GameClass::Buildmodule);
    }
    bool is_storage() const {
        return valid() && entity::is_a(comp_, GameClass::Storage);
    }
    bool is_defence() const {
        return valid() && entity::is_a(comp_, GameClass::Defencemodule);
    }
    bool is_connection() const {
        return valid() && entity::is_a(comp_, GameClass::Connectionmodule);
    }
    bool is_operational() const {
        auto* g = game();
        return valid() && g && g->IsComponentOperational(id_);
    }

    const char* macro() const {
        return valid() ? entity::get_component_macro(comp_) : nullptr;
    }

    /// Extract ware_id from macro: "prod_gen_energycells_macro" -> "energycells".
    /// Returns empty string if macro doesn't match the production naming pattern.
    std::string ware_id() const {
        const char* m = macro();
        if (!m) return {};
        std::string s(m);
        if (s.size() < 12) return {};
        if (s.substr(s.size() - 6) != "_macro") return {};
        if (s.substr(0, 5) != "prod_") return {};
        auto pos = s.find('_', 5);
        if (pos == std::string::npos) return {};
        return s.substr(pos + 1, s.size() - 6 - pos - 1);
    }

    /// Pause or resume this module via the game's FFI.
    /// @note FFI checks player ownership — may silently fail on NPC modules.
    void set_paused(bool p) {
        auto* g = game();
        if (!valid() || !g) return;
        if (is_production())       g->PauseProductionModule(id_, p);
        else if (is_processing())  g->PauseProcessingModule(id_, p);
    }

    /// True iff this module has been manually paused.
    ///
    /// Handles both Production (class 78, paused_since sentinel at +0x398) and
    /// Processingmodule (class 77, pause byte at +0x3B8). For Production, reads the
    /// `paused_since` timestamp which is the game engine's authoritative pause check
    /// (not the redundant +0x3F4 flag byte). For Processingmodule, reads its pause byte
    /// directly — no timestamp exists on that class.
    ///
    /// See docs/rev/PRODUCTION_MODULES.md §5.3.
    /// Works for NPC-owned modules (read path is not gated; only the FFI write is).
    bool is_paused() const {
        if (!valid()) return false;
        if (entity::is_a(comp_, GameClass::Production)) {
            double t = *reinterpret_cast<const double*>(
                reinterpret_cast<const uint8_t*>(comp_) + X4_PRODUCTION_PAUSED_SINCE_OFFSET);
            return t > -0.9999;
        }
        if (entity::is_a(comp_, GameClass::Processingmodule)) {
            return *(reinterpret_cast<const uint8_t*>(comp_)
                      + X4_PROCESSINGMODULE_PAUSED_FLAG_OFFSET) != 0;
        }
        return false;
    }

    /// `player.age` seconds at which this Production module was paused; returns -1.0
    /// if not currently paused or if this isn't a Production module.
    ///
    /// **Production-only.** Processingmodule (class 77) has no timestamp field — its
    /// pause/resume handlers never stamp a "since" value (confirmed by decompiling
    /// sub_14053C4A0 / sub_14053C560). If you need to track pause duration for a
    /// Processingmodule, you must maintain state outside the game (e.g., observe
    /// is_paused() transitions across polls).
    ///
    /// Subtract from current `player.age` to get seconds-paused for Production modules.
    /// Save-persistent — the timestamp survives save/load cycles.
    ///
    /// See docs/rev/PRODUCTION_MODULES.md §5.3.
    double paused_since() const {
        if (!is_production()) return -1.0;
        return *reinterpret_cast<const double*>(
            reinterpret_cast<const uint8_t*>(comp_) + X4_PRODUCTION_PAUSED_SINCE_OFFSET);
    }
};

}} // namespace x4n::module
