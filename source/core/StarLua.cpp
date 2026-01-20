#include "StarLua.hpp"
#include "StarArray.hpp"
#include "StarTime.hpp"
#include "StarLogging.hpp"
#include "imgui_lua_bindings.hpp"

namespace Star {

namespace {

// Lua-side hook registry and wrapper builder. This runs once per context to
// define the `hooks` global and keep wrapping purely in Lua.
char const* HooksLibrarySource = R"HOOKS(
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
)HOOKS";

}

std::ostream& operator<<(std::ostream& os, LuaValue const& value) {
  if (value.is<LuaBoolean>()) {
    os << (value.get<LuaBoolean>() ? "true" : "false");
  } else if (value.is<LuaInt>()) {
    os << value.get<LuaInt>();
  } else if (value.is<LuaFloat>()) {
    os << value.get<LuaFloat>();
  } else if (value.is<LuaString>()) {
    os << value.get<LuaString>().ptr();
  } else if (value.is<LuaTable>()) {
    os << "{";
    bool first = true;
    value.get<LuaTable>().iterate([&os, &first](LuaValue const& key, LuaValue const& value) {
      if (first)
        first = false;
      else
        os << ", ";
      os << key << ": " << value;
    });
    os << "}";
  } else if (value.is<LuaFunction>()) {
    os << "<function reg:" << value.get<LuaFunction>().handleIndex() << ">";
  } else if (value.is<LuaThread>()) {
    os << "<thread reg:" << value.get<LuaThread>().handleIndex() << ">";
  } else if (value.is<LuaUserData>()) {
    os << "<userdata reg:" << value.get<LuaUserData>().handleIndex() << ">";
  } else {
    os << "nil";
  }
  return os;
}

bool LuaTable::contains(char const* key) const {
  return engine().tableGet(false, handleIndex(), key) != LuaNil;
}

void LuaTable::remove(char const* key) const {
  engine().tableSet(false, handleIndex(), key, LuaNil);
}

LuaInt LuaTable::length() const {
  return engine().tableLength(false, handleIndex());
}

Maybe<LuaTable> LuaTable::getMetatable() const {
  return engine().tableGetMetatable(handleIndex());
}

void LuaTable::setMetatable(LuaTable const& table) const {
  return engine().tableSetMetatable(handleIndex(), table);
}

LuaInt LuaTable::rawLength() const {
  return engine().tableLength(true, handleIndex());
}

void LuaCallbacks::copyCallback(String srcName, String dstName) {
  m_callbacks.set(dstName, m_callbacks.get(srcName));
}

bool LuaCallbacks::removeCallback(String name) {
  return m_callbacks.remove(name);
}

LuaCallbacks& LuaCallbacks::merge(LuaCallbacks const& callbacks) {
  try {
    for (auto const& pair : callbacks.m_callbacks)
      m_callbacks.add(pair.first, pair.second);
  } catch (MapException const& e) {
    throw LuaException(strf("Failed to merge LuaCallbacks: {}", outputException(e, true)));
  }

  return *this;
}

StringMap<LuaDetail::LuaWrappedFunction> const& LuaCallbacks::callbacks() const {
  return m_callbacks;
}

bool LuaContext::containsPath(String path) const {
  return engine().contextGetPath(handleIndex(), std::move(path)) != LuaNil;
}

void LuaContext::load(char const* contents, size_t size, char const* name) {
  engine().contextLoad(handleIndex(), contents, size, name);
}

void LuaContext::load(String const& contents, String const& name) {
  load(contents.utf8Ptr(), contents.utf8Size(), name.utf8Ptr());
}

void LuaContext::load(ByteArray const& contents, String const& name) {
  load(contents.ptr(), contents.size(), name.utf8Ptr());
}

void LuaContext::setRequireFunction(RequireFunction requireFunction) {
  engine().setContextRequire(handleIndex(), std::move(requireFunction));
}

void LuaContext::setCallbacks(String const& tableName, LuaCallbacks const& callbacks) const {
  auto& eng = engine();
  if (LuaContext::contains(tableName))
    return;

  auto callbackTable = eng.createTable();
  for (auto const& p : callbacks.callbacks())
    callbackTable.set(p.first, eng.createWrappedFunction(p.second));
  LuaContext::set(tableName, callbackTable);
}

LuaString LuaContext::createString(String const& str) {
  return engine().createString(str);
}

LuaString LuaContext::createString(char const* str) {
  return engine().createString(str);
}

LuaTable LuaContext::createTable() {
  return engine().createTable();
}

LuaNullEnforcer::LuaNullEnforcer(LuaEngine& engine)
  : m_engine(&engine) { ++m_engine->m_nullTerminated; };

LuaNullEnforcer::LuaNullEnforcer(LuaNullEnforcer&& other) { m_engine = take(other.m_engine); };

LuaNullEnforcer::~LuaNullEnforcer() { if (m_engine) --m_engine->m_nullTerminated; };

LuaValue LuaConverter<Json>::from(LuaEngine& engine, Json const& v) {
  if (v.isType(Json::Type::Null)) {
    return LuaNil;
  } else if (v.isType(Json::Type::Float)) {
    return LuaFloat(v.toDouble());
  } else if (v.isType(Json::Type::Bool)) {
    return v.toBool();
  } else if (v.isType(Json::Type::Int)) {
    return LuaInt(v.toInt());
  } else if (v.isType(Json::Type::String)) {
    return engine.createString(*v.stringPtr());
  } else {
    return LuaDetail::jsonContainerToTable(engine, v);
  }
}

Maybe<Json> LuaConverter<Json>::to(LuaEngine&, LuaValue const& v) {
  if (v == LuaNil)
    return Json();

  if (auto b = v.ptr<LuaBoolean>())
    return Json(*b);

  if (auto i = v.ptr<LuaInt>())
    return Json(*i);

  if (auto f = v.ptr<LuaFloat>())
    return Json(*f);

  if (auto s = v.ptr<LuaString>())
    return Json(s->toString());

  if (v.is<LuaTable>())
    return LuaDetail::tableToJsonContainer(v.get<LuaTable>());

  return {};
}

LuaValue LuaConverter<JsonObject>::from(LuaEngine& engine, JsonObject v) {
  return engine.luaFrom<Json>(Json(std::move(v)));
}

Maybe<JsonObject> LuaConverter<JsonObject>::to(LuaEngine& engine, LuaValue v) {
  auto j = engine.luaTo<Json>(std::move(v));
  if (j.type() == Json::Type::Object) {
    return j.toObject();
  } else if (j.type() == Json::Type::Array) {
    auto list = j.arrayPtr();
    if (list->empty())
      return JsonObject();
  }

  return {};
}

LuaValue LuaConverter<JsonArray>::from(LuaEngine& engine, JsonArray v) {
  return engine.luaFrom<Json>(Json(std::move(v)));
}

Maybe<JsonArray> LuaConverter<JsonArray>::to(LuaEngine& engine, LuaValue v) {
  auto j = engine.luaTo<Json>(std::move(v));
  if (j.type() == Json::Type::Array) {
    return j.toArray();
  } else if (j.type() == Json::Type::Object) {
    auto map = j.objectPtr();
    if (map->empty())
      return JsonArray();
  }

  return {};
}

