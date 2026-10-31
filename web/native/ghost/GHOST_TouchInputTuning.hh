/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Ported from the 3D Modeling Thingy iOS port (Shlok-Bhakta/3d-modeling-thingy-ios,
 * intern/ghost/intern/GHOST_IOSInputTuning.hh); keep the two in sync. */

#pragma once

/** Physical-input values expected to need tuning after testing on touch devices. */
namespace GHOST_TouchInputTuning {

inline constexpr double pointer_acceleration_start_points_per_second = 120.0;
inline constexpr double pointer_acceleration_full_points_per_second = 1100.0;
inline constexpr double pointer_min_multiplier = 1.0;
inline constexpr double pointer_max_multiplier = 4.0;
inline constexpr bool pointer_always_wrap = true;

inline constexpr double two_finger_right_click_hold_seconds = 0.30;
inline constexpr double two_finger_right_click_slop_points = 14.0;

/* Gesture recognition. UIKit supplies these through its recognizers; the browser
 * port recognizes raw pointers itself, using UIKit's usual values. */
inline constexpr double tap_max_seconds = 0.30;
inline constexpr double tap_slop_points = 10.0;
inline constexpr double multi_tap_interval_seconds = 0.30;
inline constexpr double tap_drag_hold_seconds = 0.12;
inline constexpr double multi_finger_tap_max_seconds = 0.35;

}  // namespace GHOST_TouchInputTuning
