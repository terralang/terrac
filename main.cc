#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <list>
#include <filesystem>

#include <terra/terra.h>

#include "xopt.h"

#ifdef _WIN32
#	define TERRA_PATHSEP ';'
#else
#	define TERRA_PATHSEP ':'
#endif

using namespace std;

const char * const DEFAULT_MODPATH =
	"/usr/share/terra/modules"
	":/usr/local/share/terra/modules";

// https://github.com/stevedonovan/Penlight/blob/d90956418cdf9e315719a7e4ce314db6b512ae00/lua/pl/compat.lua#L145-L155
// MIT License
// TODO convert to C
const char * const SEARCHPATH_LUA =
	"if not package.searchpath then\n"
	"    local sep = package.config:sub(1,1)\n"
	"    function package.searchpath (mod,path)\n"
	"        mod = mod:gsub('%.',sep)\n"
	"        for m in path:gmatch('[^;]+') do\n"
	"            local nm = m:gsub('?',mod)\n"
	"            local f = io.open(nm,'r')\n"
	"            if f then f:close(); return nm end\n"
	"        end\n"
	"    end\n"
	"end\n";

struct config {
	int verbosity;
	bool help;
	bool debug;
	const char *filename;
	const char *output;
	const char *depfile;
	const char *depfile_target;
	unique_ptr<list<string>> depfiles;
	unique_ptr<list<filesystem::path>> include_dirs;
	unique_ptr<list<filesystem::path>> lib_dirs;
	unique_ptr<list<string>> libs;
	unique_ptr<list<filesystem::path>> modulepaths;
	bool nosysmods;
};

static void on_verbose(const char *v, void *data, const struct xoptOption *option, bool longArg, const char **err) {
	(void) v;
	(void) option;
	(void) err;
	(void) longArg;

	config *conf = (config *) data;

	assert(!longArg);

	if (conf->verbosity < 3) {
		++conf->verbosity;
	}
}

static void on_multi_path(const char *v, void *data, const struct xoptOption *option, bool longArg, const char **err) {
	(void) option;
	(void) longArg;
	(void) err;

	(*((unique_ptr<list<filesystem::path>> *)((char *) data + option->offset)))->push_back(filesystem::path(v));
}

static void on_multi_string(const char *v, void *data, const struct xoptOption *option, bool longArg, const char **err) {
	(void) option;
	(void) longArg;
	(void) err;

	(*((unique_ptr<list<string>> *)((char *) data + option->offset)))->push_back(v);
}

static xoptOption options[] = {
	{
		"output",
		'o',
		offsetof(config, output),
		0,
		XOPT_TYPE_STRING,
		0,
		"If specified, outputs the terra code to the given filename"
	},
	{
		"include-dir",
		'I',
		offsetof(config, include_dirs),
		&on_multi_path,
		XOPT_TYPE_STRING,
		"dir",
		"Adds a search path for C header files (can be passed multiple times)"
	},
	{
		"lib-dir",
		'L',
		offsetof(config, lib_dirs),
		&on_multi_path,
		XOPT_TYPE_STRING,
		"dir",
		"Adds a search path for libraries (can be passed multiple times)"
	},
	{
		"lib",
		'l',
		offsetof(config, libs),
		&on_multi_string,
		XOPT_TYPE_STRING,
		"name",
		"Specifies a library to be linked against the resulting binary"
	},
	{
		"mod-dir",
		'm',
		offsetof(config, modulepaths),
		&on_multi_path,
		XOPT_TYPE_STRING,
		"dir",
		"Adds a module search path"
	},
	{
		"nostdmod",
		0,
		offsetof(config, nosysmods),
		0,
		XOPT_TYPE_BOOL,
		0,
		"If specified, default system module paths are omitted from the modpath"
	},
	{
		"depfile",
		'D',
		offsetof(config, depfile),
		0,
		XOPT_TYPE_STRING,
		0,
		"If specified, emits a Ninja-compatible depfile with all included files during the build"
	},
	{
		"depfile-path",
		'P',
		offsetof(config, depfile_target),
		0,
		XOPT_TYPE_STRING,
		0,
		"If specified, all depfile paths are relativized to this path"
	},
	{
		0, // only allow -v[v[v]]
		'v',
		offsetof(config, verbosity),
		&on_verbose,
		XOPT_TYPE_BOOL,
		0,
		"Increase verbosity (default level 0, max level 3)"
	},
	{
		"debug",
		'g',
		offsetof(config, debug),
		0,
		XOPT_TYPE_BOOL,
		0,
		"Enable debugging information"
	},
	{
		"help",
		'h',
		offsetof(config, help),
		0,
		XOPT_TYPE_BOOL,
		0,
		"Shows this help message"
	},
	XOPT_NULLOPTION
};

