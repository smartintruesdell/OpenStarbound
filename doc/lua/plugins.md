# Lua Plugin System

The plugin system allows Lua scripts to be extended with additional functionality via plugin files. Plugins are loaded before the main script, allowing them to register hooks that can intercept and modify function calls.

## Overview

When a Lua script is loaded, the system automatically looks for a corresponding `.plugins` file (e.g., `playermechdeployment.lua.plugins`). If found, it loads all plugins defined in that file before loading the main script. This allows plugins to register hooks that can wrap functions in the main script.

## Plugin Configuration Format

Plugins are configured via a JSON file named `<scriptname>.plugins`. The file must contain a JSON array of plugin objects:

```json
[
  {
    "id": "my_plugin",
    "path": "/scripts/plugins/my_plugin.lua",
    "before": ["other_plugin"],
    "after": ["base_plugin"]
  },
  {
    "path": "/scripts/plugins/another_plugin.lua"
  }
]
```

### Plugin Object Fields

- **`id`** (optional): A unique identifier for the plugin. If not provided, a UUID is auto-generated. Used for dependency references.
- **`path`** (required): Path to the plugin Lua script, relative to the `.plugins` file location.
- **`before`** (optional): Array of plugin IDs or paths. This plugin will load before the specified plugins.
- **`after`** (optional): Array of plugin IDs or paths. This plugin will load after the specified plugins.

### Example

Given a script at `/scripts/deployment/playermechdeployment.lua`, create `/scripts/deployment/playermechdeployment.lua.plugins`:

```json
[
  {
    "id": "base_plugin",
    "path": "/scripts/deployment/base_plugin.lua"
  },
  {
    "id": "enhancement_plugin",
    "path": "/scripts/deployment/enhancement.lua",
    "after": ["base_plugin"]
  }
]
```

## Dependency Resolution

Plugins are loaded in dependency order using topological sort:

- If plugin A has `"before": ["B"]`, then A loads before B
- If plugin C has `"after": ["B"]`, then C loads after B
- Circular dependencies are detected and logged as errors, with the system falling back to the original plugin order

Dependencies can be specified by plugin ID or by plugin path.

## Loading Order

The system loads components in this order:

1. **Plugins** (in dependency-resolved order)
2. **Main script**
3. **Hooks application** (`hooks.applyAll()`)

This order ensures:
- Plugins can register hooks before the main script's `init()` function runs
- The main script can call functions that plugins have hooked
- All hooks are applied after both plugins and main script are loaded

## Hooks System

The hooks system (defined in `StarLua.cpp`) allows plugins to wrap functions in the main script. Plugins register hooks using `hooks.register()`, and `hooks.applyAll()` applies all registered hooks.

### Example Plugin

```lua
-- /scripts/deployment/example_plugin.lua

-- Register a hook to wrap the init() function
hooks.register("init", function(originalInit)
  return function(...)
    -- Do something before init
    print("Plugin: Before init")
    
    -- Call original function
    local result = originalInit(...)
    
    -- Do something after init
    print("Plugin: After init")
    
    return result
  end
end)
```

## Error Handling

- Missing plugin files: Logged as errors, plugin loading continues with remaining plugins
- Invalid plugin JSON: Logged as error, plugin loading stops for that script
- Dependency cycles: Logged as error with cycle path, system falls back to original order
- Hook application failures: Logged as errors, script execution continues

## Best Practices

1. **Use unique IDs**: Always provide an `id` field for plugins that will be referenced by other plugins
2. **Document dependencies**: Clearly specify `before`/`after` relationships
3. **Handle errors gracefully**: Plugin code should not crash if hooks fail
4. **Keep plugins focused**: Each plugin should have a single, well-defined purpose
5. **Test dependency chains**: Verify plugin loading order matches expectations

## Migration Notes

If you're migrating from a system without plugins:

- Existing scripts continue to work without changes
- Plugins are optional - scripts without `.plugins` files behave as before
- The hooks system must be available in the Lua context (it's automatically provided)
