#pragma once

#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>
#include <vector>

#include <common/error.h>
#include <common/interner.h>
#include <compiler/frontend/ast.h>
#include <compiler/frontend/lexer.h>
#include <compiler/frontend/module.h>
#include <compiler/frontend/parser.h>

// a fully parsed module and its resolved import map.
struct LoadedModule {
  std::filesystem::path abs_path; // absolute path to .um file
  std::string rel_path;           // e.g. "game/ecs/world"
  std::string src;                // source text (owned)
  Module mod;
  BodyIR ir;
  TypeAst type_ast;
  // maps alias SymId → index in the modules vector returned by load_modules.
  // key is the alias if given, otherwise the last path segment.
  std::unordered_map<SymId, u32> import_map;
};

// internal DFS state for load_modules.
struct ModuleLoader {
  const std::filesystem::path &root;
  Interner &interner;

  // canonical path → index in `modules` (set after a module is fully loaded).
  std::unordered_map<std::string, u32> visited;
  // canonical paths currently on the DFS stack (for cycle detection).
  std::set<std::string> in_progress;

  std::vector<LoadedModule> modules; // post-order output

  // returns the index of the module in `modules`, or an error.
  Result<u32> load(const std::filesystem::path &abs_path);
};

// load the entry file and all transitive imports.
// the returned vector is topologically ordered: each module appears after all
// its dependencies. the entry module is last.
// root defaults to the directory containing entry_file.
inline Result<std::vector<LoadedModule>>
load_modules(const std::filesystem::path &entry_file,
             const std::filesystem::path &root, Interner &interner) {
  ModuleLoader loader{root, interner};
  auto r = loader.load(std::filesystem::canonical(entry_file));
  if (!r) return std::unexpected(std::move(r.error()));
  return std::move(loader.modules);
}

// convenience overload: infer root from entry_file's parent directory.
inline Result<std::vector<LoadedModule>>
load_modules(const std::filesystem::path &entry_file, Interner &interner) {
  return load_modules(entry_file, entry_file.parent_path(), interner);
}

inline Result<u32>
ModuleLoader::load(const std::filesystem::path &abs_path) {
  std::string canonical_key = abs_path.string();

  // already loaded — return existing index.
  if (auto it = visited.find(canonical_key); it != visited.end())
    return it->second;

  // cycle check.
  if (in_progress.count(canonical_key)) {
    return std::unexpected(Error{.msg = "circular import detected: " + canonical_key});
  }

  // read source.
  std::ifstream f(abs_path);
  if (!f) return std::unexpected(Error{.msg = "cannot open '" + abs_path.string() + "'"});
  std::string src((std::istreambuf_iterator<char>(f)), {});

  // lex.
  KeywordTable kws;
  kws.init(interner);
  auto lex_r = lex_source(src, interner, kws);
  if (!lex_r)
    return std::unexpected(Error{.msg = format_error(lex_r.error(), src, abs_path.string())});

  // parse.
  IntrinsicTable intrinsics;
  intrinsics.init(interner);
  Parser parser(*lex_r, intrinsics);
  parser.parse_module();
  if (parser.error())
    return std::unexpected(
        Error{.msg = format_error(*parser.error(), src, abs_path.string())});

  in_progress.insert(canonical_key);

  // recurse into imports.
  // import_map will map alias SymId → child module index.
  std::unordered_map<SymId, u32> import_map;

  for (const ImportDecl &imp : parser.mod.imports) {
    // build filesystem path from path segments.
    std::filesystem::path rel;
    for (u32 k = 0; k < imp.path_list_count; ++k) {
      SymId seg =
          static_cast<SymId>(parser.mod.sym_list[imp.path_list_start + k]);
      rel /= interner.view(seg);
    }
    rel.replace_extension(".um");

    std::filesystem::path child_abs = root / rel;
    std::filesystem::path child_canonical;
    try {
      child_canonical = std::filesystem::canonical(child_abs);
    } catch (...) {
      in_progress.erase(canonical_key);
      return std::unexpected(Error{.msg = "module not found: '" + child_abs.string() + "'"});
    }

    auto child_r = load(child_canonical);
    if (!child_r) {
      in_progress.erase(canonical_key);
      return std::unexpected(std::move(child_r.error()));
    }
    u32 child_idx = *child_r;

    // determine alias: use `as <alias>` if given, else last segment.
    SymId alias_id = imp.alias;
    if (alias_id == 0) {
      // last segment.
      alias_id = static_cast<SymId>(
          parser.mod.sym_list[imp.path_list_start + imp.path_list_count - 1]);
    }
    import_map[alias_id] = child_idx;
  }

  in_progress.erase(canonical_key);

  // compute a relative path string for the module (relative to root).
  std::string rel_path =
      std::filesystem::relative(abs_path, root).replace_extension("").string();

  u32 idx = static_cast<u32>(modules.size());
  modules.push_back(LoadedModule{
      .abs_path = abs_path,
      .rel_path = std::move(rel_path),
      .src = std::move(src),
      .mod = std::move(parser.mod),
      .ir = std::move(parser.body_ir),
      .type_ast = std::move(parser.type_ast),
      .import_map = std::move(import_map),
  });

  visited[canonical_key] = idx;
  return idx;
}
