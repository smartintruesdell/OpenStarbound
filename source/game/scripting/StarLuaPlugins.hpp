#pragma once

#include "StarLua.hpp"
#include "StarJson.hpp"

namespace Star {

// Plugin System
// =============
// The plugin system allows Lua scripts to be extended with additional functionality
// via plugin files. Plugins are loaded before the main script.
//
// Plugin Configuration Format:
// ----------------------------
// Plugins are configured via a JSON file named <scriptname>.plugins (e.g., 
// "playermechdeployment.lua.plugins"). The file must contain a JSON array of plugin objects:
//
// [
//   {
//     "id": "plugin_unique_id",           // Optional: auto-generated UUID if not provided
//     "path": "/path/to/plugin.lua",        // Required: path to plugin script (relative to .plugins file)
//     "before": ["other_plugin_id"],       // Optional: array of plugin IDs/paths that this plugin must load before
//     "after": ["other_plugin_id"]        // Optional: array of plugin IDs/paths that this plugin must load after
//   }
// ]
//
// Dependency Resolution:
// ---------------------
// Plugins are loaded in dependency order using topological sort. If plugin A has "before": ["B"],
// then A will load before B. If plugin C has "after": ["B"], then C will load after B.
// Circular dependencies are detected and logged, with the system falling back to original order.
//
// Loading Order:
// --------------
// 1. Plugins are loaded first (in dependency order)
// 2. Main script is loaded

namespace LuaPlugins {

// Loads all plugins for a given script into the provided Lua context.
// Plugins are loaded from a .plugins file (e.g., "script.lua.plugins" for "script.lua").
// Plugins are loaded in dependency order before the main script executes.
void loadPluginsForScript(LuaContext& context, String const& assetPath);

} // namespace LuaPlugins
} // namespace Star