#ifdef NDEBUG
#	define topcheck(...) ((void)0)
#	define disarmtopcheck() ((void)0)
#else
struct _topcheck {
	_topcheck(lua_State *_L, int ret = 0) : top(lua_gettop(_L)) {
		this->armed = true;
		this->L = _L;
		this->ret = ret;
	}

	~_topcheck() {
		if (armed) {
			assert(lua_gettop(this->L) == (this->top + this->ret));
		}
	}

	bool armed;
	const int top;
	int ret;
	lua_State *L;
};

#	define topcheck(...) _topcheck __tc(__VA_ARGS__)
#	define disarmtopcheck() __tc.armed = false
#endif

static int errfn_ref = -1;

static int errfn(lua_State *L) {
	topcheck(L, 1);
	lua_getfield(L, LUA_GLOBALSINDEX, "debug");
	lua_getfield(L, -1, "traceback");
	lua_remove(L, -2);
	lua_pushvalue(L, 1);
	lua_pushinteger(L, 2);
	lua_call(L, 2, 1);
	return 1;
}

static bool is_terrafn(lua_State *L) {
	topcheck(L);

	assert(!lua_isnil(L, -2));
	assert(!lua_isnil(L, -1));

	if (!lua_isstring(L, -2)) {
		return false;
	}

	lua_getglobal(L, "terralib");
	assert(!lua_isnil(L, -1));

	lua_getfield(L, -1, "type");
	assert(!lua_isnil(L, -1));

	lua_pushvalue(L, -3);

	string where = " (at global '" + string(lua_tostring(L, -5)) + "')";

	if (lua_pcall(L, 1, 1, 0) != 0) {
		cerr << "terrac: call to terralib.type() failed: " << lua_tostring(L, -1) << where << endl;
		lua_pop(L, 2);
		return false;
	}

	string type = lua_tostring(L, -1);
	lua_pop(L, 2);

	return type == "terrafunction" || type == "terraglobalvariable";
}

static void resolve_path(lua_State *L, const char *pathname) {
	topcheck(L, 1);

	lua_getglobal(L, "package");
	assert(!lua_isnil(L, -1));
	lua_getfield(L, -1, "searchpath");
	assert(lua_isfunction(L, -1));

	lua_pushvalue(L, -3);
	lua_getfield(L, -3, pathname);
	assert(!lua_isnil(L, -1));
	lua_remove(L, -4);

	if (lua_pcall(L, 2, 1, 0) != 0) {
		cerr << "terrac: call to package.searchpath() failed: " << lua_tostring(L, -1) << endl;
		lua_pop(L, 1);
		lua_pushnil(L);
		return;
	}

	if (lua_isnil(L, -1)) {
		// not found
		return;
	}

	// get realpath
	char *rpth = realpath(lua_tostring(L, -1), NULL);

	if (rpth == NULL) {
		string where = "call to realpath() failed (module path: " + string(lua_tostring(L, -1)) + ")";
		perror(where.c_str());
	}

	lua_pop(L, 1);

	if (rpth == NULL) {
		lua_pushnil(L);
	} else {
		lua_pushstring(L, rpth);
		free(rpth);
	}
}

static int on_loaded_newindex(lua_State *L) {
	assert(lua_gettop(L) == 3); // we expect 3 args
	lua_pushvalue(L, 2);
	lua_pushvalue(L, 3);
	lua_rawset(L, 1);

	config *conf = (config *) lua_touserdata(L, lua_upvalueindex(1));

	if (conf->verbosity > 0) {
		cerr << "terrac: detected module: " << lua_tostring(L, 2) << endl;
	}

	lua_pushvalue(L, 2);

	resolve_path(L, "path");

	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		resolve_path(L, "cpath");
	}

	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		resolve_path(L, "terrapath");
	}

	lua_remove(L, -2);

	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		cerr << "terrac: could not resolve module (skipping): " << lua_tostring(L, 2) << endl;
		return 0;
	}

	if (conf->verbosity > 0) {
		cerr << "terrac: resolved " << lua_tostring(L, 2) << " -> " << lua_tostring(L, -1) << endl;
	}

	if (conf->depfile) {
		conf->depfiles->push_back(lua_tostring(L, -1));
	}

	lua_pop(L, 1);

	return 0;
}

