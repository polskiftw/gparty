from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise RuntimeError(f"expected patch anchor missing in {path}: {old[:120]!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


# Registry API/status.
path = ROOT / "apps/gfingerd/src/registry.hpp"
replace_once(
    path,
    "  std::size_t unsupported{};\n  std::size_t failed{};",
    "  std::size_t unsupported{};\n  std::size_t deferred_gifs{};\n  std::size_t failed{};",
)
replace_once(
    path,
    "  void record_failure(const gdupe::RemoteObject &object,\n"
    "                      const std::string &error, int maximum_attempts);",
    "  void record_failure(const gdupe::RemoteObject &object,\n"
    "                      const std::string &error, int maximum_attempts);\n"
    "  void defer_gif(const gdupe::RemoteObject &object,\n"
    "                 const std::filesystem::path &local_path,\n"
    "                 const std::string &reason);",
)

# Durable deferred-gif table, pending exclusion, and status accounting.
path = ROOT / "apps/gfingerd/src/registry.cpp"
replace_once(
    path,
    "CREATE TABLE IF NOT EXISTS adoption_runs(\n",
    "CREATE TABLE IF NOT EXISTS deferred_gifs(\n"
    "  file_id TEXT PRIMARY KEY REFERENCES object_versions(file_id),\n"
    "  local_path TEXT NOT NULL,\n"
    "  reason TEXT NOT NULL,\n"
    "  deferred_at INTEGER NOT NULL DEFAULT(unixepoch())\n"
    ");\n"
    "CREATE TABLE IF NOT EXISTS adoption_runs(\n",
)
replace_once(
    path,
    "SELECT o.key,o.current_file_id,o.size,o.sha1,o.content_type,o.extension,\n"
    "       o.upload_timestamp,COALESCE(f.state,''),COALESCE(f.retry_after,0)\n"
    "FROM objects o LEFT JOIN failures f ON f.file_id=o.current_file_id\n"
    "WHERE o.present=1 ORDER BY o.upload_timestamp,o.key",
    "SELECT o.key,o.current_file_id,o.size,o.sha1,o.content_type,o.extension,\n"
    "       o.upload_timestamp,COALESCE(f.state,''),COALESCE(f.retry_after,0),\n"
    "       CASE WHEN d.file_id IS NULL THEN 0 ELSE 1 END\n"
    "FROM objects o LEFT JOIN failures f ON f.file_id=o.current_file_id\n"
    "LEFT JOIN deferred_gifs d ON d.file_id=o.current_file_id\n"
    "WHERE o.present=1 ORDER BY o.upload_timestamp,o.key",
)
replace_once(
    path,
    "    const auto retry_after = sqlite3_column_int64(statement.get(), 8);\n"
    "    if (state == \"failed\" || state == \"unsupported\" || retry_after > now)\n"
    "      continue;",
    "    const auto retry_after = sqlite3_column_int64(statement.get(), 8);\n"
    "    const bool deferred_gif = sqlite3_column_int(statement.get(), 9) != 0;\n"
    "    if (state == \"failed\" || state == \"unsupported\" || deferred_gif ||\n"
    "        retry_after > now)\n"
    "      continue;",
)
replace_once(
    path,
    "  Statement clear(db_, \"DELETE FROM failures WHERE file_id=?\");\n"
    "  bind_text(clear.get(), 1, object.file_id);\n"
    "  clear.done();\n"
    "  transaction.commit();",
    "  Statement clear(db_, \"DELETE FROM failures WHERE file_id=?\");\n"
    "  bind_text(clear.get(), 1, object.file_id);\n"
    "  clear.done();\n"
    "  Statement clear_deferred(db_, \"DELETE FROM deferred_gifs WHERE file_id=?\");\n"
    "  bind_text(clear_deferred.get(), 1, object.file_id);\n"
    "  clear_deferred.done();\n"
    "  transaction.commit();",
)
replace_once(
    path,
    "\nvoid Registry::clear_failure(const std::string &file_id) {",
    "\nvoid Registry::defer_gif(const gdupe::RemoteObject &object,\n"
    "                         const std::filesystem::path &local_path,\n"
    "                         const std::string &reason) {\n"
    "  if (object.extension != \"gif\")\n"
    "    throw std::runtime_error(\"Only GIF objects may be deferred as malformed GIFs\");\n"
    "  if (local_path.empty())\n"
    "    throw std::runtime_error(\"Deferred GIF local path is empty\");\n"
    "  std::scoped_lock lock(mutex_);\n"
    "  Transaction transaction(db_);\n"
    "  if (!identity_matches(db_, object.key, object.file_id, object.size, object.sha1))\n"
    "    throw std::runtime_error(\"Object changed while malformed GIF was being deferred\");\n"
    "  Statement statement(db_, R\"SQL(\n"
    "INSERT INTO deferred_gifs(file_id,local_path,reason,deferred_at)\n"
    "VALUES(?,?,?,unixepoch())\n"
    "ON CONFLICT(file_id) DO UPDATE SET local_path=excluded.local_path,\n"
    "  reason=excluded.reason,deferred_at=unixepoch()\n"
    ")SQL\");\n"
    "  bind_text(statement.get(), 1, object.file_id);\n"
    "  bind_text(statement.get(), 2, local_path.string());\n"
    "  bind_text(statement.get(), 3, reason.substr(0, 4000));\n"
    "  statement.done();\n"
    "  Statement clear(db_, \"DELETE FROM failures WHERE file_id=?\");\n"
    "  bind_text(clear.get(), 1, object.file_id);\n"
    "  clear.done();\n"
    "  transaction.commit();\n"
    "}\n\n"
    "void Registry::clear_failure(const std::string &file_id) {",
)
replace_once(
    path,
    "SELECT o.key,o.current_file_id,o.extension,COALESCE(f.state,'')\n"
    "FROM objects o LEFT JOIN failures f ON f.file_id=o.current_file_id\n"
    "WHERE o.present=1",
    "SELECT o.key,o.current_file_id,o.extension,COALESCE(f.state,''),\n"
    "       CASE WHEN d.file_id IS NULL THEN 0 ELSE 1 END\n"
    "FROM objects o LEFT JOIN failures f ON f.file_id=o.current_file_id\n"
    "LEFT JOIN deferred_gifs d ON d.file_id=o.current_file_id\n"
    "WHERE o.present=1",
)
replace_once(
    path,
    "    const std::string failure = column_text(objects.get(), 3);\n"
    "    if (!supported_extension(extension) || failure == \"unsupported\") {",
    "    const std::string failure = column_text(objects.get(), 3);\n"
    "    const bool deferred_gif = sqlite3_column_int(objects.get(), 4) != 0;\n"
    "    if (!supported_extension(extension) || failure == \"unsupported\") {",
)
replace_once(
    path,
    "      ++result.unsupported;\n"
    "      continue;\n"
    "    }\n"
    "    if (failure == \"failed\")",
    "      ++result.unsupported;\n"
    "      continue;\n"
    "    }\n"
    "    if (deferred_gif) {\n"
    "      ++result.deferred_gifs;\n"
    "      continue;\n"
    "    }\n"
    "    if (failure == \"failed\")",
)

# Daemon policy: known giftest malformed geometry is preserved + deferred, not failed.
path = ROOT / "apps/gfingerd/src/main.cpp"
replace_once(
    path,
    "bool requires_nvdec(std::string_view extension) {\n"
    "  return extension == \"mp4\" || extension == \"m4v\" || extension == \"webm\";\n"
    "}\n",
    "bool requires_nvdec(std::string_view extension) {\n"
    "  return extension == \"mp4\" || extension == \"m4v\" || extension == \"webm\";\n"
    "}\n\n"
    "bool is_known_malformed_gif_geometry(std::string_view extension,\n"
    "                                     std::string_view error) {\n"
    "  return extension == \"gif\" &&\n"
    "         error == \"GIF frame rectangle is outside its logical canvas\";\n"
    "}\n\n"
    "std::filesystem::path deferred_gif_path(const fp::Config &config,\n"
    "                                        const gdupe::RemoteObject &object) {\n"
    "  return config.database_path.parent_path() / \"deferred-gifs\" /\n"
    "         (gdupe::sha256(object.file_id) + \".gif\");\n"
    "}\n\n"
    "void write_deferred_gif_note(const std::filesystem::path &gif_path,\n"
    "                             const gdupe::RemoteObject &object,\n"
    "                             std::string_view reason) {\n"
    "  const nlohmann::json note{\n"
    "      {\"state\", \"deferred-malformed-gif\"},\n"
    "      {\"key\", object.key},\n"
    "      {\"file_id\", object.file_id},\n"
    "      {\"size\", object.size},\n"
    "      {\"sha1\", object.sha1},\n"
    "      {\"extension\", object.extension},\n"
    "      {\"local_gif\", gif_path.string()},\n"
    "      {\"reason\", reason},\n"
    "      {\"todo\", \"Reprocess after malformed GIF geometry support is fixed\"}};\n"
    "  std::ofstream stream(gif_path.string() + \".json\", std::ios::trunc);\n"
    "  if (!stream)\n"
    "    throw std::runtime_error(\"Could not write deferred GIF recovery note\");\n"
    "  stream << note.dump(2) << '\\n';\n"
    "  if (!stream)\n"
    "    throw std::runtime_error(\"Could not finish deferred GIF recovery note\");\n"
    "}\n",
)
replace_once(
    path,
    "            << \"Unsupported: \" << status.unsupported << '\\n'\n"
    "            << \"Failed: \" << status.failed << '\\n'",
    "            << \"Unsupported: \" << status.unsupported << '\\n'\n"
    "            << \"Deferred malformed GIFs: \" << status.deferred_gifs << '\\n'\n"
    "            << \"Failed: \" << status.failed << '\\n'",
)
old = """    bool completed = false;
    bool failed = false;
    try {
      const auto fingerprint =
          fingerprinter.compute(path, item.remote.extension);
      const auto latest = b2.find_object(item.remote.key);
      if (!latest || !same_identity(*latest, item.remote)) {
        logger_.write("Discarded stale result for changed object " +
                      item.remote.key);
      } else {
        registry.save_fingerprint(item.remote, fingerprint);
        completed = true;
      }
    } catch (const fp::B2InfrastructureError &problem) {
      infrastructure_failed = true;
      logger_.write("B2 verification unavailable; result discarded and item "
                    "remains pending: " + item.remote.key + " -- " +
                    problem.what());
    } catch (const std::exception &problem) {
      if (!stopping()) {
        registry.record_failure(item.remote, problem.what(),
                                config_.maximum_item_attempts);
        failed = true;
        logger_.write("Item failed without stopping backlog: " +
                      item.remote.key + " -- " + problem.what());
      }
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
"""
new = """    bool completed = false;
    bool failed = false;
    bool keep_local_file = false;
    try {
      const auto fingerprint =
          fingerprinter.compute(path, item.remote.extension);
      const auto latest = b2.find_object(item.remote.key);
      if (!latest || !same_identity(*latest, item.remote)) {
        logger_.write("Discarded stale result for changed object " +
                      item.remote.key);
      } else {
        registry.save_fingerprint(item.remote, fingerprint);
        completed = true;
      }
    } catch (const fp::B2InfrastructureError &problem) {
      infrastructure_failed = true;
      logger_.write("B2 verification unavailable; result discarded and item "
                    "remains pending: " + item.remote.key + " -- " +
                    problem.what());
    } catch (const std::exception &problem) {
      if (!stopping() &&
          is_known_malformed_gif_geometry(item.remote.extension,
                                          problem.what())) {
        try {
          const auto latest = b2.find_object(item.remote.key);
          if (!latest || !same_identity(*latest, item.remote)) {
            logger_.write("Malformed GIF changed before deferral; local copy "
                          "discarded and new version remains pending: " +
                          item.remote.key);
          } else {
            const auto deferred_path = deferred_gif_path(config_, item.remote);
            std::filesystem::create_directories(deferred_path.parent_path());
            std::error_code move_error;
            std::filesystem::remove(deferred_path, move_error);
            move_error.clear();
            std::filesystem::rename(path, deferred_path, move_error);
            if (move_error) {
              std::filesystem::copy_file(
                  path, deferred_path,
                  std::filesystem::copy_options::overwrite_existing);
              std::filesystem::remove(path);
            }
            write_deferred_gif_note(deferred_path, item.remote, problem.what());
            registry.defer_gif(item.remote, deferred_path, problem.what());
            keep_local_file = true;
            logger_.write("Deferred malformed GIF without fingerprinting: " +
                          item.remote.key + " -> " + deferred_path.string());
          }
        } catch (const fp::B2InfrastructureError &verification_problem) {
          infrastructure_failed = true;
          logger_.write("B2 verification unavailable while deferring malformed "
                        "GIF; item remains pending: " + item.remote.key +
                        " -- " + verification_problem.what());
        } catch (const std::exception &defer_problem) {
          if (!stopping()) {
            registry.record_failure(item.remote, defer_problem.what(),
                                    config_.maximum_item_attempts);
            failed = true;
            logger_.write("Could not preserve malformed GIF for later; normal "
                          "failure policy applies: " + item.remote.key +
                          " -- " + defer_problem.what());
          }
        }
      } else if (!stopping()) {
        registry.record_failure(item.remote, problem.what(),
                                config_.maximum_item_attempts);
        failed = true;
        logger_.write("Item failed without stopping backlog: " +
                      item.remote.key + " -- " + problem.what());
      }
    }
    if (!keep_local_file) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
"""
replace_once(path, old, new)

# GUI persistent stat.
path = ROOT / "apps/gfingerd/src/control_window.cpp"
replace_once(
    path,
    "  text << \"Library: \" << library.fully_fingerprinted << \" complete, \"\n"
    "       << library.pending_objects << \" pending, \" << library.failed\n"
    "       << \" failed, \" << library.unsupported << \" unsupported\";",
    "  text << \"Library: \" << library.fully_fingerprinted << \" complete, \"\n"
    "       << library.pending_objects << \" pending, \" << library.deferred_gifs\n"
    "       << \" deferred GIFs, \" << library.failed << \" failed, \"\n"
    "       << library.unsupported << \" unsupported\";",
)

# Regression coverage for durable skip and replacement behavior.
path = ROOT / "apps/gfingerd/tests/test_registry.cpp"
text = path.read_text(encoding="utf-8")
marker = "\n} // namespace\n\nint main()"
if marker not in text:
    raise RuntimeError("registry test namespace marker missing")
test = r'''
void test_malformed_gif_deferral() {
  TempDirectory directory;
  gparty::fingerprints::Registry registry(directory.path() / "registry.db");
  auto gif = object("gif-bad", "gif");
  registry.reconcile({gif});
  require(registry.pending().size() == 1,
          "fresh GIF was not initially pending");
  const auto saved = directory.path() / "deferred-gifs" / "saved.gif";
  registry.defer_gif(gif, saved,
                     "GIF frame rectangle is outside its logical canvas");
  require(registry.pending().empty(),
          "deferred malformed GIF was queued for fingerprinting again");
  auto status = registry.status();
  require(status.deferred_gifs == 1 && status.pending_objects == 0 &&
              status.failed == 0,
          "deferred malformed GIF was not reported as its own status");

  registry.reconcile({gif});
  require(registry.pending().empty() && registry.status().deferred_gifs == 1,
          "unchanged deferred GIF was redownloaded after reconciliation");

  auto replacement = object("gif-fixed-version", "gif");
  registry.reconcile({replacement});
  status = registry.status();
  require(registry.pending().size() == 1 && status.deferred_gifs == 0 &&
              status.pending_objects == 1,
          "new B2 version incorrectly inherited an old GIF deferral");
}
'''
text = text.replace(marker, "\n" + test + marker, 1)
text = text.replace(
    "    test_failure_isolation();\n",
    "    test_failure_isolation();\n    test_malformed_gif_deferral();\n",
    1,
)
path.write_text(text, encoding="utf-8")

# Documentation and explicit technical-debt marker.
path = ROOT / "apps/gfingerd/README.md"
replace_once(
    path,
    "Staging lives under `%LOCALAPPDATA%\\GParty\\fingerprint-cache\\` using a digest of\n"
    "the B2 file ID.\n",
    "Staging lives under `%LOCALAPPDATA%\\GParty\\fingerprint-cache\\` using a digest of\n"
    "the B2 file ID.\n\n"
    "### Deferred malformed GIFs\n\n"
    "The known malformed-GIF geometry case discovered by the live `giftest` sweep is\n"
    "intentionally deferred rather than repeatedly retried. When WIC can open a GIF\n"
    "but its decoded frame rectangle disagrees with the declared logical canvas,\n"
    "`gfingerd` does not fingerprint that file yet. After re-verifying the exact B2\n"
    "file identity, it moves the downloaded original into\n"
    "`%LOCALAPPDATA%\\GParty\\deferred-gifs\\`, writes a JSON recovery note beside the\n"
    "GIF, and records the exact file ID, local path, and decoder reason in the\n"
    "`deferred_gifs` registry table. That exact object version is excluded from\n"
    "normal pending work and appears as its own GUI/stat count. A replacement B2\n"
    "version does not inherit the deferral.\n\n"
    "These files are deliberately recoverable. `TODO.md` keeps the proper malformed\n"
    "GIF normalization/reprocessing work as the first outstanding item; once that\n"
    "is implemented, the registry records and saved originals provide the reprocess\n"
    "queue.\n",
)

(ROOT / "apps/gfingerd/TODO.md").write_text(
    "# gfingerd TODO\n\n"
    "1. **Properly fix malformed GIF geometry handling.** Port/finish the tolerant GIF normalization work proven by the `giftest` sweep, then enumerate `deferred_gifs` in the fingerprint registry and reprocess every saved local copy. Only clear a deferral after the exact B2 object identity is verified and its fingerprint is successfully persisted.\n\n"
    "Deferred GIFs are intentionally recoverable technical debt, not discarded media. Until item 1 is implemented, `gfingerd` stores the original downloaded GIF under the local `deferred-gifs` directory, writes a JSON recovery note beside it, records the exact B2 file ID in `fingerprints.sqlite3`, excludes that exact version from normal pending work, and reports it separately in the GUI.\n",
    encoding="utf-8",
)

print("gfingerd deferred GIF patch applied")
