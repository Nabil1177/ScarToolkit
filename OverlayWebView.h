#pragma once
#include <string>

// Show (or create) the transparent overlay window that renders overlay.html with the given AoE4World profile id.
// Returns true if WebView2 could be created (overlay shown or will show shortly).
// If this returns false, you can fall back to your browser-based LaunchOverlayHtml().
bool OV_Show(const std::string& pid, int pollMs = 2000);

// Hide the overlay window (does not destroy it).
void OV_Hide();

// Optionally move/resize the overlay window (screen coordinates).
// Example: OV_SetBounds(60, 80, 1100, 280);
void OV_SetBounds(int x, int y, int w, int h);

// Is the overlay window currently visible?
bool OV_IsVisible();

// Is WebView2 available/initialized (i.e., overlay capable)?
bool OV_IsAvailable();