LuaEnginePtr LuaEngine::create(bool safe) {
  LuaEnginePtr self(new LuaEngine);

  self->m_state = lua_newstate(allocate, nullptr);

  self->m_scriptDefaultEnvRegistryId = LUA_NOREF;
  self->m_wrappedFunctionMetatableRegistryId = LUA_NOREF;
  self->m_requireFunctionMetatableRegistryId = LUA_NOREF;
  self->m_hookIdentifierRegistryId = LUA_NOREF;

  self->m_instructionLimit = 0;
  self->m_profilingEnabled = false;
  self->m_instructionMeasureInterval = 1000;
  self->m_instructionCount = 0;
  self->m_recursionLevel = 0;
  self->m_recursionLimit = 0;
  self->m_nullTerminated = 0;

  if (!self->m_state)
    throw LuaException("Failed to initialize Lua");

  lua_checkstack(self->m_state, 5);

  // Create handle stack thread and place it in the registry to prevent it from being garbage
  // collected.

  self->m_handleThread = lua_newthread(self->m_state);
  luaL_ref(self->m_state, LUA_REGISTRYINDEX);

  // We need 1 extra stack space to move values in and out of the handle stack.
  self->m_handleStackSize = LUA_MINSTACK - 1;
  self->m_handleStackMax = 0;

  // Set the extra space in the lua main state to the pointer to the main
  // LuaEngine
  *reinterpret_cast<LuaEngine**>(lua_getextraspace(self->m_state)) = self.get();

  // Create the common message handler function for pcall to print a better
  // message with a traceback
  lua_pushcfunction(self->m_state, [](lua_State* state) {
      // Don't modify the error if it is one of the special limit errrors
      if (lua_islightuserdata(state, 1)) {
        void* error = lua_touserdata(state, -1);
        if (error == &s_luaInstructionLimitExceptionKey || error == &s_luaRecursionLimitExceptionKey)
          return 1;
      }

      luaL_traceback(state, state, lua_tostring(state, 1), 0);
      lua_remove(state, 1);
      return 1;
    });
  self->m_pcallTracebackMessageHandlerRegistryId = luaL_ref(self->m_state, LUA_REGISTRYINDEX);

  // Create the common metatable for wrapped functions
  lua_newtable(self->m_state);
  lua_pushcfunction(self->m_state, [](lua_State* state) {
      auto func = (LuaDetail::LuaWrappedFunction*)lua_touserdata(state, 1);
      func->~function();
      return 0;
    });
  LuaDetail::rawSetField(self->m_state, -2, "__gc");
  lua_pushboolean(self->m_state, 0);
  LuaDetail::rawSetField(self->m_state, -2, "__metatable");
  self->m_wrappedFunctionMetatableRegistryId = luaL_ref(self->m_state, LUA_REGISTRYINDEX);

  // Create the common metatable for require functions
  lua_newtable(self->m_state);
  lua_pushcfunction(self->m_state, [](lua_State* state) {
      auto func = (LuaContext::RequireFunction*)lua_touserdata(state, 1);
      func->~function();
      return 0;
    });
  LuaDetail::rawSetField(self->m_state, -2, "__gc");
  lua_pushboolean(self->m_state, 0);
  LuaDetail::rawSetField(self->m_state, -2, "__metatable");
  self->m_requireFunctionMetatableRegistryId = luaL_ref(self->m_state, LUA_REGISTRYINDEX);

  // Load all base libraries and prune them of unsafe functions

  luaL_requiref(self->m_state, "_ENV", luaopen_base, true);
  if (safe) {
    StringSet baseWhitelist = {
        "assert",
        "error",
        "getmetatable",
        "ipairs",
        "next",
        "pairs",
        "pcall",
        "print",
        "rawequal",
        "rawget",
        "rawlen",
        "rawset",
        "select",
        "setmetatable",
        "tonumber",
        "tostring",
        "type",
        "unpack",
        "_VERSION",
        "xpcall"};

    lua_pushnil(self->m_state);
    while (lua_next(self->m_state, -2) != 0) {
      lua_pop(self->m_state, 1);
      String key(lua_tostring(self->m_state, -1));

      if (!baseWhitelist.contains(key)) {
        lua_pushvalue(self->m_state, -1);
        lua_pushnil(self->m_state);
        lua_rawset(self->m_state, -4);
      }
    }
  }
  lua_pop(self->m_state, 1);

  luaL_requiref(self->m_state, "os", luaopen_os, true);
  if (safe) {
    StringSet osWhitelist = {"clock", "difftime", "time", "date"};

    lua_pushnil(self->m_state);
    while (lua_next(self->m_state, -2) != 0) {
      lua_pop(self->m_state, 1);
      String key(lua_tostring(self->m_state, -1));

      if (!osWhitelist.contains(key)) {
        lua_pushvalue(self->m_state, -1);
        lua_pushnil(self->m_state);
        lua_rawset(self->m_state, -4);
      }
    }
  }
  lua_pop(self->m_state, 1);

  // loads a lua base library, leaves it at the top of the stack
  auto loadBaseLibrary = [](lua_State* state, char const* modname, lua_CFunction openf) {
    luaL_requiref(state, modname, openf, true);

    // set __metatable metamethod to false
    // otherwise scripts can access and mutate the metatable, allowing passing values
    // between script contexts, breaking the sandbox
    lua_newtable(state);
    lua_pushliteral(state, "__metatable");
    lua_pushboolean(state, 0);
    lua_rawset(state, -3);
    lua_setmetatable(state, -2);
  };

  loadBaseLibrary(self->m_state, "coroutine", luaopen_coroutine);
  // replace coroutine resume with one that appends tracebacks
  lua_pushliteral(self->m_state, "resume");
  lua_pushcfunction(self->m_state, &LuaEngine::coresumeWithTraceback);
  lua_rawset(self->m_state, -3);

  loadBaseLibrary(self->m_state, "math", luaopen_math);
  loadBaseLibrary(self->m_state, "string", luaopen_string);
  loadBaseLibrary(self->m_state, "table", luaopen_table);
  loadBaseLibrary(self->m_state, "utf8", luaopen_utf8);
  lua_pop(self->m_state, 5);

  if (!safe) {
    loadBaseLibrary(self->m_state, "io", luaopen_io);
    loadBaseLibrary(self->m_state, "package", luaopen_package);
    loadBaseLibrary(self->m_state, "debug", luaopen_debug);
    lua_pop(self->m_state, 3);
  }

  // Make a shallow copy of the default script environment and save it for
  // resetting the global state.
  lua_rawgeti(self->m_state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
  lua_newtable(self->m_state);
  LuaDetail::shallowCopy(self->m_state, -2, -1);
  self->m_scriptDefaultEnvRegistryId = luaL_ref(self->m_state, LUA_REGISTRYINDEX);
  lua_pop(self->m_state, 1);

  self->setGlobal("jarray", self->createFunction(&LuaDetail::jarray));
  self->setGlobal("jobject", self->createFunction(&LuaDetail::jobject));
  self->setGlobal("jremove", self->createFunction(&LuaDetail::jcontRemove));
  self->setGlobal("jsize", self->createFunction(&LuaDetail::jcontSize));
  self->setGlobal("jresize", self->createFunction(&LuaDetail::jcontResize));

  self->setGlobal("shared", self->createTable());

  // Create hook identifier registry (retained for legacy callers; new hook
  // system is defined in Lua per-context).
  lua_newtable(self->m_state);
  self->m_hookIdentifierRegistryId = luaL_ref(self->m_state, LUA_REGISTRYINDEX);

  return self;
}

LuaEngine::~LuaEngine() {
  // If we've had a stack space leak, this will not be zero
  starAssert(lua_gettop(m_state) == 0);
  lua_close(m_state);
}

void LuaEngine::setInstructionLimit(uint64_t instructionLimit) {
  if (instructionLimit != m_instructionLimit) {
    m_instructionLimit = instructionLimit;
    updateCountHook();
  }
}

uint64_t LuaEngine::instructionLimit() const {
  return m_instructionLimit;
}

void LuaEngine::setProfilingEnabled(bool profilingEnabled) {
  if (profilingEnabled != m_profilingEnabled) {
    m_profilingEnabled = profilingEnabled;
    m_profileEntries.clear();
    updateCountHook();
  }
}

bool LuaEngine::profilingEnabled() const {
  return m_profilingEnabled;
}

List<LuaProfileEntry> LuaEngine::getProfile() {
  List<LuaProfileEntry> profileEntries;
  for (auto const& p : m_profileEntries) {
    profileEntries.append(*p.second);
  }

  return profileEntries;
}

void LuaEngine::setInstructionMeasureInterval(unsigned measureInterval) {
  if (measureInterval != m_instructionMeasureInterval) {
    m_instructionMeasureInterval = measureInterval;
    updateCountHook();
  }
}

unsigned LuaEngine::instructionMeasureInterval() const {
  return m_instructionMeasureInterval;
}

void LuaEngine::setRecursionLimit(unsigned recursionLimit) {
  m_recursionLimit = recursionLimit;
}

unsigned LuaEngine::recursionLimit() const {
  return m_recursionLimit;
}

ByteArray LuaEngine::compile(char const* contents, size_t size, char const* name) {
  lua_checkstack(m_state, 1);

  handleError(m_state, luaL_loadbuffer(m_state, contents, size, name));

  ByteArray compiledScript;
  lua_Writer writer = [](lua_State*, void const* data, size_t size, void* byteArrayPtr) -> int {
    ((ByteArray*)byteArrayPtr)->append((char const*)data, size);
    return 0;
  };
  lua_dump(m_state, writer, &compiledScript, false);
  lua_pop(m_state, 1);

  return compiledScript;
}

ByteArray LuaEngine::compile(String const& contents, String const& name) {
  return compile(contents.utf8Ptr(), contents.utf8Size(), name.empty() ? nullptr : name.utf8Ptr());
}

ByteArray LuaEngine::compile(ByteArray const& contents, String const& name) {
  return compile(contents.ptr(), contents.size(), name.empty() ? nullptr : name.utf8Ptr());
}

lua_Debug const& LuaEngine::debugInfo(int level, const char* what) {
  lua_Debug& debug = m_debugInfo = lua_Debug();
  lua_getstack(m_state, level, &debug);
  lua_getinfo(m_state, what, &debug);
  return debug;
}

LuaString LuaEngine::createString(std::string const& str) {
  lua_checkstack(m_state, 1);

  if (m_nullTerminated > 0)
    lua_pushstring(m_state, str.data());
  else
    lua_pushlstring(m_state, str.data(), str.size());
  return LuaString(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), popHandle(m_state)));
}

LuaString LuaEngine::createString(String const& str) {
  return createString(str.utf8());
}

LuaString LuaEngine::createString(char const* str) {
  lua_checkstack(m_state, 1);

  lua_pushstring(m_state, str);
  return LuaString(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), popHandle(m_state)));
}

