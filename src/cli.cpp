/* 
 * File:   cli.cpp
 * Author: alex
 *
 * Created on June 6, 2017, 6:31 PM
 */

#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include <popt.h>

#include "staticlib/io.hpp"
#include "staticlib/json.hpp"
#include "staticlib/utils.hpp"
#include "staticlib/tinydir.hpp"

#include "wilton/wiltoncall.h"
#include "wilton/wilton_signal.h"

#include "wilton/support/exception.hpp"
#include "wilton/support/misc.hpp"

#include "cli_options.hpp"

#define WILTON_QUOTE(value) #value
#define WILTON_STR(value) WILTON_QUOTE(value)
#ifndef WILTON_DEFAULT_SCRIPT_ENGINE
#define WILTON_DEFAULT_SCRIPT_ENGINE duktape
#endif // WILTON_DEFAULT_SCRIPT_ENGINE
#define WILTON_DEFAULT_SCRIPT_ENGINE_STR WILTON_STR(WILTON_DEFAULT_SCRIPT_ENGINE)

namespace { // anonymous

int find_launcher_args_end(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if ("--" ==  std::string(argv[i])) {
            return i;
        }
    }
    return argc;
}

std::string find_exedir() {
    auto exepath = sl::utils::current_executable_path();
    auto exedir = sl::utils::strip_filename(exepath);
    std::replace(exedir.begin(), exedir.end(), '\\', '/');
    return exedir;
}

std::string read_appname(const std::string& appdir) {
    try {
        auto cfile = sl::tinydir::path(appdir + "conf/config.json");
        auto src = cfile.open_read();
        auto json = sl::json::load(src);
        return json["appname"].as_string_nonempty_or_throw();
    } catch(const std::exception&) {
        return std::string();
    }
}

std::tuple<std::string, std::string, std::string> find_startup_module(const std::string& appdir,
        const std::string& opts_startup_module_name, const std::string& startjs) {
    auto startjs_full = sl::tinydir::full_path(startjs);
    auto dir = sl::utils::strip_filename(startjs_full);
    auto mod = std::string();
    if (!opts_startup_module_name.empty()) {
        mod = opts_startup_module_name;
    } else {
        // try to get appname
        if (dir == appdir) {
            mod = read_appname(appdir);
        }
        if (mod.empty()) {
            // fallback to dir name
            mod = sl::utils::strip_parent_dir(dir);
            while(sl::utils::ends_with(mod, "/")) {
                mod.pop_back();
            }
        }
    }
    auto script = sl::utils::strip_parent_dir(startjs_full);
    if (sl::utils::ends_with(script, ".js")) {
        script = script.substr(0, script.length() - 3);
    }
    auto script_id = mod + "/" +script;
    return std::make_tuple(mod, dir, script_id);
}

std::vector<sl::json::field> prepare_paths(
        const std::string& binary_modules_paths, const std::string& startmod, const std::string& startmod_dir) {
    std::vector<sl::json::field> res;
    res.emplace_back(startmod, wilton::support::file_proto_prefix + startmod_dir);
    auto binmods = sl::utils::split(binary_modules_paths, ':');
    for(auto& mod : binmods) {
        if (!sl::utils::ends_with(mod, wilton::support::binmod_postfix)) {
            throw new wilton::support::exception(TRACEMSG("Invalid binary module path specified," +
                    " must be 'path/to/mymod.wlib', path: [" + mod + "]"));
        }
        auto modpath = sl::tinydir::path(mod);
        if (!(modpath.exists() && modpath.is_regular_file())) {
            throw new wilton::support::exception(TRACEMSG("Binary module file not found," +
                    " path: [" + mod + "]"));
        }
        auto modfile = sl::utils::strip_parent_dir(mod);
        auto modname = modfile.substr(0, modfile.length() - wilton::support::binmod_postfix.length());
        auto modfullpath = sl::tinydir::full_path(mod);
        res.emplace_back(modname, wilton::support::zip_proto_prefix + modfullpath);
    }
    return res;
}

