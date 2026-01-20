#include "StarLuaRoot.hpp"
#include "StarAssets.hpp"
#include "StarAssetPath.hpp"
#include "StarJson.hpp"
#include "StarUuid.hpp"
#include "StarUtilityLuaBindings.hpp"
#include "StarRootLuaBindings.hpp"
#include <functional>

namespace Star {

namespace {

// Plugin System
// =============
// The plugin system allows Lua scripts to be extended with additional functionality
// via plugin files. Plugins are loaded before the main script, allowing them to register
// hooks (via hooks.before() and hooks.after()) that can intercept and modify function calls in the main script.
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
// 3. hooks.applyAll() is called to apply any registered hooks
//
// This order ensures plugins can register hooks before the main script's init() function runs.

constexpr char const* HooksApplyAllPath = "hooks.applyAll";

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
    } catch (std::exception const& e) {
      Logger::error("Failed to load plugin '{}' (path: '{}') for script '{}': {}", plugin.id, plugin.path, assetPath, e.what());
    }
  }
}

} // namespace

LuaRoot::LuaRoot() {
  auto& root = Root::singleton();
  m_scriptCache = make_shared<ScriptCache>();

  restart();

  m_rootReloadListener = make_shared<CallbackListener>([cache = m_scriptCache]() {
      cache->clear();
    });
  root.registerReloadListener(m_rootReloadListener);

  m_storageDirectory = root.toStoragePath("lua");
}

LuaRoot::~LuaRoot() {
  shutdown();
}

void LuaRoot::loadScript(String const& assetPath) {
  m_scriptCache->loadScript(*m_luaEngine, assetPath);
}

bool LuaRoot::scriptLoaded(String const& assetPath) const {
  return m_scriptCache->scriptLoaded(assetPath);
}

void LuaRoot::unloadScript(String const& assetPath) {
  m_scriptCache->unloadScript(assetPath);
}

void LuaRoot::restart() {
  shutdown();

  auto& root = Root::singleton();

  m_luaEngine = LuaEngine::create(root.configuration()->get("safeScripts").toBool());

  m_luaEngine->setRecursionLimit(root.configuration()->get("scriptRecursionLimit").toUInt());
  m_luaEngine->setInstructionLimit(root.configuration()->get("scriptInstructionLimit").toUInt());
  m_luaEngine->setProfilingEnabled(root.configuration()->get("scriptProfilingEnabled").toBool());
  m_luaEngine->setInstructionMeasureInterval(root.configuration()->get("scriptInstructionMeasureInterval").toUInt());
}

void LuaRoot::shutdown() {
  clearScriptCache();

  if (!m_luaEngine)
    return;

  auto profile = m_luaEngine->getProfile();
  if (!profile.empty()) {
    profile.sort([](auto const& a, auto const& b) {
        return a.totalTime > b.totalTime;
      });

    std::function<Json (LuaProfileEntry const&)> jsonFromProfileEntry = [&](LuaProfileEntry const& entry) -> Json {
        JsonObject profile;
        profile.set("function", entry.name.value("<function>"));
        profile.set("scope", entry.nameScope.value("?"));
        profile.set("source", strf("{}:{}", entry.source, entry.sourceLine));
        profile.set("self", entry.selfTime);
        profile.set("total", entry.totalTime);
        List<LuaProfileEntry> calls;
        for (auto p : entry.calls)
          calls.append(*p.second);
        profile.set("calls", calls.sorted([](auto const& a, auto const& b) { return a.totalTime > b.totalTime; }).transformed(jsonFromProfileEntry));
        return profile;
      };

    String profileSummary = Json(profile.transformed(jsonFromProfileEntry)).repr(1);

    if (!File::isDirectory(m_storageDirectory)) {
      Logger::info("Creating lua storage directory");
      File::makeDirectory(m_storageDirectory);
    }

    String filename = strf("{}.luaprofile", Time::printCurrentDateAndTime("<year>-<month>-<day>-<hours>-<minutes>-<seconds>-<millis>"));
    String path = File::relativeTo(m_storageDirectory, filename);
    Logger::info("Writing lua profile {}", filename);
    File::writeFile(profileSummary, path);
  }

  m_luaEngine.reset();
}

LuaContext LuaRoot::createContext(String const& script) {
  return createContext(StringList{script});
}