static bool get_terrac(lua_State *L) {
	topcheck(L, 1);

	lua_getglobal(L, "terrac");
	if (lua_isnil(L, -1)) {
		cerr << "terrac: ERROR: global `terrac` not found" << endl;
		return false;
	}

	return true;
}

static bool get_link_flags(lua_State *L) {
	topcheck(L, 1);

	if (!get_terrac(L)) {
		return false;
	}

	lua_getfield(L, -1, "link_flags");
	lua_remove(L, -2);

	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
	} else if (!lua_istable(L, -1)) {
		cerr << "terrac: WARNING: terrac.link_flags is not nil and is not a table - replacing with empty table (must be table or nil)" << endl;
		lua_pop(L, 1);
		lua_newtable(L);
	}

	return true;
}

static bool get_cflags(lua_State *L) {
	topcheck(L, 1);

	if (!get_terrac(L)) {
		return false;
	}

	lua_getfield(L, -1, "c_flags");
	lua_remove(L, -2);

	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
	} else if (!lua_istable(L, -1)) {
		cerr << "terrac: WARNING: terrac.c_flags is not nil and is not a table - replacing with empty table (must be table or nil)" << endl;
		lua_pop(L, 1);
		lua_newtable(L);
	}

	return true;
}

static bool inject_link_flags(lua_State *L, config &conf) {
	topcheck(L);

	if (!get_link_flags(L)) {
		lua_pop(L, 1);
		return false;
	}

	size_t n = lua_objlen(L, -1);

	for (const filesystem::path &ip : *conf.lib_dirs) {
		lua_pushinteger(L, ++n);
		lua_pushstring(L, "-L");
		lua_settable(L, -3);

		lua_pushinteger(L, ++n);
		lua_pushstring(L, ip.c_str());
		lua_settable(L, -3);
	}

	for (const string &l : *conf.libs) {
		string fullarg = "-l" + l;

		lua_pushinteger(L, ++n);
		lua_pushstring(L, fullarg.c_str());
		lua_settable(L, -3);
	}

	lua_pop(L, 1);

	return true;
}

static filesystem::path mod_to_path(string mod) {
	istringstream iss(mod);
	filesystem::path result;
	string leaf;
	while (getline(iss, leaf, '.')) {
		result.push_back(leaf);
	}
	return result;
}

static filesystem::path mod_to_relpath(string mod) {
	assert(mod[0] == '.');

	filesystem::path result;
	size_t i = 0;
	while (mod[i] == '.') {
		result.push_back("..");
		++i;
	}

	return (result / mod_to_path(mod.substr(i)));
}

static void pathenv_to_paths(string pathenv, vector<filesystem::path> &paths) {
	istringstream iss(pathenv);
	string path;
	while (getline(iss, path, TERRA_PATHSEP)) {
		paths.emplace_back(path);
	}
}