LuaTable LuaEngine::createTable(int narr, int nrec) {
  lua_checkstack(m_state, 1);

  lua_createtable(m_state, narr, nrec);
  return LuaTable(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), popHandle(m_state)));
}

LuaThread LuaEngine::createThread() {
  lua_checkstack(m_state, 1);

  lua_newthread(m_state);
  return LuaThread(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), popHandle(m_state)));
}

void LuaEngine::threadPushFunction(int threadIndex, int functionIndex) {
  lua_State* thread = lua_tothread(m_handleThread, threadIndex);

  int status = lua_status(thread);
  lua_Debug ar;
  if (status != LUA_OK || lua_getstack(thread, 0, &ar) > 0 || lua_gettop(thread) > 0)
    throw LuaException(strf("Cannot push function to active or errored thread with status {}", status));

  pushHandle(thread, functionIndex);
}

LuaThread::Status LuaEngine::threadStatus(int handleIndex) {
  lua_State* thread = lua_tothread(m_handleThread, handleIndex);

  int status = lua_status(thread);
  if (status != LUA_OK && status != LUA_YIELD)
    return LuaThread::Status::Error;

  lua_Debug ar;
  if (status == LUA_YIELD || lua_getstack(thread, 0, &ar) > 0 || lua_gettop(thread) > 0)
    return LuaThread::Status::Active;

  return LuaThread::Status::Dead;
}

LuaContext LuaEngine::createContext() {
  lua_checkstack(m_state, 2);

  // Create a new blank environment and copy the default environment to it.
  lua_newtable(m_state);
  lua_rawgeti(m_state, LUA_REGISTRYINDEX, m_scriptDefaultEnvRegistryId);
  LuaDetail::shallowCopy(m_state, -1, -2);
  lua_pop(m_state, 1);

  auto context = LuaContext(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), popHandle(m_state)));
  // Add loadstring
  auto handleIndex = context.handleIndex();
  context.set("loadstring", createFunction([this, handleIndex](String const& source, Maybe<String> const& name, Maybe<LuaTable> const& env) -> LuaFunction {
    String functionName = name ? strf("loadstring: {}", *name) : "loadstring";
    return createFunctionFromSource(env ? env->handleIndex() : handleIndex, source.utf8Ptr(), source.utf8Size(), functionName.utf8Ptr());
  }));

  // Install Lua-side hook support in the new context.
  context.load(HooksLibrarySource, "star_hooks.lua");

  // Then set that environment as the new context environment in the registry.
  return context;
}

void LuaEngine::collectGarbage(Maybe<unsigned> steps) {
  for (auto handleIndex : take(m_handleFree)) {
    lua_pushnil(m_handleThread);
    lua_replace(m_handleThread, handleIndex);
  }

  if (steps)
    lua_gc(m_state, LUA_GCSTEP, *steps);
  else
    lua_gc(m_state, LUA_GCCOLLECT, 0);
}

void LuaEngine::setAutoGarbageCollection(bool autoGarbageColleciton) {
  lua_gc(m_state, LUA_GCSTOP, autoGarbageColleciton ? 1 : 0);
}

void LuaEngine::tuneAutoGarbageCollection(float pause, float stepMultiplier) {
  lua_gc(m_state, LUA_GCSETPAUSE, round(pause * 100));
  lua_gc(m_state, LUA_GCSETSTEPMUL, round(stepMultiplier * 100));
}

size_t LuaEngine::memoryUsage() const {
  return (size_t)lua_gc(m_state, LUA_GCCOUNT, 0) * 1024 + lua_gc(m_state, LUA_GCCOUNTB, 0);
}

LuaNullEnforcer LuaEngine::nullTerminate() {
  return LuaNullEnforcer(*this);
}

void LuaEngine::setNullTerminated(bool nullTerminated) {
  m_nullTerminated = nullTerminated ? 0 : INT_MIN;
}

void LuaEngine::addImGui() {
  lua_checkstack(m_state, 1);

  lua_rawgeti(m_state, LUA_REGISTRYINDEX, m_scriptDefaultEnvRegistryId);
  pushImguiBindings(m_state);
  lua_setfield(m_state, -2, "imgui");

  lua_pop(m_state, 1);
}

LuaEngine* LuaEngine::luaEnginePtr(lua_State* state) {
  return (*reinterpret_cast<LuaEngine**>(lua_getextraspace(state)));
}

void LuaEngine::countHook(lua_State* state, lua_Debug* ar) {
  starAssert(ar->event == LUA_HOOKCOUNT);
  lua_checkstack(state, 4);

  auto self = luaEnginePtr(state);

  // If the instruction count is 0, that means in this sequence of calls,
  // we have not hit a debug hook yet.  Since we don't know the state of
  // the internal lua instruction counter at the start, we don't know how
  // many instructions have been executed, only that it is >= 1 and <=
  // m_instructionMeasureInterval, so we pick the low estimate.
  if (self->m_instructionCount == 0)
    self->m_instructionCount = 1;
  else
    self->m_instructionCount += self->m_instructionMeasureInterval;

  if (self->m_instructionLimit != 0 && self->m_instructionCount > self->m_instructionLimit) {
    lua_pushlightuserdata(state, &s_luaInstructionLimitExceptionKey);
    lua_error(state);
  }

  if (self->m_profilingEnabled) {
    // find bottom of the stack
    // ar will contain the stack info from the last call that returns 1
    int stackLevel = -1;
    while (lua_getstack(state, stackLevel + 1, ar) == 1)
      stackLevel++;

    shared_ptr<LuaProfileEntry> parentEntry = nullptr;
    while (true) {
      // Get the 'n' name info and 'S' source info
      if (lua_getinfo(state, "nS", ar) == 0)
        break;

      auto key = make_tuple(String(ar->short_src), (unsigned)ar->linedefined);
      auto& entryMap = parentEntry ? parentEntry->calls : self->m_profileEntries;
      if (!entryMap.contains(key)) {
        auto e = make_shared<LuaProfileEntry>();
        e->source = ar->short_src;
        e->sourceLine = ar->linedefined;
        entryMap.set(key, e);
      }
      auto entry = entryMap.get(key);

      // metadata
      if (!entry->name && ar->name)
        entry->name = String(ar->name);
      if (!entry->nameScope && String() != ar->namewhat)
        entry->nameScope = String(ar->namewhat);

      // Only add timeTaken to the self time for the function we are actually
      // in, not parent functions
      if (stackLevel == 0) {
        entry->totalTime += 1;
        entry->selfTime += 1;
      } else {
        entry->totalTime += 1;
      }

      parentEntry = entry;
      if (lua_getstack(state, --stackLevel, ar) == 0)
        break;
    }
  }
}

void* LuaEngine::allocate(void*, void* ptr, size_t oldSize, size_t newSize) {
  if (newSize == 0) {
    Star::free(ptr, oldSize);
    return nullptr;
  } else {
    return Star::realloc(ptr, newSize);
  }
}

void LuaEngine::handleError(lua_State* state, int res) {
  if (res != LUA_OK) {
    if (lua_islightuserdata(state, -1)) {
      void* error = lua_touserdata(state, -1);
      if (error == &s_luaInstructionLimitExceptionKey) {
        lua_pop(state, 1);
        throw LuaInstructionLimitReached();
      }
      if (error == &s_luaRecursionLimitExceptionKey) {
        lua_pop(state, 1);
        throw LuaRecursionLimitReached();
      }
    }

    String error;
    if (lua_isstring(state, -1))
      error = strf("Error code {}, {}", res, lua_tostring(state, -1));
    else
      error = strf("Error code {}, <unknown error>", res);

    lua_pop(state, 1);

    // This seems terrible, but as far as I can tell, this is exactly what the
    // stock lua repl does.
    if (error.endsWith("<eof>"))
      throw LuaIncompleteStatementException(error.takeUtf8());
    else
      throw LuaException(error.takeUtf8());
  }
}

int LuaEngine::pcallWithTraceback(lua_State* state, int nargs, int nresults) {
  int msghPosition = lua_gettop(state) - nargs;
  lua_rawgeti(m_state, LUA_REGISTRYINDEX, m_pcallTracebackMessageHandlerRegistryId);
  lua_insert(state, msghPosition);
  int ret = lua_pcall(state, nargs, nresults, msghPosition);
  lua_remove(state, msghPosition);
  return ret;
}

int LuaEngine::coresumeWithTraceback(lua_State* state) {
  lua_State* co = lua_tothread(state, 1);
  if (!co) {
    lua_checkstack(state, 2);
    lua_pushboolean(state, 0);
    lua_pushliteral(state, "bad argument #1 to 'resume' (thread expected)");
    return 2;
  }

  int args = lua_gettop(state) - 1;
  lua_checkstack(co, args);
  if (lua_status(co) == LUA_OK && lua_gettop(co) == 0) {
    lua_checkstack(state, 2);
    lua_pushboolean(state, 0);
    lua_pushliteral(state, "cannot resume dead coroutine");
    return 2;
  }

  lua_xmove(state, co, args);
  int status = lua_resume(co, state, args);
  if (status == LUA_OK || status == LUA_YIELD) {
    int res = lua_gettop(co);
    lua_checkstack(state, res + 1);
    lua_pushboolean(state, 1);
    lua_xmove(co, state, res);
    return res + 1;
  } else {
    lua_checkstack(state, 2);
    lua_pushboolean(state, 0);
    propagateErrorWithTraceback(co, state);
    return 2;
  }
}