LuaContext LuaRoot::createContext(StringList const& scriptPaths) {
  auto newContext = m_luaEngine->createContext();

  // Apply callbacks BEFORE setting up require function and loading scripts
  // This ensures callbacks are available when scripts load and when plugins load via require
  for (auto const& callbackPair : m_luaCallbacks)
    newContext.setCallbacks(callbackPair.first, callbackPair.second);

  auto cache = m_scriptCache;
  newContext.setRequireFunction([cache](LuaContext& context, LuaString const& module) {
    if (!context.get("_SBLOADED").is<LuaTable>())
      context.set("_SBLOADED", context.createTable());
    auto t = context.get<LuaTable>("_SBLOADED");
    if (!t.contains(module)) {
      t.set(module, true);
      cache->loadContextScript(context, module.toString());
    }
  });

  auto assets = Root::singleton().assets();

  for (auto const& scriptPath : scriptPaths) {
    if (assets->assetExists(scriptPath))
      cache->loadContextScript(newContext, scriptPath);
    else
      Logger::error("Script '{}' does not exist", scriptPath);
  }

  return newContext;
}

void LuaRoot::collectGarbage(Maybe<unsigned> steps) {
  if (m_luaEngine)
    m_luaEngine->collectGarbage(steps);
}

void LuaRoot::setAutoGarbageCollection(bool autoGarbageColleciton) {
  if (m_luaEngine)
    m_luaEngine->setAutoGarbageCollection(autoGarbageColleciton);
}

void LuaRoot::tuneAutoGarbageCollection(float pause, float stepMultiplier) {
  if (m_luaEngine)
    m_luaEngine->tuneAutoGarbageCollection(pause, stepMultiplier);
}

size_t LuaRoot::luaMemoryUsage() const {
  return m_luaEngine ? m_luaEngine->memoryUsage() : 0;
}

size_t LuaRoot::scriptCacheMemoryUsage() const {
  return m_luaEngine ? m_scriptCache->memoryUsage() : 0;
}

void LuaRoot::clearScriptCache() const {
  return m_scriptCache->clear();
}

void LuaRoot::addCallbacks(String const& groupName, LuaCallbacks const& callbacks) {
  m_luaCallbacks[groupName] = callbacks;
}

LuaEngine& LuaRoot::luaEngine() const {
  return *m_luaEngine;
}

void LuaRoot::ScriptCache::loadScript(LuaEngine& engine, String const& assetPath) {
  auto assets = Root::singleton().assets();
  RecursiveMutexLocker locker(mutex);

  // Load main script
  scripts[assetPath] = engine.compile(*assets->bytes(assetPath), assetPath);
}

bool LuaRoot::ScriptCache::scriptLoaded(String const& assetPath) const {
  RecursiveMutexLocker locker(mutex);
  return scripts.contains(assetPath);
}

void LuaRoot::ScriptCache::unloadScript(String const& assetPath) {
  RecursiveMutexLocker locker(mutex);
  scripts.remove(assetPath);
}

void LuaRoot::ScriptCache::clear() {
  RecursiveMutexLocker locker(mutex);
  scripts.clear();
}

void LuaRoot::ScriptCache::loadContextScript(LuaContext& context, String const& assetPath) {
  RecursiveMutexLocker locker(mutex);
  if (!scriptLoaded(assetPath))
    loadScript(context.engine(), assetPath);

  // Load plugins BEFORE the main script. This ordering is critical because:
  // 1. Plugins need to register hooks (via hooks.before() and hooks.after()) before the main script's init() runs
  // 2. The main script may call functions that plugins have hooked
  // 3. Callbacks (sb, root) are already applied by createContext() before this point
  loadPluginsForScript(context, assetPath);

  // Load the main script after plugins have registered their hooks
  context.load(scripts.get(assetPath));

  // Apply all registered hooks now that both plugins and main script are loaded.
  // The hooks system (defined in star_hooks.lua, injected per-context) allows plugins to wrap functions in the main script.
  // hooks.applyAll() iterates through all registered hooks and wraps the target functions.
  // Contract: hooks.applyAll() must exist if any hooks were registered, but it's safe to call
  // even if no hooks exist (the function will simply do nothing).
  if (context.containsPath(HooksApplyAllPath)) {
    try {
      context.invokePath(HooksApplyAllPath);
    } catch (std::exception const& e) {
      Logger::error("Failed to apply hooks for script '{}': {}", assetPath, e.what());
      // Continue execution even if hooks fail - the script should still work
    }
  }
}

size_t LuaRoot::ScriptCache::memoryUsage() const {
  RecursiveMutexLocker locker(mutex);
  size_t total = 0;
  for (auto const& p : scripts)
    total += p.second.size();
  return total;
}

}