static int terra_loadmodule(lua_State *L) {
	/*
		The only parameter that is required is
		the first one - the module name. It must
		also not be empty.

		The second parameter - the origin file - is
		only required if the module is a relative
		path.

		The third parameter - the original require()
		function - is only used if passed and provides
		a fallback mechanism for requiring files.
	*/
	config &conf = *((config *)lua_touserdata(L, lua_upvalueindex(1)));

	string mod = luaL_checkstring(L, 1);
	string origin = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
	stringstream reason;
	string terramodpath;
	vector<filesystem::path> terrapaths;
	filesystem::path modpath;
	filesystem::path trypath;

	if (mod.empty()) {
		luaL_error(L, "module path cannot be empty");
		return 0; /* not hit */
	}

	// Fail fast: valid module paths are only [.a-z0-9_-]i
	for (int i = 0, len = mod.length(); i < len; i++) {
		char c = mod[i];
		if (!(
			(c >= 'a' && c <= 'z') ||
			(c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') ||
			c == '.' || c == '_' || c == '-'))
		{
			reason << "\n\tnot a valid module string (contains invalid characters)";
			goto fallback_loader;
		}
	}

	if (mod[0] == '.') {
		if (origin.empty()) {
			reason << "\n\tmodule looked like a relative path but no origin was provided";
			goto fallback_loader;
		}

		modpath = mod_to_relpath(mod);

		trypath = (filesystem::path(origin) / modpath.with_extension(".t")).resolve(false);
		if (trypath.exists()) {
			modpath = trypath;
			goto resolved;
		}
		reason << "\n\tno terra module '" << trypath << "'";

		trypath = (filesystem::path(origin) / modpath / "init.t").resolve(false);
		if (trypath.exists()) {
			modpath = trypath;
			goto resolved;
		}
		reason << "\n\tno terra module '" << trypath << "'";
	} else {
		// get the path
		lua_getglobal(L, "terralib");
		lua_getfield(L, -1, "modpath");
		terramodpath = lua_isnil(L, -1) ? "" : lua_tostring(L, -1);
		lua_pop(L, 2);

		if (terramodpath.empty()) {
			reason << "\n\tterralib.modpath is empty";
			goto fallback_loader;
		}

		modpath = mod_to_path(mod);
		pathenv_to_paths(terramodpath, terrapaths);

		for (const auto &path : terrapaths) {
			trypath = (path / modpath).with_extension(".t");
			if (trypath.exists()) {
				modpath = trypath;
				goto resolved;
			}
			reason << "\n\tno terra module '" << trypath << "'";

			trypath = path / modpath / "init.t";
			if (trypath.exists()) {
				modpath = trypath;
				goto resolved;
			}
			reason << "\n\tno terra module '" << trypath << "'";
		}
	}

fallback_loader:
	if (lua_isfunction(L, 3)) {
		// try to load it
		lua_pushvalue(L, 3);
		lua_pushvalue(L, 1);
		if (lua_pcall(L, 1, 1, 0) != 0) {
			// better error message opportunity here
			luaL_error(L, "terra module '%s' not found:%s\ndefault loader also failed: %s",
				lua_tostring(L, 1),
				reason.str().c_str(),
				lua_tostring(L, -1));
			return 0; /* not hit */
		}

		// success!
		conf.depfiles->push_back(lua_tostring(L, 1));
		return 1;
	}

	luaL_error(L, "could not find module '%s'");
	return 0; /* not hit */

resolved:
	if (terra_loadfile(L, modpath.str().c_str())) {
		lua_error(L);
		return 0; /* not hit */
	}

	lua_call(L, 0, 1);

	// success!
	conf.depfiles->push_back(modpath.str());
	return 1;
}

static int try_module_load(lua_State *L) {
	topcheck(L, 1);
	lua_getglobal(L, "terralib");
	lua_getfield(L, -1, "loadmodule");
	lua_remove(L, -2);
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		disarmtopcheck();
		luaL_error(L, "terralib.loadmodule is not a function");
		return 0; /* not hit */
	}
	lua_pushvalue(L, 1); // the module being require()'d
	lua_pushvalue(L, lua_upvalueindex(1)); // the origin filename
	lua_pushvalue(L, lua_upvalueindex(2)); // the original (lua-provided) require()
	lua_call(L, 3, 1);
	return 1;
}

static int make_module_loader(lua_State *L) {
	topcheck(L, 1);

	if (string(lua_tostring(L, 2)) != "require") {
		// fallback to regular index
		lua_pushvalue(L, lua_upvalueindex(1));
		lua_pushvalue(L, 1);
		lua_pushvalue(L, 2);
		lua_call(L, 2, 1);
		return 1;
	}

	lua_Debug dbg;
	assert(lua_getstack(L, 1, &dbg) == 1);
	assert(lua_getinfo(L, "S", &dbg) != 0);

	const char *source = dbg.source;
	if (*source == '@') {
		++source;
	} else {
		// @ signifies a module-loadable chunk.
		// this chunk doesn't start with it,
		// so give it the old require.
		lua_pushvalue(L, lua_upvalueindex(2));
		return 1;
	}

	lua_pushstring(L, source);
	lua_pushvalue(L, lua_upvalueindex(2));
	lua_pushcclosure(L, &try_module_load, 2);

	return 1;
}