void LuaEngine::propagateErrorWithTraceback(lua_State* from, lua_State* to) {
  if (const char* error = lua_tostring(from, -1)) {
    luaL_traceback(to, from, error, 0); // error + traceback
    lua_pop(from, 1);
  } else {
    lua_xmove(from, to, 1); // just error, no traceback
  }
}

char const* LuaEngine::stringPtr(int handleIndex) {
  return lua_tostring(m_handleThread, handleIndex);
}

size_t LuaEngine::stringLength(int handleIndex) {
  if (m_nullTerminated > 0)
    return strlen(lua_tostring(m_handleThread, handleIndex));
  else {
    size_t len = 0;
    lua_tolstring(m_handleThread, handleIndex, &len);
    return len;
  }
}

String LuaEngine::string(int handleIndex) {
  if (m_nullTerminated > 0)
    return String(lua_tostring(m_handleThread, handleIndex));
  else {
    size_t len = 0;
    const char* data = lua_tolstring(m_handleThread, handleIndex, &len);
    return String(data, len);
  }
}

StringView LuaEngine::stringView(int handleIndex) {
  if (m_nullTerminated > 0)
    return StringView(lua_tostring(m_handleThread, handleIndex));
  else {
    size_t len = 0;
    const char* data = lua_tolstring(m_handleThread, handleIndex, &len);
    return StringView(data, len);
  }
}

LuaValue LuaEngine::tableGet(bool raw, int handleIndex, LuaValue const& key) {
  lua_checkstack(m_state, 1);

  pushHandle(m_state, handleIndex);
  pushLuaValue(m_state, key);
  if (raw)
    lua_rawget(m_state, -2);
  else
    lua_gettable(m_state, -2);

  LuaValue v = popLuaValue(m_state);
  lua_pop(m_state, 1);
  return v;
}

LuaValue LuaEngine::tableGet(bool raw, int handleIndex, char const* key) {
  lua_checkstack(m_state, 1);

  pushHandle(m_state, handleIndex);
  if (raw)
    LuaDetail::rawGetField(m_state, -1, key);
  else
    lua_getfield(m_state, -1, key);
  lua_remove(m_state, -2);
  return popLuaValue(m_state);
}

void LuaEngine::tableSet(bool raw, int handleIndex, LuaValue const& key, LuaValue const& value) {
  lua_checkstack(m_state, 1);

  pushHandle(m_state, handleIndex);
  pushLuaValue(m_state, key);
  pushLuaValue(m_state, value);

  if (raw)
    lua_rawset(m_state, -3);
  else
    lua_settable(m_state, -3);

  lua_pop(m_state, 1);
}

void LuaEngine::tableSet(bool raw, int handleIndex, char const* key, LuaValue const& value) {
  lua_checkstack(m_state, 1);
  pushHandle(m_state, handleIndex);
  pushLuaValue(m_state, value);

  if (raw)
    LuaDetail::rawSetField(m_state, -2, key);
  else
    lua_setfield(m_state, -2, key);

  lua_pop(m_state, 1);
}

LuaInt LuaEngine::tableLength(bool raw, int handleIndex) {
  if (raw) {
    return lua_rawlen(m_handleThread, handleIndex);

  } else {
    lua_checkstack(m_state, 1);
    pushHandle(m_state, handleIndex);
    lua_len(m_state, -1);
    LuaInt len = lua_tointeger(m_state, -1);
    lua_pop(m_state, 2);
    return len;
  }
}

void LuaEngine::tableIterate(int handleIndex, function<bool(LuaValue key, LuaValue value)> iterator) {
  lua_checkstack(m_state, 4);

  pushHandle(m_state, handleIndex);
  lua_pushnil(m_state);
  while (lua_next(m_state, -2) != 0) {
    lua_pushvalue(m_state, -2);
    LuaValue key = popLuaValue(m_state);
    LuaValue value = popLuaValue(m_state);
    bool cont = false;
    try {
      cont = iterator(std::move(key), std::move(value));
    } catch (...) {
      lua_pop(m_state, 2);
      throw;
    }
    if (!cont) {
      lua_pop(m_state, 1);
      break;
    }
  }

  lua_pop(m_state, 1);
}

Maybe<LuaTable> LuaEngine::tableGetMetatable(int handleIndex) {
  lua_checkstack(m_state, 2);

  pushHandle(m_state, handleIndex);
  if (lua_getmetatable(m_state, -1) == 0) {
    lua_pop(m_state, 1);
    return {};
  }
  LuaTable table = popLuaValue(m_state).get<LuaTable>();
  lua_pop(m_state, 1);
  return table;
}

void LuaEngine::tableSetMetatable(int handleIndex, LuaTable const& table) {
  lua_checkstack(m_state, 2);

  pushHandle(m_state, handleIndex);
  pushHandle(m_state, table.handleIndex());
  lua_setmetatable(m_state, -2);
  lua_pop(m_state, 1);
}

void LuaEngine::setContextRequire(int handleIndex, LuaContext::RequireFunction requireFunction) {
  lua_checkstack(m_state, 4);

  pushHandle(m_state, handleIndex);

  auto funcUserdata = (LuaContext::RequireFunction*)lua_newuserdata(m_state, sizeof(LuaContext::RequireFunction));
  new (funcUserdata) LuaContext::RequireFunction(std::move(requireFunction));
  lua_rawgeti(m_state, LUA_REGISTRYINDEX, m_requireFunctionMetatableRegistryId);
  lua_setmetatable(m_state, -2);

  lua_pushvalue(m_state, -2);

  auto invokeRequire = [](lua_State* state) {
    try {
      lua_checkstack(state, 2);

      auto require = (LuaContext::RequireFunction*)lua_touserdata(state, lua_upvalueindex(1));
      auto self = luaEnginePtr(state);

      auto moduleName = self->luaTo<LuaString>(self->popLuaValue(state));

      lua_pushvalue(state, lua_upvalueindex(2));
      LuaContext context(LuaDetail::LuaHandle(RefPtr<LuaEngine>(self), self->popHandle(state)));

      (*require)(context, moduleName);
      return 0;
    } catch (LuaInstructionLimitReached const&) {
      lua_pushlightuserdata(state, &s_luaInstructionLimitExceptionKey);
      return lua_error(state);
    } catch (LuaRecursionLimitReached const&) {
      lua_pushlightuserdata(state, &s_luaRecursionLimitExceptionKey);
      return lua_error(state);
    } catch (std::exception const& e) {
      luaL_where(state, 1);
      lua_pushstring(state, printException(e, true).c_str());
      lua_concat(state, 2);
      return lua_error(state);
    }
  };

  lua_pushcclosure(m_state, invokeRequire, 2);

  LuaDetail::rawSetField(m_state, -2, "require");

  lua_pop(m_state, 1);
}

void LuaEngine::contextLoad(int handleIndex, char const* contents, size_t size, char const* name) {
  lua_checkstack(m_state, 2);

  // First load the script...
  handleError(m_state, luaL_loadbuffer(m_state, contents, size, name));

  // Then set the _ENV upvalue for the newly loaded chunk to our context env so
  // we load the scripts into the right environment.
  pushHandle(m_state, handleIndex);
  lua_setupvalue(m_state, -2, 1);

  incrementRecursionLevel();
  int res = pcallWithTraceback(m_state, 0, 0);
  decrementRecursionLevel();
  handleError(m_state, res);
}

LuaDetail::LuaFunctionReturn LuaEngine::contextEval(int handleIndex, String const& lua) {
  int stackSize = lua_gettop(m_state);
  lua_checkstack(m_state, 2);

  // First, try interpreting the lua as an expression by adding "return", then
  // as a statement.  This is the same thing the actual lua repl does.
  int loadRes = luaL_loadstring(m_state, ("return " + lua).utf8Ptr());
  if (loadRes == LUA_ERRSYNTAX) {
    lua_pop(m_state, 1);
    loadRes = luaL_loadstring(m_state, lua.utf8Ptr());
  }
  handleError(m_state, loadRes);

  pushHandle(m_state, handleIndex);
  lua_setupvalue(m_state, -2, 1);

  incrementRecursionLevel();
  int callRes = pcallWithTraceback(m_state, 0, LUA_MULTRET);
  decrementRecursionLevel();
  handleError(m_state, callRes);

  int returnValues = lua_gettop(m_state) - stackSize;
  if (returnValues == 0) {
    return LuaDetail::LuaFunctionReturn();
  } else if (returnValues == 1) {
    return LuaDetail::LuaFunctionReturn(popLuaValue(m_state));
  } else {
    LuaVariadic<LuaValue> ret(returnValues);
    for (int i = returnValues - 1; i >= 0; --i)
      ret[i] = popLuaValue(m_state);
    return LuaDetail::LuaFunctionReturn(ret);
  }
}

