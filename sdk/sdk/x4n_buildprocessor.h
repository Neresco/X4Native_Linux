// ---------------------------------------------------------------------------
// x4n_buildprocessor.h — Buildprocessor typed wrapper
// ---------------------------------------------------------------------------
// Part of the X4Native SDK. Included by x4native.h.
//
// Provides:
//   x4n::buildprocessor::Buildprocessor — lightweight handle for the sub-entity
//     that a Construction Vehicle attaches to a BuildTask while actively
//     constructing. Exposes the engine's `started_at` / `paused_at` timestamps
//     and the build-stage counters via direct-field reads (not gated).
//
// Usage:
//   // From a BuildTaskInfo, `buildercomponent` is the Buildprocessor's UID.
//   x4n::buildprocessor::Buildprocessor bp(task.buildercomponent);
//   if (bp.valid()) {
//       double age_s = game_time - bp.started_at();
//       if (age_s > 15 * 3600.0) { /* build stuck > 15h */ }
//   }
//
// Thin handle: UniverseID + cached X4Component*. No heap allocation.
// All functions require on_game_loaded to have fired.
// ---------------------------------------------------------------------------
#pragma once

#include "x4n_core.h"
#include "x4n_entity.h"
#include <cstddef>
#include <cstdint>

namespace x4n { namespace buildprocessor {

/// Lightweight typed handle for a Buildprocessor (GameClass 10).
/// Constructor validates the entity is actually a Buildprocessor.
///
/// Lifecycle: created by the engine when a Construction Vehicle engages a
/// BuildTask; destroyed when the CV disengages (task complete, aborted, or
/// abandonbuildtime elapses at ~1h of resource-wait). `started_at` is stamped
/// on attach and reset to -1.0 on detach — so its value is "current phase
/// start," not "task queue time."
class Buildprocessor {
    UniverseID id_;
    X4Component* comp_;
public:
    explicit Buildprocessor(UniverseID id)
        : id_(id), comp_(entity::find_component(id)) {
        if (comp_ && !entity::is_a(comp_, GameClass::Buildprocessor))
            comp_ = nullptr;
    }

    bool valid() const { return comp_ != nullptr; }
    UniverseID id() const { return id_; }

    /// `player.age` seconds at which the CV began processing the current
    /// BuildTask. Returns `-1.0` sentinel if the buildprocessor is invalid or
    /// currently idle (no task being processed). Works for NPC-owned CVs —
    /// read path is a direct memory load, not gated.
    ///
    /// Semantic match for vanilla's `$StartBuildTime` on `$EconomyActionReports`
    /// (set in `factionlogic_economy.xml` when construction began). Subtract
    /// from current `player.age` for elapsed-since-start.
    ///
    /// **Caveat:** resets when the CV disengages. A build with cycling CVs
    /// (attach → abandon → attach) reports the MOST RECENT attach time, not
    /// the original queue time. For "has this task existed > Xh" semantics
    /// you need your own timer — this field only measures active CV-work time.
    ///
    /// See docs/rev/PRODUCTION_MODULES.md (to be extended) for binary traces.
    double started_at() const { return read_double_or(X4_BUILDPROCESSOR_STARTED_AT_OFFSET, -1.0); }

    /// True iff this buildprocessor is currently processing a build.
    /// Equivalent to `started_at() > -0.9999` (sentinel check matching the
    /// engine's own test — see binary sub_14034FE00).
    bool is_processing() const {
        return started_at() > -0.9999;
    }

    /// `player.age` at which the current build phase was paused; `-1.0` if not
    /// currently paused (or not processing at all). Same -1.0 sentinel idiom
    /// as `paused_since()` on Production modules. The engine's
    /// GetBuildProcessorEstimatedTimeLeft uses this to subtract pause time
    /// from elapsed-since-start.
    double paused_at() const { return read_double_or(X4_BUILDPROCESSOR_PAUSED_AT_OFFSET, -1.0); }

    /// 1-indexed current build stage (0 if not processing). Stage count
    /// comes from the macro's build plan — a single station module may be
    /// composed of multiple stages. Paired with `total_stages()` yields
    /// overall progress %.
    int32_t current_stage() const { return read_int32_or(X4_BUILDPROCESSOR_CURRENT_STAGE_OFFSET, 0); }

    /// Total build stages for the current task's macro (0 if not processing).
    /// @see current_stage
    int32_t total_stages() const { return read_int32_or(X4_BUILDPROCESSOR_TOTAL_STAGES_OFFSET, 0); }

private:
    // Direct-field accessors. All fields are plain scalars at known offsets
    // in the Buildprocessor component, so we share one read path per type.
    double read_double_or(std::ptrdiff_t offset, double fallback) const {
        if (!valid()) return fallback;
        return *reinterpret_cast<const double*>(
            reinterpret_cast<const uint8_t*>(comp_) + offset);
    }
    int32_t read_int32_or(std::ptrdiff_t offset, int32_t fallback) const {
        if (!valid()) return fallback;
        return *reinterpret_cast<const int32_t*>(
            reinterpret_cast<const uint8_t*>(comp_) + offset);
    }
};

}} // namespace x4n::buildprocessor