static void inject_mod_loader(lua_State *L, config &conf) {
	topcheck(L);

	// let's tango.
	//
	// - push original _G.__index and _G.require as upvalues to make_module_loader
	// - set make_module_loader as _G.__index
	// - nil-out _G.require in order to start triggering new module loader
	lua_getglobal(L, "_G");
	lua_getmetatable(L, -1);
	lua_getfield(L, -1, "__index");
	lua_getfield(L, -3, "require");
	assert(!lua_isnil(L, -1));
	lua_pushcclosure(L, &make_module_loader, 2);
	lua_setfield(L, -2, "__index");
	lua_pushnil(L);
	lua_setfield(L, -3, "require");
	lua_pop(L, 2);

	// install the default module loader
	lua_getglobal(L, "terralib");
	lua_pushlightuserdata(L, &conf);
	lua_pushcclosure(L, &terra_loadmodule, 1);
	lua_setfield(L, -2, "loadmodule");
	lua_pop(L, 1);
}

/*
static void print_table(lua_State *L, int t) {
	topcheck(L);

	size_t len = lua_objlen(L, t);

	cerr << "\x1b[35mBEGIN TABLE: " << len << " elements" << endl;

	for (size_t i = 1; i <= len; i++) {
		lua_pushinteger(L, i);
		lua_gettable(L, t - 1);
		cerr << "\t" << i << ": " << lua_tostring(L, -1) << endl;
		lua_pop(L, 1);
	}

	cerr << "END TABLE.\x1b[m" << endl;
}
*/

static bool inject_cflags(lua_State *L, config &conf) {
	topcheck(L);

	if (!get_cflags(L)) {
		lua_pop(L, 1);
		return false;
	}

	size_t n = lua_objlen(L, -1);

	for (const filesystem::path &ip : *conf.include_dirs) {
		lua_pushinteger(L, ++n);
		lua_pushstring(L, "-I");
		lua_settable(L, -3);

		lua_pushinteger(L, ++n);
		lua_pushstring(L, ip.c_str());
		lua_settable(L, -3);
	}

	lua_pop(L, 1);

	return true;
}

static int table_assign(lua_State *L) {
	int nargs = lua_gettop(L);

	luaL_checktype(L, 1, LUA_TTABLE);
	for (int i = 2; i <= nargs; i++) {
		luaL_checktype(L, i, LUA_TTABLE);
	}

	topcheck(L, 1);

	for (int i = 2; i <= nargs; i++) {
		lua_pushnil(L);
		while (lua_next(L, i)) {
			lua_pushvalue(L, -2);
			lua_pushvalue(L, -2);
			lua_settable(L, 1);
			lua_pop(L, 1);
		}
	}

	lua_pushvalue(L, 1);
	return 1;
}

/*
	can be used for both includec and includecstring as it's
	just a simple proxy.

	if terra changes the prototype between the two, this will
	have to be updated.
*/
static int terrac_includec(lua_State *L) {
	luaL_checkstring(L, 1);

	size_t nargs = lua_gettop(L);

	/*
		"artificial" means that we're artifically
		injecting the second parameter where the Lua
		function was caled without one.
	*/
	int artificial = 0;
	if (nargs < 2) {
		artificial = 1;
		nargs = 2;
	}

	// upvalue 1 has the original includec[string] function
	lua_pushvalue(L, lua_upvalueindex(1));
	assert(!lua_isnil(L, -1));

	for (size_t i = 1; i <= nargs; i++) {
		if (i == 2) {
			topcheck(L, 1);

			size_t n = 0;
			lua_newtable(L);

			// cflags object
			get_cflags(L);
			assert(!lua_isnil(L, -1));
			size_t cn = lua_objlen(L, -1);
			for (size_t j = 1; j <= cn; j++) {
				topcheck(L);
				lua_pushinteger(L, ++n); // args_k = ++n
				lua_pushinteger(L, j);   // cflag_k = j
				lua_gettable(L, -3);     // push(cflags[cflag_k])
				lua_settable(L, -4);     // args[args_k] = pop()
			}
			lua_pop(L, 1);

			// args object
			if (!artificial && !lua_isnil(L, i)) {
				luaL_checktype(L, i, LUA_TTABLE);
				cn = lua_objlen(L, i);
				for (size_t j = 1; j <= cn; j++) {
					topcheck(L);
					lua_pushinteger(L, ++n); // args_k = ++n
					lua_pushinteger(L, j);   // cflag_k = j
					lua_gettable(L, i);      // push(cflags[cflag_k])
					lua_settable(L, -3);     // args[args_k] = pop()
				}
			}

			continue;
		}

		lua_pushvalue(L, i);
	}

	if (lua_pcall(L, nargs, LUA_MULTRET, 0) != 0) {
		lua_error(L);
		return 0;
	}

	return lua_gettop(L) - (nargs - artificial);
}