LuaValue LuaEngine::contextGetPath(int handleIndex, String path) {
  lua_checkstack(m_state, 2);
  pushHandle(m_state, handleIndex);

  std::string utf8Path = path.takeUtf8();
  char* utf8Ptr = &utf8Path[0];
  size_t utf8Size = utf8Path.size();

  size_t subPathStart = 0;
  for (size_t i = 0; i < utf8Size; ++i) {
    if (utf8Path[i] == '.') {
      utf8Path[i] = '\0';

      lua_getfield(m_state, -1, utf8Ptr + subPathStart);
      lua_remove(m_state, -2);

      if (lua_type(m_state, -1) != LUA_TTABLE) {
        lua_pop(m_state, 1);
        return LuaNil;
      }

      subPathStart = i + 1;
    }
  }

  lua_getfield(m_state, -1, utf8Ptr + subPathStart);
  lua_remove(m_state, -2);

  return popLuaValue(m_state);
}

void LuaEngine::contextSetPath(int handleIndex, String path, LuaValue const& value) {
  lua_checkstack(m_state, 3);
  pushHandle(m_state, handleIndex);

  std::string utf8Path = path.takeUtf8();
  char* utf8Ptr = &utf8Path[0];
  size_t utf8Size = utf8Path.size();

  size_t subPathStart = 0;
  for (size_t i = 0; i < utf8Size; ++i) {
    if (utf8Path[i] == '.') {
      utf8Path[i] = '\0';

      int type = lua_getfield(m_state, -1, utf8Ptr + subPathStart);
      if (type == LUA_TNIL) {
        lua_pop(m_state, 1);
        lua_newtable(m_state);
        lua_pushvalue(m_state, -1);
        lua_setfield(m_state, -3, utf8Ptr + subPathStart);
        lua_remove(m_state, -2);
      } else if (type == LUA_TTABLE) {
        lua_remove(m_state, -2);
      } else {
        lua_pop(m_state, 2);
        throw LuaException("Sub-path in setPath is not nil and is not a table");
      }

      subPathStart = i + 1;
    }
  }

  pushLuaValue(m_state, value);
  lua_setfield(m_state, -2, utf8Ptr + subPathStart);
  lua_pop(m_state, 1);
}

int LuaEngine::popHandle(lua_State* state) {
  lua_xmove(state, m_handleThread, 1);
  return placeHandle();
}

void LuaEngine::pushHandle(lua_State* state, int handleIndex) {
  lua_pushvalue(m_handleThread, handleIndex);
  lua_xmove(m_handleThread, state, 1);
}

int LuaEngine::copyHandle(int handleIndex) {
  lua_pushvalue(m_handleThread, handleIndex);
  return placeHandle();
}

int LuaEngine::placeHandle() {
  if (auto free = m_handleFree.maybeTakeLast()) {
    lua_replace(m_handleThread, *free);
    return *free;

  } else {
    if (m_handleStackMax >= m_handleStackSize) {
      if (!lua_checkstack(m_handleThread, m_handleStackSize)) {
        throw LuaException("Exhausted the size of the handle thread stack");
      }
      m_handleStackSize *= 2;
    }
    m_handleStackMax += 1;
    return m_handleStackMax;
  }
}

LuaFunction LuaEngine::createWrappedFunction(LuaDetail::LuaWrappedFunction function) {
  lua_checkstack(m_state, 2);

  auto funcUserdata = (LuaDetail::LuaWrappedFunction*)lua_newuserdata(m_state, sizeof(LuaDetail::LuaWrappedFunction));
  new (funcUserdata) LuaDetail::LuaWrappedFunction(std::move(function));

  lua_rawgeti(m_state, LUA_REGISTRYINDEX, m_wrappedFunctionMetatableRegistryId);
  lua_setmetatable(m_state, -2);

  auto invokeFunction = [](lua_State* state) {
    auto func = (LuaDetail::LuaWrappedFunction*)lua_touserdata(state, lua_upvalueindex(1));
    auto self = luaEnginePtr(state);

    int argumentCount = lua_gettop(state);
    try {
      // For speed, if the argument count is less than some pre-defined
      // value, use a stack array.
      int const MaxArrayArgs = 8;
      LuaDetail::LuaFunctionReturn res;
      if (argumentCount <= MaxArrayArgs) {
        Array<LuaValue, MaxArrayArgs> args;
        for (int i = argumentCount - 1; i >= 0; --i)
          args[i] = self->popLuaValue(state);
        res = (*func)(*self, argumentCount, args.ptr());
      } else {
        List<LuaValue> args(argumentCount);
        for (int i = argumentCount - 1; i >= 0; --i)
          args[i] = self->popLuaValue(state);
        res = (*func)(*self, argumentCount, args.ptr());
      }

      if (auto val = res.ptr<LuaValue>()) {
        self->pushLuaValue(state, *val);
        return 1;
      } else if (auto vec = res.ptr<LuaVariadic<LuaValue>>()) {
        for (auto const& r : *vec)
          self->pushLuaValue(state, r);
        return (int)vec->size();
      } else {
        return 0;
      }
    } catch (LuaInstructionLimitReached const&) {
      lua_pushlightuserdata(state, &s_luaInstructionLimitExceptionKey);
      return lua_error(state);
    } catch (LuaRecursionLimitReached const&) {
      lua_pushlightuserdata(state, &s_luaRecursionLimitExceptionKey);
      return lua_error(state);
    } catch (std::exception const& e) {
      luaL_where(state, 1);
      lua_pushstring(state, printException(e, true).c_str());
      lua_concat(state, 2);
      return lua_error(state);
    }
  };

  lua_pushcclosure(m_state, invokeFunction, 1);

  return LuaFunction(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), popHandle(m_state)));
}

LuaFunction LuaEngine::createRawFunction(lua_CFunction function) {
  lua_checkstack(m_state, 2);

  lua_pushcfunction(m_state, function);
  return LuaFunction(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), popHandle(m_state)));
}

LuaFunction LuaEngine::createFunctionFromSource(int handleIndex, char const* contents, size_t size, char const* name) {
  lua_checkstack(m_state, 2);

  handleError(m_state, luaL_loadbufferx(m_state, contents, size, name, "t"));

  pushHandle(m_state, handleIndex);
  lua_setupvalue(m_state, -2, 1);

  return LuaFunction(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), popHandle(m_state)));
}

void LuaEngine::pushLuaValue(lua_State* state, LuaValue const& luaValue) {
  lua_checkstack(state, 1);

  struct Pusher {
    LuaEngine* engine;
    lua_State* state;

    void operator()(LuaNilType const&) {
      lua_pushnil(state);
    }

    void operator()(LuaBoolean const& b) {
      lua_pushboolean(state, b);
    }

    void operator()(LuaInt const& i) {
      lua_pushinteger(state, i);
    }

    void operator()(LuaFloat const& f) {
      lua_pushnumber(state, f);
    }

    void operator()(LuaReference const& ref) {
      if (&ref.engine() != engine)
        throw LuaException("lua reference values cannot be shared between engines");
      engine->pushHandle(state, ref.handleIndex());
    }
  };

  luaValue.call(Pusher{this, state});
}

LuaValue LuaEngine::popLuaValue(lua_State* state) {
  lua_checkstack(state, 1);

  LuaValue result;
  starAssert(!lua_isnone(state, -1));
  switch (lua_type(state, -1)) {
    case LUA_TNIL: {
      lua_pop(state, 1);
      break;
    }
    case LUA_TBOOLEAN: {
      result = lua_toboolean(state, -1) != 0;
      lua_pop(state, 1);
      break;
    }
    case LUA_TNUMBER: {
      if (lua_isinteger(state, -1)) {
        result = lua_tointeger(state, -1);
        lua_pop(state, 1);
      } else {
        result = lua_tonumber(state, -1);
        lua_pop(state, 1);
      }
      break;
    }
    case LUA_TSTRING: {
      result = LuaString(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), popHandle(state)));
      break;
    }
    case LUA_TTABLE: {
      result = LuaTable(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), popHandle(state)));
      break;
    }
    case LUA_TFUNCTION: {
      result = LuaFunction(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), popHandle(state)));
      break;
    }
    case LUA_TTHREAD: {
      result = LuaThread(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), popHandle(state)));
      break;
    }
    case LUA_TUSERDATA: {
      if (lua_getmetatable(state, -1) == 0) {
        lua_pop(state, 1);
        throw LuaException("Userdata in popLuaValue missing metatable");
      }
      lua_pop(state, 1);
      result = LuaUserData(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), popHandle(state)));
      break;
    }
    default: {
      lua_pop(state, 1);
      throw LuaException("Unsupported type in popLuaValue");
    }
  }

  return result;
}

void LuaEngine::incrementRecursionLevel() {
  // We reset the instruction count and profiling timing only on the *top
  // level* function entrance, not on recursive entrances.
  if (m_recursionLevel == 0) {
    m_instructionCount = 0;
  }

  if (m_recursionLimit != 0 && m_recursionLevel == m_recursionLimit)
    throw LuaRecursionLimitReached();

  ++m_recursionLevel;
}

