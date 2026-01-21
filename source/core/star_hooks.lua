-- Lua-side hook registry and wrapper builder. This runs once per context to
-- define the `hooks` global and keep wrapping purely in Lua.
local hooks = {}
local registry = {}
local unpack = table.unpack or unpack

local function ensureEntry(path)
  local entry = registry[path]
  if not entry then
    entry = {before = {}, after = {}, wrapper = nil, original = nil}
    registry[path] = entry
  end
  return entry
end

local function splitPath(path)
  local parts = {}
  for part in string.gmatch(path, "[^%.]+") do
    parts[#parts + 1] = part
  end
  return parts
end

local function resolveContainer(parts)
  local container = _ENV
  if #parts == 0 then
    return nil, nil
  end
  for i = 1, #parts - 1 do
    local nextVal = container[parts[i]]
    if type(nextVal) ~= "table" then
      return nil, nil
    end
    container = nextVal
  end
  return container, parts[#parts]
end

local function wrapPath(path, entry)
  local parts = splitPath(path)
  local container, key = resolveContainer(parts)
  if not container or not key then
    return
  end

  local current = container[key]
  if entry.wrapper and current == entry.wrapper and entry.original then
    current = entry.original
  end

  if type(current) ~= "function" then
    return
  end

  entry.original = current

  local beforeList = entry.before
  local afterList = entry.after

  -- Get the original function's _ENV upvalue
  local originalEnv = _ENV
  if debug and debug.getupvalue then
    local name, value = debug.getupvalue(current, 1)
    if name == "_ENV" then
      originalEnv = value
    end
  end

  -- Create wrapped versions of hooks that use the original function's environment
  -- This ensures hooks can access C++ callbacks like sb.logInfo correctly
  local wrappedBeforeList = {}
  for i = 1, #beforeList do
    local hook = beforeList[i]
    wrappedBeforeList[i] = function(...)
      -- Try to set hook's _ENV to match original function's _ENV
      local oldEnv = nil
      local hadEnv = false
      local success = false
      
      if debug and debug.getupvalue and debug.setupvalue then
        local ok, name, env = pcall(debug.getupvalue, hook, 1)
        if ok and name == "_ENV" then
          oldEnv = env
          hadEnv = true
          local setOk = pcall(debug.setupvalue, hook, 1, originalEnv)
          if setOk then
            success = true
          end
        end
      end
      
      -- Call hook - it will use originalEnv if we successfully set it
      local results = {hook(...)}
      
      -- Restore hook's original _ENV if we changed it
      if hadEnv and success and debug.setupvalue then
        pcall(debug.setupvalue, hook, 1, oldEnv)
      end
      
      return unpack(results)
    end
  end
  
  local wrappedAfterList = {}
  for i = 1, #afterList do
    local hook = afterList[i]
    wrappedAfterList[i] = function(...)
      -- Try to set hook's _ENV to match original function's _ENV
      local oldEnv = nil
      local hadEnv = false
      local success = false
      
      if debug and debug.getupvalue and debug.setupvalue then
        local ok, name, env = pcall(debug.getupvalue, hook, 1)
        if ok and name == "_ENV" then
          oldEnv = env
          hadEnv = true
          local setOk = pcall(debug.setupvalue, hook, 1, originalEnv)
          if setOk then
            success = true
          end
        end
      end
      
      -- Call hook - it will use originalEnv if we successfully set it
      local results = {hook(...)}
      
      -- Restore hook's original _ENV if we changed it
      if hadEnv and success and debug.setupvalue then
        pcall(debug.setupvalue, hook, 1, oldEnv)
      end
      
      return unpack(results)
    end
  end

  local function wrapped(...)
    local originalArgs = {...}
    local args = originalArgs

    -- Call before hooks with correct environment
    for i = 1, #wrappedBeforeList do
      local out = {wrappedBeforeList[i](unpack(args))}
      if #out > 0 then
        args = out
      end
    end

    local results = {entry.original(unpack(args))}
    local currentResults = results

    -- Call after hooks with correct environment
    for i = 1, #wrappedAfterList do
      local out = {wrappedAfterList[i](unpack(currentResults), unpack(originalArgs))}
      if #out > 0 then
        currentResults = out
      end
    end

    return unpack(currentResults)
  end

  entry.wrapper = wrapped
  container[key] = wrapped
end

function hooks.before(path, fn)
  assert(type(path) == "string", "hooks.before: path must be string")
  assert(type(fn) == "function", "hooks.before: hook must be function")
  local entry = ensureEntry(path)
  entry.before[#entry.before + 1] = fn
end

function hooks.after(path, fn)
  assert(type(path) == "string", "hooks.after: path must be string")
  assert(type(fn) == "function", "hooks.after: hook must be function")
  local entry = ensureEntry(path)
  entry.after[#entry.after + 1] = fn
end

function hooks.remove(path, fn)
  assert(type(path) == "string", "hooks.remove: path must be string")
  assert(type(fn) == "function", "hooks.remove: hook must be function")
  local entry = registry[path]
  if not entry then
    return
  end
  local function removeFrom(list)
    for i = #list, 1, -1 do
      if list[i] == fn then
        table.remove(list, i)
      end
    end
  end
  removeFrom(entry.before)
  removeFrom(entry.after)
end

function hooks.clear(path)
  assert(type(path) == "string", "hooks.clear: path must be string")
  local entry = registry[path]
  if not entry then
    return
  end
  entry.before = {}
  entry.after = {}
  entry.original = nil
  entry.wrapper = nil
end

function hooks.has(path)
  assert(type(path) == "string", "hooks.has: path must be string")
  local entry = registry[path]
  if not entry then
    return false
  end
  return #entry.before > 0 or #entry.after > 0
end

function hooks.applyAll()
  for path, entry in pairs(registry) do
    wrapPath(path, entry)
  end
end

_ENV.hooks = hooks
