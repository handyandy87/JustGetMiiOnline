#pragma once

#include <wups/config.h>

// What the plugin is set to do, kept in WUPS storage and edited from the config menu.
//
// Everything here is read once when the app starts, because the patches it decides go in before
// the app draws anything. Changing something in the menu takes effect the next time YouTube is
// launched, not while it is running.

// Which YouTube the console starts. The first three are the revival host's October 2013 build in
// one of its arrangements of the two screens. The shell reads -h5vcc-target-screen on an element
// and draws it on the screen that names, and the build ships one rule of its own: the player, the
// branding and the logo on both, everything else left to fall to the GamePad, which is what
// leaves the TV idle until a video plays.
//
// - 2012 (Launch) is SteelHybrid: the interface on the TV, the GamePad given to the video panel
// - 2013 (Standard) is Steel: the build's own arrangement, nothing added
// - 2013 (Mirrored Screen) is SteelDual: the interface on both screens, the keyboard kept to the
//   GamePad
// - 2016 (Standard) is the revival host's March 2016 build, in the one arrangement it shipped:
//   the interface on the GamePad and the watch screen on both
// - Current (Official) is whatever youtube.com serves the console itself, which the plugin keeps
//   out of but for the User-Agent
enum Version {
    VERSION_2012_LAUNCH = 0,
    VERSION_2013_STANDARD,
    VERSION_2013_DUAL,
    VERSION_2016,
    VERSION_CURRENT
};

// Reads what is stored, carrying a stored App and Screens over to a version the first time. Call
// from the plugin init hook, before anything asks for a value.
void settings_load();

// Builds the config menu. Call from the menu open hook.
bool settings_menu(WUPSConfigCategoryHandle root);

// Writes anything the menu changed. Call from the menu close hook.
void settings_save();

Version settings_version();
bool settings_trace();

// Whether the suggestion strip is turned on, which only the 2013 build takes.
bool settings_suggestions();

// Whether a video's comments can be opened on the GamePad, which the 2013 and 2016 builds take.
bool settings_comments();

// The name the page knows the 2013 build's arrangement by, which is what rides on the start url
// and the listener's config reply, or null for a version whose screens are its own.
const char *settings_screens_name();