void LuaEngine::decrementRecursionLevel() {
  starAssert(m_recursionLevel != 0);
  --m_recursionLevel;
}

void LuaEngine::updateCountHook() {
  if (m_instructionLimit || m_profilingEnabled)
    lua_sethook(m_state, &LuaEngine::countHook, LUA_MASKCOUNT, m_instructionMeasureInterval);
  else
    lua_sethook(m_state, &LuaEngine::countHook, 0, 0);
}

int LuaEngine::s_luaInstructionLimitExceptionKey = 0;
int LuaEngine::s_luaRecursionLimitExceptionKey = 0;

// Hook management implementation

void LuaEngine::registerHook(String const& functionPath, bool before, LuaFunction hook) {
  Logger::info("Hook registered: {} (before={})", functionPath, before);
  registerHookInternal(functionPath, before, std::move(hook));
  wrapPathFunction(functionPath);
}

void LuaEngine::registerHook(LuaFunction target, bool before, LuaFunction hook) {
  registerHookInternal(std::move(target), before, std::move(hook));
}

void LuaEngine::registerHookInternal(String const& functionPath, bool before, LuaFunction hook) {
  auto& hooks = m_hookRegistry[functionPath];
  if (before) {
    hooks.beforeHooks.append(std::move(hook));
    Logger::info("registerHookInternal: added before hook for '{}' (total before hooks: {})", functionPath, hooks.beforeHooks.size());
  } else {
    hooks.afterHooks.append(std::move(hook));
    Logger::info("registerHookInternal: added after hook for '{}' (total after hooks: {})", functionPath, hooks.afterHooks.size());
  }
}

void LuaEngine::registerHookInternal(LuaFunction target, bool before, LuaFunction hook) {
  // Use the function's handle index as a unique identifier
  void* funcPtr = reinterpret_cast<void*>(static_cast<intptr_t>(target.handleIndex()));
  auto& hooks = m_functionHookRegistry[funcPtr];
  if (before)
    hooks.beforeHooks.append(std::move(hook));
  else
    hooks.afterHooks.append(std::move(hook));
}

void LuaEngine::removeHook(String const& functionPath, LuaFunction hook) {
  if (auto hooks = m_hookRegistry.maybe(functionPath)) {
    hooks->beforeHooks.filter([&hook](LuaFunction const& h) {
        return h.handleIndex() != hook.handleIndex();
      });
    hooks->afterHooks.filter([&hook](LuaFunction const& h) {
        return h.handleIndex() != hook.handleIndex();
      });
    if (hooks->beforeHooks.empty() && hooks->afterHooks.empty())
      m_hookRegistry.remove(functionPath);
  }
}

void LuaEngine::removeHook(LuaFunction target, LuaFunction hook) {
  void* funcPtr = reinterpret_cast<void*>(static_cast<intptr_t>(target.handleIndex()));
  if (auto hooks = m_functionHookRegistry.maybe(funcPtr)) {
    hooks->beforeHooks.filter([&hook](LuaFunction const& h) {
        return h.handleIndex() != hook.handleIndex();
      });
    hooks->afterHooks.filter([&hook](LuaFunction const& h) {
        return h.handleIndex() != hook.handleIndex();
      });
    if (hooks->beforeHooks.empty() && hooks->afterHooks.empty())
      m_functionHookRegistry.remove(funcPtr);
  }
}

void LuaEngine::clearHooks(String const& functionPath) {
  m_hookRegistry.remove(functionPath);
}

void LuaEngine::clearHooks(LuaFunction target) {
  void* funcPtr = reinterpret_cast<void*>(static_cast<intptr_t>(target.handleIndex()));
  m_functionHookRegistry.remove(funcPtr);
}

bool LuaEngine::hasHooks(String const& functionPath) const {
  if (auto hooks = m_hookRegistry.maybe(functionPath))
    return !hooks->beforeHooks.empty() || !hooks->afterHooks.empty();
  return false;
}

bool LuaEngine::hasHooks(LuaFunction target) const {
  void* funcPtr = reinterpret_cast<void*>(static_cast<intptr_t>(target.handleIndex()));
  if (auto hooks = m_functionHookRegistry.maybe(funcPtr))
    return !hooks->beforeHooks.empty() || !hooks->afterHooks.empty();
  return false;
}

LuaFunction LuaEngine::wrapFunctionWithHooks(String const& identifier, LuaFunction func) {
  // Check if we've already wrapped this function
  int originalHandle = func.handleIndex();
  if (auto cached = m_wrappedFunctionCache.maybe(originalHandle)) {
    return LuaFunction(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), *cached));
  }

  lua_checkstack(m_state, 10);

  // Store identifier in registry and use index as upvalue
  lua_rawgeti(m_state, LUA_REGISTRYINDEX, m_hookIdentifierRegistryId);
  int identifierIndex = (int)luaL_len(m_state, -1) + 1;
  pushLuaValue(m_state, luaFrom(identifier));
  lua_rawseti(m_state, -2, identifierIndex);
  lua_pop(m_state, 1);

  // Create closure with identifier index and function handle as upvalues
  lua_pushinteger(m_state, identifierIndex);
  lua_pushinteger(m_state, originalHandle);
  lua_pushcclosure(m_state, [](lua_State* state) -> int {
      auto self = luaEnginePtr(state);
      int identifierIndex = (int)lua_tointeger(state, lua_upvalueindex(1));
      int originalHandle = (int)lua_tointeger(state, lua_upvalueindex(2));

      // Get identifier from registry
      lua_rawgeti(state, LUA_REGISTRYINDEX, self->m_hookIdentifierRegistryId);
      lua_rawgeti(state, -1, identifierIndex);
      String identifier = self->luaTo<String>(self->popLuaValue(state));
      lua_pop(state, 1);

      int argc = lua_gettop(state);

      // Collect arguments
      LuaVariadic<LuaValue> args(argc);
      for (int i = argc; i >= 1; --i)
        args[i - 1] = self->popLuaValue(state);

      // Execute hooks and original function
      LuaFunction originalFunc(LuaDetail::LuaHandle(RefPtr<LuaEngine>(self), originalHandle));
      auto result = self->executeHooks(identifier, originalFunc, args);

      // Push return values
      if (auto val = result.ptr<LuaValue>()) {
        self->pushLuaValue(state, *val);
        return 1;
      } else if (auto vec = result.ptr<LuaVariadic<LuaValue>>()) {
        for (auto const& r : *vec)
          self->pushLuaValue(state, r);
        return (int)vec->size();
      } else {
        return 0;
      }
    }, 2);

  int wrappedHandle = popHandle(m_state);
  m_wrappedFunctionCache[originalHandle] = wrappedHandle;
  return LuaFunction(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), wrappedHandle));
}

// Helper to call a function with variadic arguments
LuaDetail::LuaFunctionReturn LuaEngine::callFunctionWithVariadic(LuaEngine* engine, int handleIndex, LuaVariadic<LuaValue> const& args) {
  lua_State* state = engine->m_state;
  lua_checkstack(state, (int)args.size() + 5);
  
  int stackSizeBefore = lua_gettop(state);
  engine->pushHandle(state, handleIndex);
  
  for (auto const& arg : args)
    engine->pushLuaValue(state, arg);
  
  int stackSizeWithArgs = lua_gettop(state);
  int expectedArgs = (int)args.size() + 1; // +1 for function
  if (stackSizeWithArgs != stackSizeBefore + expectedArgs) {
    Logger::error("callFunctionWithVariadic: stack size mismatch! before={}, after push={}, expected={}", 
                  stackSizeBefore, stackSizeWithArgs, stackSizeBefore + expectedArgs);
  }
  
  engine->incrementRecursionLevel();
  int res = engine->pcallWithTraceback(state, (int)args.size(), LUA_MULTRET);
  engine->decrementRecursionLevel();
  
  if (res != LUA_OK) {
    Logger::error("callFunctionWithVariadic: pcall failed with error code {}", res);
    engine->handleError(state, res);
    // After handleError, stack should be back to original size (error message might be on stack)
    // But we should return empty result
    return LuaDetail::LuaFunctionReturn();
  }
  
  int stackSizeAfter = lua_gettop(state);
  int returnValues = stackSizeAfter - stackSizeBefore;
  
  if (returnValues < 0) {
    Logger::error("callFunctionWithVariadic: negative return values! before={}, after={}", 
                  stackSizeBefore, stackSizeAfter);
    return LuaDetail::LuaFunctionReturn();
  }
  
  if (returnValues == 0) {
    return LuaDetail::LuaFunctionReturn();
  } else if (returnValues == 1) {
    return engine->popLuaValue(state);
  } else {
    LuaVariadic<LuaValue> ret(returnValues);
    for (int i = returnValues - 1; i >= 0; --i)
      ret[i] = engine->popLuaValue(state);
    return ret;
  }
}

