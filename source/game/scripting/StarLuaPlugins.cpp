#include "StarLuaPlugins.hpp"
#include "StarAssets.hpp"
#include "StarAssetPath.hpp"
#include "StarJson.hpp"
#include "StarUuid.hpp"
#include "StarUtilityLuaBindings.hpp"
#include "StarRootLuaBindings.hpp"
#include "StarRoot.hpp"
#include <functional>

namespace Star {
namespace LuaPlugins {

namespace {

struct PluginEntry {
  String id;
  String path;
  String pluginsFilePath;  // Path to the .plugins file that defined this plugin
  StringList before;
  StringList after;
};

String resolvePluginPath(String const& pluginPath, String const& basePath) {
  return AssetPath::relativeTo(basePath, pluginPath);
}

List<PluginEntry> parsePlugins(Json const& pluginsJson, String const& pluginsFilePath) {
  List<PluginEntry> plugins;
  if (!pluginsJson.isType(Json::Type::Array))
    throw JsonParsingException::format("Plugins file '{}' is not a JSON array", pluginsFilePath);

  for (auto const& entry : pluginsJson.toArray()) {
    if (!entry.isType(Json::Type::Object))
      throw JsonParsingException::format("Plugins file '{}' has non-object entry", pluginsFilePath);

    auto obj = entry.toObject();

    PluginEntry plugin;
    if (auto id = obj.maybe("id"))
      plugin.id = id->toString();
    else
      plugin.id = Uuid().hex();

    if (auto path = obj.maybe("path"))
      plugin.path = path->toString();
    else
      throw JsonParsingException::format("Plugins file '{}' entry missing required 'path'", pluginsFilePath);

    if (auto before = obj.maybe("before")) {
      if (!before->isType(Json::Type::Array))
        throw JsonParsingException::format("Plugins file '{}' entry 'before' must be array", pluginsFilePath);
      for (auto const& b : before->toArray())
        plugin.before.append(b.toString());
    }

    if (auto after = obj.maybe("after")) {
      if (!after->isType(Json::Type::Array))
        throw JsonParsingException::format("Plugins file '{}' entry 'after' must be array", pluginsFilePath);
      for (auto const& a : after->toArray())
        plugin.after.append(a.toString());
    }

    plugin.pluginsFilePath = pluginsFilePath;
    plugins.append(std::move(plugin));
  }

  return plugins;
}

// Resolves plugin dependencies using topological sort (Kahn's algorithm).
// Dependencies are specified via "before" (plugin must load before target) and
// "after" (plugin must load after target) fields in the plugin JSON.
// Returns plugins in dependency order, or original order if cycle detected.
List<PluginEntry> resolvePluginDependencies(List<PluginEntry> const& plugins) {
  // Build lookup by id and path for reference matching
  HashMap<String, size_t> idIndex;
  HashMap<String, size_t> pathIndex;
  for (size_t i = 0; i < plugins.size(); ++i) {
    idIndex[plugins[i].id] = i;
    pathIndex[plugins[i].path] = i;
  }

  // Validate uniqueness
  for (size_t i = 0; i < plugins.size(); ++i) {
    for (size_t j = i + 1; j < plugins.size(); ++j) {
      if (plugins[i].id == plugins[j].id) {
        Logger::warn("Duplicate plugin ID '{}' found at indices {} and {}", plugins[i].id, i, j);
      }
      if (plugins[i].path == plugins[j].path) {
        Logger::warn("Duplicate plugin path '{}' found at indices {} and {}", plugins[i].path, i, j);
      }
    }
  }

  // Build adjacency list for efficient topological sort (O(E+V) instead of O(E×V))
  List<List<size_t>> adjacencyList(plugins.size());
  auto addEdge = [&](size_t from, size_t to) {
    if (from == to)
      return;
    adjacencyList[from].append(to);
  };

  for (size_t i = 0; i < plugins.size(); ++i) {
    auto const& plugin = plugins[i];
    // before: plugin -> target (plugin must load before target)
    for (auto const& beforeId : plugin.before) {
      if (auto it = idIndex.maybe(beforeId))
        addEdge(i, *it);
      else if (auto pt = pathIndex.maybe(beforeId))
        addEdge(i, *pt);
      else
        Logger::warn("Plugin '{}' references missing 'before' dependency '{}'", plugin.id, beforeId);
    }
    // after: target -> plugin (plugin must load after target)
    for (auto const& afterId : plugin.after) {
      if (auto it = idIndex.maybe(afterId))
        addEdge(*it, i);
      else if (auto pt = pathIndex.maybe(afterId))
        addEdge(*pt, i);
      else
        Logger::warn("Plugin '{}' references missing 'after' dependency '{}'", plugin.id, afterId);
    }
  }

  // Topological sort (Kahn's algorithm) using adjacency list
  List<size_t> indegree(plugins.size(), 0);
  for (size_t i = 0; i < adjacencyList.size(); ++i) {
    for (auto to : adjacencyList[i])
      ++indegree[to];
  }

  List<size_t> queue;
  for (size_t i = 0; i < plugins.size(); ++i) {
    if (indegree[i] == 0)
      queue.append(i);
  }

  List<size_t> order;
  while (!queue.empty()) {
    size_t n = queue.takeLast();
    order.append(n);
    // Only iterate over outgoing edges from n (O(E) total instead of O(E×V))
    for (auto to : adjacencyList[n]) {
      if (--indegree[to] == 0)
        queue.append(to);
    }
  }

  // If cycle detected, use DFS to identify the cycle
  if (order.size() != plugins.size()) {
    // Find nodes still in the graph (have non-zero indegree)
    List<size_t> cycleNodes;
    for (size_t i = 0; i < plugins.size(); ++i) {
      if (indegree[i] > 0)
        cycleNodes.append(i);
    }

    // Use DFS to find a cycle path
    // Try to find cycle starting from any node with remaining edges
    for (auto startNode : cycleNodes) {
      List<bool> visited;
      visited.resize(plugins.size());
      List<bool> recStack;
      recStack.resize(plugins.size());
      List<size_t> cyclePath;
      size_t cycleStart = 0;
      
      std::function<bool(size_t)> findCycle = [&](size_t node) -> bool {
        if (recStack[node]) {
          // Found a cycle - find where this node appears in the path
          for (size_t i = 0; i < cyclePath.size(); ++i) {
            if (cyclePath[i] == node) {
              cycleStart = i;
              cyclePath.append(node);  // Close the cycle
              return true;
            }
          }
          return true;
        }
        if (visited[node])
          return false;
        
        visited[node] = true;
        recStack[node] = true;
        cyclePath.append(node);
        
        for (auto to : adjacencyList[node]) {
          if (findCycle(to)) {
            return true;
          }
        }
        
        cyclePath.removeLast();
        recStack[node] = false;
        return false;
      };

      if (findCycle(startNode)) {
        // Extract just the cycle portion from cyclePath
        List<size_t> cycle;
        for (size_t i = cycleStart; i < cyclePath.size(); ++i) {
          cycle.append(cyclePath[i]);
        }
        // Build cycle description
        String cycleDesc;
        for (size_t idx = 0; idx < cycle.size(); ++idx) {
          if (idx > 0)
            cycleDesc += " -> ";
          cycleDesc += strf("'{}'", plugins[cycle[idx]].id);
        }
        Logger::error("Plugin dependency cycle detected: {}. Using original plugin order.", cycleDesc);
        return plugins;
      }
    }
    
    // Fallback if cycle detection failed
    Logger::error("Plugin dependency cycle detected (could not identify path). Using original plugin order.");
    return plugins;
  }

  List<PluginEntry> resolved;
  for (auto idx : order)
    resolved.append(plugins[idx]);
  return resolved;
}

} // namespace

void loadPluginsForScript(LuaContext& context, String const& assetPath) {
  auto assets = Root::singleton().assets();
  auto baseAsset = AssetPath::split(assetPath).basePath;
  String pluginsPath = baseAsset + ".plugins";

  Json pluginsJson;
  try {
    if (assets->assetExists(pluginsPath)) {
      pluginsJson = assets->json(pluginsPath);
      Logger::debug("loadPluginsForScript '{}': read plugins file '{}', type: {}, size: {}", 
                   assetPath, pluginsPath, 
                   pluginsJson.isType(Json::Type::Array) ? "Array" : 
                   pluginsJson.isType(Json::Type::Object) ? "Object" : "Other",
                   pluginsJson.isType(Json::Type::Array) ? pluginsJson.toArray().size() : 0);
    } else {
      pluginsJson = JsonArray();
      Logger::debug("loadPluginsForScript '{}': plugins file '{}' does not exist, using empty array", assetPath, pluginsPath);
    }
  } catch (std::exception const& e) {
    Logger::error("Failed to read plugins file '{}' for script '{}': {}", pluginsPath, assetPath, e.what());
    return;
  }

  List<PluginEntry> plugins;
  try {
    plugins = parsePlugins(pluginsJson, pluginsPath);
    Logger::debug("loadPluginsForScript '{}': parsed {} plugins", assetPath, plugins.size());
  } catch (std::exception const& e) {
    Logger::error("Failed to parse plugins file '{}' for script '{}': {}", pluginsPath, assetPath, e.what());
    return;
  }

  if (plugins.empty()) {
    Logger::debug("loadPluginsForScript '{}': no plugins to load", assetPath);
    return;
  }

  // Resolve relative paths to absolute asset paths
  // Create mutable copy to avoid const correctness issues
  String baseDir = AssetPath::directory(pluginsPath);
  List<PluginEntry> resolvedPlugins;
  resolvedPlugins.reserve(plugins.size());
  for (auto const& plugin : plugins) {
    PluginEntry resolved = plugin;
    resolved.path = resolvePluginPath(plugin.path, baseDir);
    resolvedPlugins.append(std::move(resolved));
  }

  auto orderedPlugins = resolvePluginDependencies(resolvedPlugins);

  for (auto const& plugin : orderedPlugins) {
    Logger::debug("loadPluginsForScript '{}': loading plugin '{}' from '{}'", assetPath, plugin.id, plugin.path);
    
    // Ensure callbacks are available for plugins (they should be, but double-check)
    if (!context.get("sb").is<LuaTable>()) {
      Logger::debug("loadPluginsForScript '{}': sb not available, applying callbacks", assetPath);
      context.setCallbacks("sb", LuaBindings::makeUtilityCallbacks());
      context.setCallbacks("root", LuaBindings::makeRootCallbacks());
    }
    
    if (!assets->assetExists(plugin.path)) {
      Logger::error("Plugin script '{}' (id '{}') not found for script '{}'", plugin.path, plugin.id, assetPath);
      continue;
    }

    try {
      auto bytes = assets->bytes(plugin.path);
      auto compiled = context.engine().compile(*bytes, plugin.path);
      Logger::debug("loadPluginsForScript '{}': compiling and loading plugin '{}'", assetPath, plugin.id);
      context.load(compiled);
      Logger::debug("loadPluginsForScript '{}': successfully loaded plugin '{}'", assetPath, plugin.id);
    } catch (LuaException const& e) {
      Logger::error("LuaException while loading plugin '{}' (path: '{}') for script '{}': {}", plugin.id, plugin.path, assetPath, e.what());
      // Continue loading other plugins even if one fails
    } catch (std::exception const& e) {
      Logger::error("Exception while loading plugin '{}' (path: '{}') for script '{}': {}", plugin.id, plugin.path, assetPath, e.what());
      // Continue loading other plugins even if one fails
    }
  }
}

} // namespace LuaPlugins
} // namespace Star