void dyload_module(const std::string& name) {
    auto err_dyload = wilton_dyload(name.c_str(), static_cast<int>(name.length()), nullptr, 0);
    if (nullptr != err_dyload) {
        auto msg = TRACEMSG(err_dyload);
        wilton_free(err_dyload);
        throw wilton::support::exception(msg);
    }
}

void init_signals() {
    dyload_module("wilton_signal");
    auto err_init = wilton_signal_initialize();
    if (nullptr != err_init) {
        auto msg = TRACEMSG(err_init);
        wilton_free(err_init);
        throw wilton::support::exception(msg);
    }
}

std::vector<sl::json::field> collect_env_vars(char** envp) {
    auto vec = std::vector<sl::json::field>();
    for (char** el = envp; *el != nullptr; el++) {
        auto var = std::string(*el);
        auto parts = sl::utils::split(var, '=');
        if (2 == parts.size()) {
            vec.emplace_back(parts.at(0), parts.at(1));
        }
    }
    std::sort(vec.begin(), vec.end(), [](const sl::json::field& a, const sl::json::field& b) {
        return a.name() < b.name();
    });
    return vec;
}

std::string write_temp_one_liner(const std::string& exedir, const std::string& deps, const std::string& code) {
    // prepare tmp file path
    auto rsg = sl::utils::random_string_generator();
    auto path_str = exedir + "../work/" + rsg.generate(8) + ".js";
    auto path = sl::tinydir::path(path_str);
    auto tmpl_path = sl::tinydir::path(exedir + "../conf/one-liner-template.js");
    
    // prepare deps
    auto deps_line = std::string();
    auto args_line = std::string();
    if (deps.length() > 0) {
        auto parts = sl::utils::split(deps, ':');
        for (size_t i = 0; i < parts.size(); i++) {
            auto dep = parts.at(i);
            if (dep.length() > 0) {
                deps_line.append("\"").append(dep).append("\"");
                if (i < parts.size() - 1) {
                    deps_line.append(", ");
                }
                auto dep_parts = sl::utils::split(dep, '/');
                if (dep_parts.size() > 0) {
                    auto dep_name = dep_parts.back();
                    args_line.append(dep_name);
                    if (i < parts.size() - 1) {
                        args_line.append(", ");
                    }
                }
            }
        }
    }

    // load and format template
    auto tmpl_src = tmpl_path.open_read();
    auto src = sl::io::make_replacer_source(tmpl_src, {
        {"deps_line", deps_line},
        {"args_line", args_line},
        {"code", code}
    }, [](const std::string& err) {
        std::cerr << err << std::endl;
    });
    auto sink = path.open_write();
    sl::io::copy_all(src, sink);
    return path_str;
}

} // namespace

// valgrind run:
// valgrind --leak-check=yes --show-reachable=yes --track-origins=yes --error-exitcode=42 --track-fds=yes --suppressions=../deps/cmake/resources/valgrind/openssl_malloc.supp  ./bin/wilton_cli ../test/scripts/runWiltonTests.js -m ../../modules.zip