static bool inject_includec(lua_State *L, config& conf) {
	topcheck(L);

	lua_getglobal(L, "terralib");
	assert(!lua_isnil(L, -1));

	lua_getfield(L, -1, "includec");
	assert(!lua_isnil(L, -1));
	lua_pushlightuserdata(L, (void *) &conf);
	lua_pushcclosure(L, &terrac_includec, 2);
	lua_setfield(L, -2, "includec");

	lua_getfield(L, -1, "includecstring");
	assert(!lua_isnil(L, -1));
	lua_pushlightuserdata(L, (void *) &conf);
	lua_pushcclosure(L, &terrac_includec, 2);
	lua_setfield(L, -2, "includecstring");

	lua_pop(L, 1);

	return true;
}

int pmain(config &conf) {
	int status = 0;

	assert(conf.filename != nullptr);

	// init environment
	lua_State *L = luaL_newstate();
	if (L == NULL) {
		cerr << "terrac: memory allocation for LuaJIT state failed" << endl;
		return 42;
	}

	// load libraries
	luaL_openlibs(L);

	// patch package.searchpath
	if (luaL_dostring(L, SEARCHPATH_LUA) != 0) {
		cerr << "terrac: could not patch package.searchpath(): " << lua_tostring(L, -1) << endl;
		return 1;
	}

	// configure Terra
	terra_Options topts;
	topts.verbose = conf.verbosity == 0 ? 0 : conf.verbosity - 1;
	topts.debug = (int) conf.debug;

	terra_initwithoptions(L, &topts);

	// set error handler ref
	lua_pushcfunction(L, &errfn);
	errfn_ref = lua_gettop(L);

	// override loaded table to detect dependencies
	{
		topcheck(L);
		lua_getglobal(L, "package");
		assert(!lua_isnil(L, -1));
		lua_getfield(L, -1, "loaded");
		lua_newtable(L);
		lua_pushlightuserdata(L, (void *) &conf);
		lua_pushcclosure(L, &on_loaded_newindex, 1);
		lua_setfield(L, -2, "__newindex");
		lua_setmetatable(L, -2);
		lua_pop(L, 2);
	}

	// inject module path
	{
		topcheck(L);
		stringstream modpath;

		char *terramodpath = getenv("TERRA_MODPATH");
		if (terramodpath != NULL) {
			modpath << TERRA_PATHSEP << terramodpath;
		}

		for (const auto &mpath : *conf.modulepaths) {
			modpath << TERRA_PATHSEP << mpath;
		}

		if (!conf.nosysmods) {
			modpath << TERRA_PATHSEP << DEFAULT_MODPATH;
		}

		string modpaths = modpath.str().substr(1);

		lua_getglobal(L, "terralib");
		lua_pushstring(L, modpaths.c_str());
		lua_setfield(L, -2, "modpath");
		lua_pop(L, 1);
	}

	// inject module loader
	{
		topcheck(L);
		inject_mod_loader(L, conf);
	}

	// create terrac object
	{
		topcheck(L);
		lua_newtable(L);
		lua_newtable(L);
		lua_setfield(L, -2, "c_flags");
		lua_newtable(L);
		lua_setfield(L, -2, "link_flags");
		lua_setglobal(L, "terrac");
	}

	// inject configuration
	{
		topcheck(L);
		if (!inject_cflags(L, conf)) return 1;
		if (!inject_link_flags(L, conf)) return 1;
	}

	// inject includec replacements
	{
		topcheck(L);
		if (!inject_includec(L, conf)) return 1;
	}

	// inject table.assign
	{
		topcheck(L);
		lua_getglobal(L, "table");
		assert(!lua_isnil(L, -1));
		lua_pushcfunction(L, &table_assign);
		lua_setfield(L, -2, "assign");
		lua_pop(L, 1);
	}

	// run the file
	if ((terra_loadfile(L, conf.filename) || lua_pcall(L, 0, LUA_MULTRET, errfn_ref))) {
		cerr << "terrac: terra error: " << lua_tostring(L, -1) << endl;
		return 1;
	}

	if (conf.output) {
		topcheck(L);

		// enumerate public globals
		lua_newtable(L);
		lua_pushnil(L);
		while (lua_next(L, LUA_GLOBALSINDEX) != 0) {
			if (is_terrafn(L)) {
				if (conf.verbosity > 0) {
					cerr << "terrac: export: " << lua_tostring(L, -2) << endl;
				}

				lua_pushvalue(L, -2);
				lua_pushvalue(L, -2);
				lua_settable(L, -5);
			}

			lua_pop(L, 1);
		}

		// compile it
		lua_getglobal(L, "terralib");
		assert(!lua_isnil(L, -1));

		lua_getfield(L, -1, "saveobj");
		assert(!lua_isnil(L, -1));

		lua_pushstring(L, conf.output);
		lua_pushvalue(L, -4);
		if (!get_link_flags(L)) return 1;
		lua_pushnil(L);
		lua_pushboolean(L, (int) !conf.debug);

		if (conf.verbosity > 0) {
			cerr << "terrac: exporting public symbols to " << conf.output << endl;
		}

		if (lua_pcall(L, 5, 0, errfn_ref) != 0) {
			cerr << "terrac: call to terralib.saveobj() failed: " << lua_tostring(L, -1) << endl;
			lua_pop(L, 1);
			status = 1;
		}

		lua_pop(L, 2);
	}

	if (conf.depfile) {
		ofstream df;
		df.open(conf.depfile, ios::trunc | ios::out);
		if (df.bad()) {
			cerr << "terrac: failed to open depfile '" << conf.depfile << "' for writing: " << strerror(errno) << endl;
			status = 1;
			goto cleanup;
		}

		filesystem::path target;

		if (conf.output) {
			target = conf.output;
		} else {
			target = conf.filename;
		}

		if (conf.depfile_target) {
			target = filesystem::relative(target, conf.depfile_target);
		}

		df << target.string() << ":";

		for (const string &depstr : *conf.depfiles) {
			filesystem::path dep(depstr);

			if (conf.depfile_target) {
				dep = filesystem::relative(dep, conf.depfile_target);
			}

			df << " " << dep.string();
		}

		df << endl;
		df.close();
	}

	// cleanup
cleanup:
	terra_llvmshutdown();
	lua_close(L);

	return status;
}

