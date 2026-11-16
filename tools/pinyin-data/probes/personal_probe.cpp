#include <rime_api.h>
#include <rime_levers_api.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

RimeApi* api;
RimeTraits traits;
std::filesystem::path user;
void start(bool deploy=false) {
  api->initialize(&traits);
  if (deploy) {
    api->deployer_initialize(&traits);
    if(api->start_maintenance(true)) api->join_maintenance_thread();
  }
}
RimeSessionId session(const char* schema="voiceime") {
  auto id=api->create_session();
  if (!id || !api->select_schema(id,schema)) {std::cerr<<"schema fail\n"; std::exit(3);}
  return id;
}
RimeSessionId generated_session(const char* schema,const char* dictionary) {
  RimeConfig config={nullptr};
  if(!api->schema_open(schema,&config)) std::exit(4);
  if(!api->config_set_string(&config,"personal/user_dict",dictionary)) std::exit(5);
  auto id=session(schema);
  api->config_close(&config);
  return id;
}
void write(const std::string& value, const std::string& name="voiceime_personal") {
  std::ofstream f(user/(name+".txt"),std::ios::binary);
  f<<value<<"\tqysyx\t100\n";
}
std::string query(RimeSessionId id,const std::string& input="qysyx",bool commit=false) {
  api->clear_composition(id);
  for (unsigned char c:input) api->process_key(id,c,0);
  RIME_STRUCT(RimeContext,c);
  std::string result;
  if(api->get_context(id,&c)) {
    if(c.menu.num_candidates) result=c.menu.candidates[0].text;
    api->free_context(&c);
  }
  if(commit) {
    api->process_key(id,0x20,0);
    RIME_STRUCT(RimeCommit,committed);
    if(api->get_commit(id,&committed)) api->free_commit(&committed);
  }
  api->clear_composition(id);
  return result;
}
void record(const char* name,const std::string& value) {std::cout<<name<<'\t'<<value<<'\n';}
int main(int argc,char**argv) {
  if(argc!=4) return 2;
  std::string mode=argv[3];
  if(mode!="generation" && mode!="refresh" && mode!="private" && mode!="normal") {
    std::cerr<<"Unknown probe mode\n";
    return 2;
  }
  user=argv[2];
  if(std::filesystem::exists(user)) {
    std::cerr<<"Refusing to reuse an existing user-data directory\n";
    return 2;
  }
  std::filesystem::create_directories(user);
  api=rime_get_api();
  std::cerr<<"rime_version="<<api->get_version()<<'\n';
  RIME_STRUCT_INIT(RimeTraits,traits);
  traits.shared_data_dir=argv[1];traits.user_data_dir=argv[2];
  traits.distribution_name="Simple Voice isolated personal probe";
  traits.distribution_code_name="voiceime-study";
  traits.distribution_version="2026-11-15";
  traits.app_name="rime.voiceime-study";traits.min_log_level=2;
  api->setup(&traits);
  write("轻音术语甲");
  if(mode=="generation") {
    start(true);
    auto a=session();
    record("generation_old_before",query(a));
    write("轻音术语新代","voiceime_personal_g2");
    auto b=generated_session("voiceime","voiceime_personal_g2");
    record("generation_new_while_old_alive",query(b));
    auto c=generated_session("voiceime_private","voiceime_personal_g2");
    record("generation_private_while_old_alive",query(c));
    api->destroy_session(c);
    record("generation_old_still_alive",query(a));
    std::filesystem::remove(user/"voiceime_personal.txt");
    record("generation_old_after_file_deleted",query(a));
    api->destroy_session(a);a=session();
    record("generation_recreated_old",query(a));
    api->destroy_session(a);api->destroy_session(b);
    write("轻音术语甲");
    a=session();
    record("generation_after_all_sessions_released",query(a));
    api->destroy_session(a);
    a=generated_session("voiceime_private","voiceime_personal_g2");
    record("generation_private_reapplied_after_release",query(a));
    api->destroy_session(a);api->finalize();
    start();a=session();
    record("generation_after_runtime_restart",query(a));
    api->destroy_session(a);api->finalize();
  } else if(mode=="refresh") {
    start(true);
    auto a=session();
    record("initial",query(a));
    write("轻音术语乙");
    record("existing_session",query(a));
    api->destroy_session(a);a=session();
    record("recreate_only_session",query(a));
    auto b=session();
    write("轻音术语丙");
    api->destroy_session(a);a=session();
    record("recreate_with_other_session_alive",query(a));
    api->destroy_session(a);api->destroy_session(b);a=session();
    record("recreate_after_all_sessions_destroyed",query(a));
    write("轻音术语丁");
    api->destroy_session(a);api->finalize();start();a=session();
    record("full_runtime_reinitialize",query(a));
    api->destroy_session(a);
    auto module=api->find_module("levers");
    auto levers=module?reinterpret_cast<RimeLeversApi*>(module->get_api()):nullptr;
    record("export_count",levers?std::to_string(levers->export_user_dict("voiceime",(user/"learned-export.tsv").string().c_str())):"no levers");
    api->finalize();
  } else if(mode=="private" || mode=="normal") {
    start(true);
    auto a=session(mode=="private"?"voiceime_private":"voiceime");
    record("personal_lookup",query(a));
    record("personal_prefix",query(a,"qys"));
    record("commit_personal",query(a,"qysyx",true));
    record("commit_common_word",query(a,"nihao",true));
    record("commit_sentence",query(a,"jintiantianqibucuo",true));
    api->destroy_session(a);
    auto module=api->find_module("levers");
    auto levers=module?reinterpret_cast<RimeLeversApi*>(module->get_api()):nullptr;
    record("export_count",levers?std::to_string(levers->export_user_dict("voiceime",(user/"learned-export.tsv").string().c_str())):"no levers");
    api->finalize();
    record("automatic_db_exists",std::filesystem::exists(user/"voiceime.userdb")?"true":"false");
    std::ifstream f(user/"voiceime_personal.txt",std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(f)),{});
    record("personal_unchanged",contents=="轻音术语甲\tqysyx\t100\n"?"true":"false");
  }
}
