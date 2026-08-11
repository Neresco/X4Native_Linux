// ---------------------------------------------------------------------------
// x4n_container.h — Container typed wrapper (station / ship base class)
// ---------------------------------------------------------------------------
// Part of the X4Native SDK. Included by x4native.h.
//
// Provides:
//   x4n::container::Container — lightweight handle for Container-class entities
//                                (stations, ships — anything that holds trade
//                                wares and carries a global price factor)
//
// Usage:
//   x4n::container::Container cont(station_id);
//   if (cont.valid()) {
//       cont.set_global_price_factor(0.85f);  // bypass FFI player-owned gate
//   }
//
// Thin handle: UniverseID + cached X4Component*. No heap allocation.
// All functions require on_game_loaded to have fired.
// ---------------------------------------------------------------------------
#pragma once

#include "x4n_core.h"
#include "x4n_entity.h"
#include <cstdint>

namespace x4n { namespace container {

/// Lightweight typed handle for a Container-class entity (station / ship).
/// Constructor validates the entity IS-A Container via vtable IsOrDerivedFromClassID.
class Container {
    UniverseID id_;
    X4Component* comp_;
public:
    explicit Container(UniverseID id)
        : id_(id), comp_(entity::find_component(id)) {
        if (comp_ && !entity::is_a(comp_, GameClass::Container))
            comp_ = nullptr;
    }

    bool valid() const { return comp_ != nullptr; }
    UniverseID id() const { return id_; }

    /// Read the station-global price factor directly from the Container field
    /// at +0x7A0. Returns 1.0f if the component is invalid (matches engine
    /// default when no override is set); returns -1.0f only if the field
    /// itself carries the "restore default" sentinel previously written.
    ///
    /// Works for NPC-owned containers (read path is not gated).
    float get_global_price_factor() const {
        if (!valid()) return 1.0f;
        return *reinterpret_cast<const float*>(
            reinterpret_cast<const uint8_t*>(comp_) + X4_CONTAINER_PRICE_FACTOR_OFFSET);
    }

    /// Set the station-global price factor via direct field write, bypassing
    /// the FFI's player-ownership gate. Works on NPC-owned containers.
    ///
    /// Clamp matches the FFI at `SetContainerGlobalPriceFactor` (0x1401B73D0):
    /// negative values write the -1.0f "restore default" sentinel; positive
    /// values are clamped to [0.0, 1.0] before the store. The engine reads
    /// this field when computing trade offers, so matching the FFI's clamp
    /// preserves identical downstream semantics.
    ///
    /// Precedent: `x4n::module::Module::is_paused()` reads the Production
    /// `paused_since` field directly, bypassing the symmetric read-side gate
    /// (both FFIs share the "not player-owned! Aborting call." sentinel).
    ///
    /// See docs/rev/STATE_MUTATION.md for the direct-mutation safety model.
    void set_global_price_factor(float factor) {
        if (!valid()) return;
        float stored;
        if (factor < 0.0f) {
            stored = -1.0f;  // engine sentinel: "no override"
        } else if (factor > 1.0f) {
            stored = 1.0f;
        } else {
            stored = factor;
        }
        *reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(comp_) + X4_CONTAINER_PRICE_FACTOR_OFFSET) = stored;
    }
};

}} // namespace x4n::container