int main(int argc, char** argv, char** envp) {    
    try {
        // parse laucher args
        int launcher_argc = find_launcher_args_end(argc, argv);
        wilton::launcher::cli_options opts(launcher_argc, argv);
        
        // collect app args
        auto apprags = std::vector<std::string>();
        for (int i = launcher_argc + 1; i < argc; i++) {
            apprags.emplace_back(argv[i]);
        }

        // check invalid options
        if (!opts.parse_error.empty()) {
            std::cerr << "ERROR: " << opts.parse_error << std::endl;
            std::cerr << opts.usage() << std::endl;
            return 1;
        }

        // show help
        if (0 != opts.help) {
            std::cout << opts.usage() << std::endl;
            poptPrintHelp(opts.ctx, stdout, 0);
            return 0;
        }                
        
        auto exedir = find_exedir();
        
        // check startup script
        auto startjs = 0 == opts.exec_one_liner ? opts.startup_script : 
                write_temp_one_liner(exedir, opts.exec_deps, opts.exec_code);
        auto tmpcleaner = sl::support::defer([&opts, &startjs]() STATICLIB_NOEXCEPT {
            if (0 != opts.exec_one_liner) {
                std::remove(startjs.c_str());
            }
        });
        auto startjs_path = sl::tinydir::path(startjs);
        if (!startjs_path.exists()) {
            std::cerr << "ERROR: specified script file not found: [" + startjs + "]" << std::endl;
            return 1;
        }
        if(!startjs_path.is_regular_file()) {
            std::cerr << "ERROR: invalid script file specified: [" + startjs + "]" << std::endl;
            return 1;
        }
        
        // check modules dir
        auto moddir = !opts.modules_dir_or_zip.empty() ? opts.modules_dir_or_zip : exedir + "../std.wlib";
        auto modpath = sl::tinydir::path(moddir);
        if (!modpath.exists()) {
            std::cerr << "ERROR: specified modules directory (or wlib bundle) not found: [" + moddir + "]" << std::endl;
            return 1;
        }
        auto modurl = modpath.is_directory() ?
                std::string("file://") + sl::tinydir::full_path(moddir) :
                std::string("zip://") + moddir;
        if (modpath.is_directory() && '/' != modurl.at(modurl.length() - 1)) {
            modurl.push_back('/');
        }

        // get appdir
        auto appdir = !opts.application_dir.empty() ? opts.application_dir : sl::tinydir::full_path(exedir + "../");
        if ('/' != appdir.back()) {
            appdir.push_back('/');
        }
        
        // get startup module
        auto startmod = std::string();
        auto startmod_dir = std::string();
        auto startmod_id = std::string();
        std::tie(startmod, startmod_dir, startmod_id) = find_startup_module(appdir, opts.startup_module_name, startjs);
        if (startmod.empty()) {
            std::cerr << "ERROR: cannot determine startup module name, use '-s' to specify it" << std::endl;
            return 1;
        }

        // prepare paths
        auto paths = prepare_paths(opts.binary_modules_paths, startmod, startmod_dir);

        // get script engine name
        auto script_engine = !opts.script_engine_name.empty() ? opts.script_engine_name : std::string(WILTON_DEFAULT_SCRIPT_ENGINE_STR);

        // env vars
        auto env_vars = collect_env_vars(envp);
        
        // startup call
        auto input = std::string();
        if (0 == opts.load_only) {
            input = sl::json::dumps({
                {"module", startmod_id},
                {"func", "main"},
                {"args", [&apprags] {
                        auto res = std::vector<sl::json::value>();
                        for (auto& st : apprags) {
                            res.emplace_back(st);
                        }
                        return res;
                    }()}
            });
        } else  {
            input = sl::json::dumps({
                {"module", startmod_id}
            });
        }
        // wilton init
        auto config = sl::json::dumps({
            {"defaultScriptEngine", script_engine},
            {"applicationDirectory", appdir},
            {"requireJs", {
                    {"waitSeconds", 0},
                    {"enforceDefine", true},
                    {"nodeIdCompat", true},
                    {"baseUrl", modurl},
                    {"paths", std::move(paths)}
                }
            },
            {"environmentVariables", std::move(env_vars)}
        });
        if (0 != opts.print_config) {
            std::cout << input << std::endl;
            std::cout << config << std::endl;
        }

        // init wilton
        auto err_init = wiltoncall_init(config.c_str(), static_cast<int> (config.length()));
        if (nullptr != err_init) {
            std::cerr << "ERROR: " << err_init << std::endl;
            wilton_free(err_init);
            return 1;
        }

        // load script engine
        dyload_module("wilton_logging");
        dyload_module("wilton_loader");
        dyload_module("wilton_" + script_engine);

        // init signals/ctrl+c to allow their use from js
        init_signals();

        // call script
        char* out = nullptr;
        int out_len = 0;
        char* err_run = wiltoncall_runscript(script_engine.c_str(), static_cast<int>(script_engine.length()),
                input.c_str(), static_cast<int> (input.length()), &out, &out_len);
        if (nullptr != err_run) {
            std::cerr << "ERROR: " << err_run << std::endl;
            wilton_free(err_run);
            return 1;
        }
        wilton_free(out);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
}