int main(int argc, const char **argv) {
	int status = 1;
	const char *err = nullptr;
	int extrac = 0;
	const char **extrav = nullptr;
	string absfilename;

	config conf;
	conf.help = false;
	conf.filename = nullptr;
	conf.debug = false;
	conf.verbosity = 0;
	conf.output = nullptr;
	conf.depfile = nullptr;
	conf.depfile_target = nullptr;
	conf.include_dirs.reset(new list<filesystem::path>());
	conf.lib_dirs.reset(new list<filesystem::path>());
	conf.libs.reset(new list<string>());
	conf.depfiles.reset(new list<string>());
	conf.modulepaths.reset(new list<filesystem::path>());
	conf.nosysmods = false;

	XOPT_SIMPLE_PARSE(
		argv[0],
		XOPT_CTX_SLOPPYSHORTS,
		&options[0], &conf,
		argc, argv,
		&extrac, &extrav,
		&err,
		stderr,
		"[-h] [--] file.t",
		"Unofficial Terra compiler",
		nullptr,
		14);

	if (err) {
		goto error;
	}

	if (extrac != 1) {
		err = "expected exactly one filename";
		goto error;
	}

	if (extrav[0][0] == 0) {
		err = "specified filename is an empty string";
		goto error;
	}

	conf.filename = extrav[0];

	// get absolute filename of input
	// this is important since the module loader has to
	// be able to navigate the filesystem efficiently.
	absfilename = filesystem::path(conf.filename).resolve().str();
	conf.filename = absfilename.c_str();

error:
	if (err) {
		cerr << "terrac: error: " << err << endl;
		status = 2;
		goto exit;
	}

	if (conf.verbosity > 0) {
		for (const filesystem::path &p : *conf.include_dirs) {
			cerr << "terrac: include dir: " << p << endl;
		}

		for (const filesystem::path &p : *conf.lib_dirs) {
			cerr << "terrac: library search path: " << p << endl;
		}
	}

	status = pmain(conf);

exit:
	if (extrav != nullptr) {
		free(extrav);
	}

	return status;

xopt_help:
	status = 2;
	goto exit;
}