LuaDetail::LuaFunctionReturn LuaEngine::executeHooks(String const& identifier, LuaFunction func, LuaVariadic<LuaValue> const& args) {
  Logger::info("executeHooks '{}': starting, args.size()={}", identifier, args.size());
  
  // TEMPORARILY DISABLE HOOK EXECUTION TO TEST IF HOOKS ARE THE ISSUE
  // Execute before hooks
  if (auto hooks = m_hookRegistry.maybe(identifier)) {
    Logger::info("executeHooks '{}': SKIPPING {} before hooks (temporarily disabled for testing)", identifier, hooks->beforeHooks.size());
    // for (auto const& hook : hooks->beforeHooks) {
    //   try {
    //     Logger::info("executeHooks '{}': calling before hook", identifier);
    //     // Call hook with original args only
    //     // For hooks with no arguments, use callFunction() directly to preserve execution context
    //     // (same as invoke<void>() but without template conversion issues)
    //     if (args.empty()) {
    //       callFunction(hook.handleIndex());
    //     } else {
    //       auto hookResult = callFunctionWithVariadic(this, hook.handleIndex(), args);
    //       (void)hookResult; // Ignore return value
    //     }
    //     Logger::info("executeHooks '{}': before hook completed", identifier);
    //   } catch (std::exception const& e) {
    //     // Log error but continue - hooks shouldn't break execution
    //     Logger::error("Error executing before hook for '{}': {}", identifier, e.what());
    //   } catch (...) {
    //     Logger::error("Unknown error executing before hook for '{}'", identifier);
    //   }
    // }
  }

  // Call original function
  Logger::info("executeHooks '{}': calling original function", identifier);
  
  // For functions with no arguments, use callFunction() directly to preserve execution context
  // (same as invoke<void>() but without template conversion issues)
  // For functions with arguments, use callFunctionWithVariadic
  LuaDetail::LuaFunctionReturn result;
  if (args.empty()) {
    // No arguments - use callFunction() to match normal path
    Logger::info("executeHooks '{}': calling with callFunction() (no args)", identifier);
    result = callFunction(func.handleIndex());
  } else {
    // Has arguments - use callFunctionWithVariadic
    result = callFunctionWithVariadic(this, func.handleIndex(), args);
  }
  Logger::info("executeHooks '{}': original function completed", identifier);

  // TEMPORARILY DISABLE HOOK EXECUTION TO TEST IF HOOKS ARE THE ISSUE
  // Execute after hooks
  if (auto hooks = m_hookRegistry.maybe(identifier)) {
    Logger::info("executeHooks '{}': SKIPPING {} after hooks (temporarily disabled for testing)", identifier, hooks->afterHooks.size());
    // Check if original function returned a value (not void)
    // bool hasReturnValue = result.ptr<LuaValue>() != nullptr || result.ptr<LuaVariadic<LuaValue>>() != nullptr;
    
    // for (auto const& hook : hooks->afterHooks) {
    //   try {
    //     // Pass result as first argument, then original args
    //     // After hooks can modify the return value by returning a new value
    //     LuaVariadic<LuaValue> afterArgs;
    //     if (auto vec = result.ptr<LuaVariadic<LuaValue>>()) {
    //       // Multiple return values - append all of them
    //       for (auto const& val : *vec)
    //         afterArgs.append(val);
    //     } else if (auto val = result.ptr<LuaValue>()) {
    //       // Single return value
    //       afterArgs.append(*val);
    //     }
    //     // Then append original args
    //     for (auto const& arg : args)
    //       afterArgs.append(arg);
    //     
    //     // For hooks with no arguments, use callFunction() directly to preserve execution context
    //     // (same as invoke<void>() but without template conversion issues)
    //     // Otherwise use callFunctionWithVariadic
    //     LuaDetail::LuaFunctionReturn hookResult;
    //     if (afterArgs.empty()) {
    //       callFunction(hook.handleIndex());
    //       hookResult = LuaDetail::LuaFunctionReturn(); // void return
    //     } else {
    //       hookResult = callFunctionWithVariadic(this, hook.handleIndex(), afterArgs);
    //     }
    //     
    //     // Only allow modification if original function had a return value (not void)
    //     // This prevents breaking void functions like init()
    //     if (hasReturnValue) {
    //       // If hook returns a value, use it as the new result (allowing modification)
    //       // Only modify if hook actually returned something (not nil/empty)
    //       if (auto val = hookResult.ptr<LuaValue>()) {
    //         if (*val != LuaNil) {
    //           result = hookResult;
    //         }
    //       } else if (auto vec = hookResult.ptr<LuaVariadic<LuaValue>>()) {
    //         if (!vec->empty()) {
    //           // Use first return value as new result
    //           result = LuaDetail::LuaFunctionReturn(vec->at(0));
    //         }
    //       }
    //     }
    //     // If hook returns nothing/nil, or original was void, keep original result unchanged
    //   } catch (std::exception const& e) {
    //     // Log error but continue - hooks shouldn't break execution
    //     Logger::error("Error executing after hook for '{}': {}", identifier, e.what());
    //   } catch (...) {
    //     Logger::error("Unknown error executing after hook for '{}'", identifier);
    //   }
    // }
  }

  return result;
}

LuaDetail::LuaFunctionReturn LuaEngine::executeHooks(void* funcPtr, LuaFunction func, LuaVariadic<LuaValue> const& args) {
  // Similar to executeHooks(String), but using function pointer registry
  if (auto hooks = m_functionHookRegistry.maybe(funcPtr)) {
    for (auto const& hook : hooks->beforeHooks) {
      try {
        auto hookResult = callFunctionWithVariadic(this, hook.handleIndex(), args);
        // Ignore hook return value - before hooks are informational only
        (void)hookResult;
      } catch (std::exception const& e) {
        // Log error but continue - hooks shouldn't break execution
        Logger::error("Error executing before hook for function pointer: {}", e.what());
      } catch (...) {
        Logger::error("Unknown error executing before hook for function pointer");
      }
    }
  }

  auto result = callFunctionWithVariadic(this, func.handleIndex(), args);

  // Check if original function returned a value (not void)
  bool hasReturnValue = result.ptr<LuaValue>() != nullptr || result.ptr<LuaVariadic<LuaValue>>() != nullptr;

  if (auto hooks = m_functionHookRegistry.maybe(funcPtr)) {
    for (auto const& hook : hooks->afterHooks) {
      try {
        LuaVariadic<LuaValue> afterArgs;
        if (auto vec = result.ptr<LuaVariadic<LuaValue>>()) {
          // Multiple return values - append all of them
          for (auto const& val : *vec)
            afterArgs.append(val);
        } else if (auto val = result.ptr<LuaValue>()) {
          // Single return value
          afterArgs.append(*val);
        }
        // Then append original args
        for (auto const& arg : args)
          afterArgs.append(arg);
        
        auto hookResult = callFunctionWithVariadic(this, hook.handleIndex(), afterArgs);
        
        // Only allow modification if original function had a return value (not void)
        // This prevents breaking void functions
        if (hasReturnValue) {
          // If hook returns a value, use it as the new result (allowing modification)
          // Only modify if hook actually returned something (not nil/empty)
          if (auto val = hookResult.ptr<LuaValue>()) {
            if (*val != LuaNil) {
              result = hookResult;
            }
          } else if (auto vec = hookResult.ptr<LuaVariadic<LuaValue>>()) {
            if (!vec->empty()) {
              // Use first return value as new result
              result = LuaDetail::LuaFunctionReturn(vec->at(0));
            }
          }
        }
        // If hook returns nothing/nil, or original was void, keep original result unchanged
      } catch (std::exception const& e) {
        // Log error but continue - hooks shouldn't break execution
        Logger::error("Error executing after hook for function pointer: {}", e.what());
      } catch (...) {
        Logger::error("Unknown error executing after hook for function pointer");
      }
    }
  }

  return result;
}

void LuaEngine::wrapPathFunction(String const& path) {
  // Split path by '.' to navigate to the function
  lua_checkstack(m_state, 10);
  
  // Get the global environment
  lua_rawgeti(m_state, LUA_REGISTRYINDEX, m_scriptDefaultEnvRegistryId);
  
  std::string utf8Path = path.utf8();
  char* utf8Ptr = &utf8Path[0];
  size_t utf8Size = utf8Path.size();
  
  size_t subPathStart = 0;
  for (size_t i = 0; i < utf8Size; ++i) {
    if (utf8Path[i] == '.') {
      utf8Path[i] = '\0';
      
      lua_getfield(m_state, -1, utf8Ptr + subPathStart);
      lua_remove(m_state, -2);
      
      if (lua_type(m_state, -1) != LUA_TTABLE) {
        lua_pop(m_state, 1);
        return; // Path doesn't exist or isn't a table
      }
      
      subPathStart = i + 1;
    }
  }
  
  // Get the function at the end of the path
  lua_getfield(m_state, -1, utf8Ptr + subPathStart);
  if (lua_type(m_state, -1) != LUA_TFUNCTION) {
    lua_pop(m_state, 2);
    return; // Not a function
  }
  
  int funcHandle = popHandle(m_state);
  LuaFunction func(LuaDetail::LuaHandle(RefPtr<LuaEngine>(this), funcHandle));
  
  // Check if already wrapped
  if (m_wrappedFunctionCache.contains(funcHandle)) {
    lua_pop(m_state, 1);
    return;
  }
  
  // Wrap the function
  LuaFunction wrapped = wrapFunctionWithHooks(path, func);
  
  // Replace the original function with the wrapped one
  pushHandle(m_state, wrapped.handleIndex());
  lua_setfield(m_state, -2, utf8Ptr + subPathStart);
  
  lua_pop(m_state, 1);
}

