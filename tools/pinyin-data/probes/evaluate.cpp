#include <rime_api.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

std::string quote(const std::string& text) {
  std::string result = "\"";
  for (char c : text) {
    if (c == '\\' || c == '"') result += '\\';
    if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else result += c;
  }
  return result + "\"";
}

std::vector<std::string> split(const std::string& value, char delimiter) {
  std::vector<std::string> fields;
  std::istringstream stream(value);
  std::string field;
  while (std::getline(stream, field, delimiter)) fields.push_back(field);
  return fields;
}

int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "evaluate.exe SHARED_DIR USER_DIR SCHEMA_ID CORPUS.tsv\n";
    return 2;
  }
  std::ifstream corpus(argv[4]);
  if (!corpus) {
    std::cerr << "Cannot open corpus\n";
    return 2;
  }
  if (std::filesystem::exists(argv[2])) {
    std::cerr << "Refusing to reuse an existing user-data directory\n";
    return 2;
  }
  std::filesystem::create_directories(argv[2]);
  RimeApi* api = rime_get_api();
  std::cerr << "rime_version=" << api->get_version() << '\n';
  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = argv[1];
  traits.user_data_dir = argv[2];
  traits.distribution_name = "Simple Voice isolated dictionary evaluation";
  traits.distribution_code_name = "voiceime-study";
  traits.distribution_version = "2026-11-15";
  traits.app_name = "rime.voiceime-study";
  traits.min_log_level = 2;
  auto start = std::chrono::steady_clock::now();
  api->setup(&traits);
  api->initialize(&traits);
  api->deployer_initialize(&traits);
  if (api->start_maintenance(true)) api->join_maintenance_thread();
  auto session = api->create_session();
  if (!session || !api->select_schema(session, argv[3])) return 3;
  auto deployed = std::chrono::steady_clock::now();
  std::cerr << "deployment_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(deployed-start).count() << '\n';
  std::string line;
  std::getline(corpus, line);
  std::cout << "[\n";
  bool first = true;
  while (std::getline(corpus, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto fields = split(line, '\t');
    if (fields.size() != 3 || fields[0].empty()) continue;
    api->clear_composition(session);
    RIME_STRUCT(RimeCommit, stale);
    if (api->get_commit(session, &stale)) api->free_commit(&stale);
    auto begin = std::chrono::steady_clock::now();
    for (unsigned char c : fields[1]) {
      api->process_key(session, c, c >= 'A' && c <= 'Z' ? 1 : 0);
    }
    auto end = std::chrono::steady_clock::now();
    if (!first) std::cout << ",\n";
    first = false;
    std::cout << "{\"category\":" << quote(fields[0]) << ",\"input\":" << quote(fields[1])
              << ",\"expected\":" << quote(fields[2]) << ",\"typing_us\":"
              << std::chrono::duration_cast<std::chrono::microseconds>(end-begin).count();
    RIME_STRUCT(RimeContext, context);
    std::vector<std::string> candidates;
    if (api->get_context(session, &context)) {
      std::cout << ",\"preedit\":" << quote(context.composition.preedit ? context.composition.preedit : "");
      for (int i = 0; i < context.menu.num_candidates && i < 5; ++i)
        candidates.emplace_back(context.menu.candidates[i].text ? context.menu.candidates[i].text : "");
      api->free_context(&context);
    }
    std::cout << ",\"candidates\":[";
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (i) std::cout << ',';
      std::cout << quote(candidates[i]);
    }
    std::cout << "]}";
  }
  std::cout << "\n]\n";
  api->clear_composition(session);
  api->destroy_session(session);
  api->finalize();
}
