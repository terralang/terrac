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

#include <terra/terra.h>

#include "xopt.h"
#include "filesystem.hpp"

using namespace std;

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
#else
struct _topcheck {
	_topcheck(lua_State *_L, int ret = 0) : top(lua_gettop(_L)) {
		this->L = _L;
		this->ret = ret;
	}

	~_topcheck() {
		assert(lua_gettop(this->L) == (this->top + this->ret));
	}

	const int top;
	int ret;
	lua_State *L;
};

#	define topcheck(...) _topcheck __tc(__VA_ARGS__)
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
		lua_pushstring(L, ip.str().c_str());
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
		lua_pushstring(L, ip.str().c_str());
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
	topts.usemcjit = 0;

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

		lua_pushstring(L, conf.output);            // 1
		lua_pushvalue(L, -4);                      // 2
		if (!get_link_flags(L)) return 1;          // 3
		lua_pushnil(L);                            // 4
		lua_pushboolean(L, (int) !conf.debug);     // 5

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
			target = target.relative(conf.depfile_target);
		}

		df << target.str() << ":";

		for (const string &depstr : *conf.depfiles) {
			filesystem::path dep(depstr);

			if (conf.depfile_target) {
				dep = dep.relative(conf.depfile_target);
			}

			df << " " << dep.str();
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

error:
	if (err) {
		cerr << "terrac: error: " << err << endl;
		status = 2;
		goto exit;
	}

	if (conf.verbosity > 0) {
		for (const filesystem::path &p : *conf.include_dirs) {
			cerr << "terrac: include dir: " << p.str() << endl;
		}

		for (const filesystem::path &p : *conf.lib_dirs) {
			cerr << "terrac: library search path: " << p.str() << endl;
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