void LuaDetail::rawSetField(lua_State* state, int index, char const* key) {
  lua_checkstack(state, 1);

  int absTableIndex = lua_absindex(state, index);
  lua_pushstring(state, key);

  // Move the newly pushed key to the secont to top spot, leaving the value in
  // the top spot.
  lua_insert(state, -2);

  // Pops the value and the key
  lua_rawset(state, absTableIndex);
}

void LuaDetail::rawGetField(lua_State* state, int index, char const* key) {
  lua_checkstack(state, 2);

  int absTableIndex = lua_absindex(state, index);
  lua_pushstring(state, key);

  // Pops the key
  lua_rawget(state, absTableIndex);
}

void LuaDetail::shallowCopy(lua_State* state, int sourceIndex, int targetIndex) {
  lua_checkstack(state, 3);

  int absSourceIndex = lua_absindex(state, sourceIndex);
  int absTargetIndex = lua_absindex(state, targetIndex);

  lua_pushnil(state);
  while (lua_next(state, absSourceIndex) != 0) {
    lua_pushvalue(state, -2);
    lua_insert(state, -2);
    lua_rawset(state, absTargetIndex);
  }
}

LuaTable LuaDetail::insertJsonMetatable(LuaEngine& engine, LuaTable const& table, Json::Type type) {
  auto newIndexMetaMethod = [](LuaTable const& table, LuaValue const& key, LuaValue const& value) {
    auto mt = table.getMetatable();
    auto nils = mt->rawGet<LuaTable>("__nils");

    // If we are setting an entry to nil, need to add a bogus integer entry
    // to the __nils table, otherwise need to set the entry *in* the __nils
    // table to nil and remove it.
    if (value == LuaNil)
      nils.rawSet(key, 0);
    else
      nils.rawSet(key, LuaNil);
    table.rawSet(key, value);
  };

  auto mt = engine.createTable();
  auto nils = engine.createTable();
  mt.rawSet("__nils", nils);
  mt.rawSet("__newindex", engine.createFunction(newIndexMetaMethod));
  mt.rawSet("__typehint", type == Json::Type::Array ? 1 : 2);
  table.setMetatable(mt);
  return nils;
}

LuaTable LuaDetail::jsonContainerToTable(LuaEngine& engine, Json const& container) {
  if (!container.isType(Json::Type::Array) && !container.isType(Json::Type::Object))
    throw LuaException("jsonContainerToTable called on improper json type");

  auto table = engine.createTable();
  auto nils = insertJsonMetatable(engine, table, container.type());

  if (container.isType(Json::Type::Array)) {
    auto vlist = container.arrayPtr();
    for (size_t i = 0; i < vlist->size(); ++i) {
      auto const& val = (*vlist)[i];
      if (val)
        table.rawSet(i + 1, val);
      else
        nils.rawSet(i + 1, 0);
    }
  } else {
    for (auto const& pair : *container.objectPtr()) {
      if (pair.second)
        table.rawSet(pair.first, pair.second);
      else
        nils.rawSet(pair.first, 0);
    }
  }

  return table;
}

Maybe<Json> LuaDetail::tableToJsonContainer(LuaTable const& table) {
  JsonObject stringEntries;
  Map<unsigned, Json> intEntries;
  int typeHint = 0;

  if (auto mt = table.getMetatable()) {
    if (auto th = mt->get<Maybe<int>>("__typehint"))
      typeHint = *th;

    if (auto nils = mt->get<Maybe<LuaTable>>("__nils")) {
      bool failedConversion = false;
      // Nil entries just have a garbage integer as their value
      nils->iterate([&](LuaValue const& key, LuaValue const&) {
        if (auto i = asInteger(key)) {
          intEntries[*i] = Json();
        } else {
          if (auto str = table.engine().luaMaybeTo<String>(key)) {
            stringEntries[str.take()] = Json();
          } else {
            failedConversion = true;
            return false;
          }
        }
        return true;
      });
      if (failedConversion)
        return {};
    }
  }

  bool failedConversion = false;
  table.iterate([&](LuaValue key, LuaValue value) {
      auto jsonValue = table.engine().luaMaybeTo<Json>(value);
      if (!jsonValue) {
        failedConversion = true;
        return false;
      }

      if (auto i = asInteger(key)) {
        intEntries[*i] = jsonValue.take();
      } else {
        auto stringKey = table.engine().luaMaybeTo<String>(std::move(key));
        if (!stringKey) {
          failedConversion = true;
          return false;
        }

        stringEntries[stringKey.take()] = jsonValue.take();
      }

      return true;
    });

  if (failedConversion)
    return {};

  bool interpretAsList = stringEntries.empty()
      && (typeHint == 1 || (typeHint != 2 && !intEntries.empty() && prev(intEntries.end())->first == intEntries.size()));
  if (interpretAsList) {
    JsonArray list;
    for (auto& p : intEntries)
      list.set(p.first - 1, std::move(p.second));
    return Json(std::move(list));
  } else {
    for (auto& p : intEntries)
      stringEntries[toString(p.first)] = std::move(p.second);
    return Json(std::move(stringEntries));
  }
}

Json LuaDetail::jarrayCreate() {
  return JsonArray();
}

Json LuaDetail::jobjectCreate() {
  return JsonObject();
}

LuaTable LuaDetail::jarray(LuaEngine& engine, Maybe<LuaTable> table) {
  if (auto t = table.ptr()) {
    insertJsonMetatable(engine, *t, Json::Type::Array);
    return *t;
  } else {
    return jsonContainerToTable(engine, JsonArray());
  }
}

LuaTable LuaDetail::jobject(LuaEngine& engine, Maybe<LuaTable> table) {
  if (auto t = table.ptr()) {
    insertJsonMetatable(engine, *t, Json::Type::Object);
    return *t;
  } else {
    return jsonContainerToTable(engine, JsonObject());
  }
}


void LuaDetail::jcontRemove(LuaTable const& table, LuaValue const& key) {
  if (auto mt = table.getMetatable()) {
    if (auto nils = mt->rawGet<Maybe<LuaTable>>("__nils"))
      nils->rawSet(key, LuaNil);
  }

  table.rawSet(key, LuaNil);
}

size_t LuaDetail::jcontSize(LuaTable const& table) {
  size_t elemCount = 0;
  size_t highestIndex = 0;
  bool hintList = false;

  if (auto mt = table.getMetatable()) {
    if (mt->rawGet<Maybe<int>>("__typehint") == 1)
      hintList = true;

    if (auto nils = mt->rawGet<Maybe<LuaTable>>("__nils")) {
      nils->iterate([&](LuaValue const& key, LuaValue const&) {
        auto i = asInteger(key);
        if (i && *i >= 0)
          highestIndex = max<int>(*i, highestIndex);
        else
          hintList = false;
        ++elemCount;
      });
    }
  }

  table.iterate([&](LuaValue const& key, LuaValue const&) {
    auto i = asInteger(key);
    if (i && *i >= 0)
      highestIndex = max<int>(*i, highestIndex);
    else
      hintList = false;
    ++elemCount;
  });

  if (hintList)
    return highestIndex;
  else
    return elemCount;
}

void LuaDetail::jcontResize(LuaTable const& table, size_t targetSize) {
  if (auto mt = table.getMetatable()) {
    if (auto nils = mt->rawGet<Maybe<LuaTable>>("__nils")) {
      nils->iterate([&](LuaValue const& key, LuaValue const&) {
        auto i = asInteger(key);
        if (i && *i > 0 && (size_t)*i > targetSize)
          nils->rawSet(key, LuaNil);
      });
    }
  }

  table.iterate([&](LuaValue const& key, LuaValue const&) {
    auto i = asInteger(key);
    if (i && *i > 0 && (size_t)*i > targetSize)
      table.rawSet(key, LuaNil);
  });

  table.set(targetSize, table.get(targetSize));
}

Maybe<LuaInt> LuaDetail::asInteger(LuaValue const& v) {
  if (v.is<LuaInt>())
    return v.get<LuaInt>();
  if (v.is<LuaFloat>()) {
    auto f = v.get<LuaFloat>();
    if ((LuaFloat)(LuaInt)f == f)
      return (LuaInt)f;
    return {};
  }
  /*// Kae: This prevents 1-1 conversion between Lua and Star::Json.
  if (v.is<LuaString>())
    return maybeLexicalCast<LuaInt>(v.get<LuaString>().ptr());
  //*/
  return {};
}

}
