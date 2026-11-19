/**
 * Intel GPUs and the D3D12 render target cache (render_target_cache.cpp Initialize): whether depth/stencil resolve
 * clears are drawn instead of issued with ClearDepthStencilView, and which render target path is used. Pure, so the
 * port's host tests can pin it (wt-i21 tests/host/d3d12_intel_policy_host_test.cpp). Issue #21 (Intel UHD 770).
 */
#pragma once

#include <string_view>

namespace rex::graphics::d3d12::intel_policy {

enum class Path { kHostRenderTargets, kPixelShaderInterlock };

// force: d3d12_clear_depth_via_drawing (any GPU). intel_default: d3d12_clear_depth_via_drawing_intel. is_intel: the
// adapter's vendor is Intel. On Intel, ClearDepthStencilView with a rectangle was seen (UHD 770, 2026-11-18) to leave
// the second EDRAM tile's depth in place; drawing the clear does not.
constexpr bool DrawDepthClears(bool force, bool intel_default, bool is_intel) {
  return force || (is_intel && intel_default);
}

// path_setting: render_target_path_d3d12 ("rtv" or "rov" is the user's choice; any other text is automatic).
// Automatic is rov only on Intel when depth clears are not drawn (the old Intel workaround; rov is 1-2 FPS in menus
// there). rov is never chosen without rasterizer-ordered views.
constexpr Path ChoosePath(std::string_view path_setting, bool is_intel, bool rov_supported,
                          bool depth_clears_drawn) {
  Path path;
  if (path_setting == "rtv") {
    path = Path::kHostRenderTargets;
  } else if (path_setting == "rov") {
    path = Path::kPixelShaderInterlock;
  } else {
    path = (is_intel && !depth_clears_drawn) ? Path::kPixelShaderInterlock : Path::kHostRenderTargets;
  }
  if (path == Path::kPixelShaderInterlock && !rov_supported) {
    path = Path::kHostRenderTargets;
  }
  return path;
}

}  // namespace rex::graphics::d3d12::intel_policy
