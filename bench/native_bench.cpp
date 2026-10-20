// Native community speed client. Protocol lineage and differences: docs/BENCHMARKING.md.
#include "native_bench.hpp"
#include <barrier>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <thread>
#include <sys/utsname.h>

#ifndef STRATA_BENCH_COMMIT
#define STRATA_BENCH_COMMIT "unknown"
#endif
namespace nb=native_bench;
using nb::json;
namespace fs=std::filesystem;

namespace {
struct Options {
    std::string url="http://127.0.0.1:8080",model="strata",key_env="STRATA_KEY",engine_commit="unrecorded";
    std::string gpu_name,notes,nonce,model_sha256;
    fs::path out,corpus,metadata,compare;
    int runs=3,clients=0,context=0,timeout=3600;
    bool long_test=false,think=false;
};
std::string utc(bool date=false) {
    std::time_t now=std::time(nullptr);std::tm t{};gmtime_r(&now,&t);char b[64];
    std::strftime(b,sizeof(b),date?"%Y-%m-%d":"%Y-%m-%dT%H:%M:%SZ",&t);return b;
}
int positive(const std::string & value,int min,int max) {
    size_t end=0;long n=std::stol(value,&end);
    if(end!=value.size() || n<min || n>max)throw std::runtime_error("numeric option is outside its supported range");
    return int(n);
}
void usage() {
    std::cout<<"unbiased-bench --url http://host:port [--model alias] [--runs 3] [--long] [--clients N]\n"
        "  --out DIR             new result directory; never overwrite a previous run\n"
        "  --api-key-env NAME    read Bearer key from environment (default STRATA_KEY)\n"
        "  --engine-commit SHA   measured engine's commit, separate from this client's revision\n"
        "  --model-sha256 SHA    optional checksum of the measured GGUF\n"
        "  --corpus-dir DIR      explicit public repository docs for the agent workload\n"
        "  --metadata FILE       optional reproducibility metadata; see docs/BENCHMARKING.md\n"
        "  --context N           fallback context when server does not advertise it\n"
        "  --gpu-name NAME       explicit card label when automatic detection is incomplete\n"
        "  --notes TEXT          background workloads, power limits and measurement caveats\n"
        "  --compare RESULTS     older results.json or its containing directory\n"
        "  --timeout SEC         request deadline, default3600 (1..7200)\n"
        "  --think               leave thinking enabled; default suite disables it\n"
        "The client only sends API requests. It never starts engines, downloads models or executes tools.\n";
}
Options parse(int argc,char ** argv) {
    Options o;
    for(int i=1;i<argc;++i) {
        std::string a=argv[i];
        if(a=="--help" || a=="-h") {usage();std::exit(0);}
        if(a=="--long") {o.long_test=true;continue;}
        if(a=="--think") {o.think=true;continue;}
        if(i+1>=argc)throw std::runtime_error("missing value for "+a);
        std::string v=argv[++i];
        if(a=="--url")o.url=v;else if(a=="--model")o.model=v;else if(a=="--out")o.out=v;
        else if(a=="--api-key-env")o.key_env=v;else if(a=="--engine-commit")o.engine_commit=v;
        else if(a=="--model-sha256")o.model_sha256=v;else if(a=="--corpus-dir")o.corpus=v;
        else if(a=="--metadata")o.metadata=v;else if(a=="--compare")o.compare=v;
        else if(a=="--gpu-name")o.gpu_name=v;else if(a=="--notes")o.notes=v;
        else if(a=="--runs")o.runs=positive(v,1,100);else if(a=="--clients")o.clients=positive(v,0,64);
        else if(a=="--context")o.context=positive(v,1,10485760);else if(a=="--timeout")o.timeout=positive(v,1,7200);
        else throw std::runtime_error("unknown option: "+a);
    }
    if(o.engine_commit!="unrecorded" && !std::regex_match(o.engine_commit,std::regex("[0-9a-fA-F]{7,64}")))
        throw std::runtime_error("--engine-commit must be a Git commit hash");
    if(!o.model_sha256.empty() && !std::regex_match(o.model_sha256,std::regex("[0-9a-fA-F]{64}")))
        throw std::runtime_error("--model-sha256 must contain64 hexadecimal characters");
    o.nonce=std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if(o.out.empty())o.out=fs::path("bench/results")/("native-"+o.nonce);
    return o;
}
json public_metadata(const fs::path & path) {
    if(path.empty())return json::object();
    auto raw=json::parse(nb::read_file(path));
    if(!raw.is_object())throw std::runtime_error("metadata must be a JSON object");
    json out=json::object();
    // Deliberately do not copy arbitrary config/env objects or authentication fields.
    for(const auto & key:{"engine_version","backend","gpu","gpu_vram_mib","cpu","installed_ram_gib",
        "engine_threads","cpu_quota","memory_limit_gib","kv","vram_reserve_mib","draft","tail_skip",
        "model_file","model_sha256","corpus_commit","model_settings","notes"}) {
        if(raw.contains(key)) {
            if(!raw[key].is_primitive())throw std::runtime_error("metadata values must be scalar: "+std::string(key));
            out[key]=raw[key];
        }
    }
    return out;
}
json hardware() {
    json out={{"gpu","not detected"},{"gpu_vram_mib",nullptr},{"cpu","not read"},
        {"visible_cpu_threads",std::thread::hardware_concurrency()},{"visible_ram_gib",nullptr},
        {"hardware_scope","Benchmark-client observations; remote inference hardware requires explicit supplied metadata."}};
    auto cpus=nb::read_file("/proc/cpuinfo");std::istringstream cpu(cpus);std::string line;
    while(std::getline(cpu,line))if(line.rfind("model name",0)==0){out["cpu"]=nb::trim(line.substr(line.find(':')+1));break;}
    std::istringstream memory(nb::read_file("/proc/meminfo"));
    while(std::getline(memory,line))if(line.rfind("MemTotal:",0)==0){std::istringstream n(line.substr(9));double kib;if(n>>kib)out["visible_ram_gib"]=kib/1048576.;break;}
    out["cpu_quota_raw"]=nb::trim(nb::read_file("/sys/fs/cgroup/cpu.max"));
    out["memory_limit_raw"]=nb::trim(nb::read_file("/sys/fs/cgroup/memory.max"));
    utsname info{};if(uname(&info)==0)out["os"]=std::string(info.sysname)+" "+info.release+" "+info.machine;
    std::error_code ec;
    if(fs::is_directory("/sys/class/drm",ec))for(const auto & entry:fs::directory_iterator("/sys/class/drm",ec)) {
        auto name=entry.path().filename().string();if(!std::regex_match(name,std::regex("card[0-9]+")))continue;
        auto device=entry.path()/"device";
        auto vendor=nb::trim(nb::read_file(device/"vendor"));
        if(vendor!="0x1002" && vendor!="0x10de" && vendor!="0x8086")continue;
        auto label=nb::trim(nb::read_file(device/"product_name"));
        if(label.empty())label=(vendor=="0x1002"?"AMD Radeon":vendor=="0x10de"?"NVIDIA":"Intel")+
            std::string(" (PCI ")+vendor+":"+nb::trim(nb::read_file(device/"device"))+")";
        auto vram=nb::trim(nb::read_file(device/"mem_info_vram_total"));
        out["gpu"]=label;if(!vram.empty())out["gpu_vram_mib"]=std::stod(vram)/1048576.;
        out["gpu_detection_note"]="First visible DRM card; use --gpu-name/metadata to identify the served card on multi-GPU hosts.";
        break;
    }
    return out;
}
std::string fingerprint(const std::string & bytes) {
    uint64_t hash=14695981039346656037ULL;for(unsigned char c:bytes){hash^=c;hash*=1099511628211ULL;}
    std::ostringstream out;out<<std::hex<<std::setw(16)<<std::setfill('0')<<hash;return out.str();
}
struct CorpusFile {std::string name,body;size_t words;};
std::vector<CorpusFile> corpus(const fs::path & dir) {
    std::vector<CorpusFile> files;
    if(!dir.empty()) {
        if(!fs::is_directory(dir))throw std::runtime_error("--corpus-dir must name a directory");
        for(const auto & name:{"README.md","docs/HOW_IT_WORKS.md","docs/DETAILS.md","docs/INSTALL.md","docs/MODELS.md","docs/COMMUNITY_BENCHMARKS.md"}) {
            std::istringstream input(nb::read_file(dir/name));std::string word,body;size_t count=0;
            while(count<1500 && input>>word){if(count)body+=' ';body+=word;++count;}
            if(count>=750)files.push_back({name,body,count});
        }
    }
    for(int i=0;files.size()<5;++i)files.push_back({"generated/part"+std::to_string(i)+".txt",nb::long_prompt(1500),1500});
    files.resize(5);return files;
}

class Suite {
    const Options & o;const nb::Client & client;const nb::Redactor & redact;std::ofstream journal;
    std::vector<CorpusFile> files;double tokens_per_word=1.1,overhead=40;size_t sequence=0;
public:
    int context;json result={{"chat",json::object()},{"prompts",json::object()},{"agent",json::object()},
        {"clients",json::object()},{"skipped",json::object()},{"failures",json::array()}};
    json corpus_info=json::array();
    Suite(const Options & o,const nb::Client & client,const nb::Redactor & redactor,int context):
        o(o),client(client),redact(redactor),journal(o.out/"requests.jsonl"),files(corpus(o.corpus)),context(context) {
        if(!journal)throw std::runtime_error("Cannot create request journal");
        for(const auto & f:files)corpus_info.push_back({{"name",f.name},{"words",f.words},{"fingerprint_fnv1a64",fingerprint(f.body)}});
    }
    std::string tag(const std::string & prefix) const {return prefix+"-"+o.nonce;}
    nb::Stream request(const std::string & stage,const json & messages,int cap,int run=-1,int turn=-1,const json & tools=json::array()) {
        auto response=client.chat(messages,cap,o.think,tools);auto row=response.digest();
        row["stage"]=stage;row["run"]=run;row["turn"]=turn;row["request_index"]=sequence++;
        journal<<redact.object(row).dump()<<'\n';journal.flush();
        if(!row["ok"].get<bool>())result["failures"].push_back({{"where",stage},{"run",run},{"turn",turn},
            {"error",row["err"].is_null()?json("No non-empty content/reasoning event"):row["err"]}});
        return response;
    }
    json prompt(int target,int run) const {
        auto words=std::max(50,int((target-overhead)/tokens_per_word));
        return json::array({{{"role","user"},{"content","[bench "+tag(std::to_string(run)+"-"+std::to_string(target)+"-p")+"]\n"+nb::long_prompt(words)}}});
    }
    void warmup() {
        std::cout<<"Warm-up and calibration (excluded from measured summaries) ...\n"<<std::flush;
        auto warm=request("warmup_chat",json::array({{{"role","user"},{"content",nb::prompts[0]}}}),24);
        if(!warm.digest()["ok"].get<bool>())throw std::runtime_error("Warm-up request failed");
        std::vector<double> counts;
        for(int words:{200,800}) {
            auto r=request("calibration",json::array({{{"role","user"},{"content","["+tag("cal-"+std::to_string(words))+"]\n"+nb::long_prompt(words)}}}),1);
            auto d=r.digest();if(!d["ok"].get<bool>() || !d["prompt_tokens"].is_number())
                throw std::runtime_error("Calibration requires successful responses with prompt token counts");
            counts.push_back(nb::numeric(d["prompt_tokens"]));
        }
        if(counts[1]<=counts[0])throw std::runtime_error("Invalid token-count calibration");
        tokens_per_word=(counts[1]-counts[0])/600.;overhead=std::max(0.,counts[0]-200*tokens_per_word);
        auto r=request("warmup_prompt",prompt(1024,-1),8);
        if(!r.digest()["ok"].get<bool>())throw std::runtime_error("Prompt warm-up failed");
    }
    void chat() {
        json rows=json::array();
        for(int run=0;run<o.runs;++run)for(size_t i=0;i<nb::prompts.size();++i) {
            auto r=request("chat",json::array({{{"role","user"},{"content","["+tag(std::to_string(run)+"-"+std::to_string(i)+"-c")+"] "+nb::prompts[i]}}}),100,run);
            auto d=r.digest();d["run"]=run;d["prompt"]=i;rows.push_back(d);
            std::cout<<"chat "<<run+1<<"/"<<i+1<<": "<<(d["ok"].get<bool>()?"ok":"FAILED")<<'\n'<<std::flush;
        }
        result["chat"]={{"requests",rows},{"decode_tps",nb::column_stat(rows,"decode_tps")},
            {"ttft_s",nb::column_stat(rows,"ttft_s")},{"prompt_tokens",nb::column_stat(rows,"prompt_tokens")},
            {"generated_tokens",nb::column_stat(rows,"generated_tokens")}};
    }
    void long_prompt(int target,const std::string & label) {
        if(context && target+24+256>context){result["skipped"][label]="Context is "+std::to_string(context)+" tokens";return;}
        json rows=json::array();for(int run=0;run<o.runs;++run) {
            auto r=request(label,prompt(target,run),24,run);auto d=r.digest();d["run"]=run;rows.push_back(d);
            std::cout<<label<<" "<<run+1<<": "<<(d["ok"].get<bool>()?"ok":"FAILED")<<'\n'<<std::flush;
        }
        result["prompts"][label]={{"target_tokens",target},{"requests",rows},
            {"prompt_tps",nb::column_stat(rows,"prompt_tps")},{"ttft_s",nb::column_stat(rows,"ttft_s")},
            {"prompt_tokens",nb::column_stat(rows,"prompt_tokens")},{"reused_tokens",nb::column_stat(rows,"reused_tokens")}};
    }
    void agent() {
        if(context && context<10240){result["skipped"]["agent"]="Context is below10240 tokens";return;}
        json sessions=json::array(),follow=json::array(),last=json::array(),first=json::array();
        std::vector<double> hits,rereads,fresh;
        for(int run=0;run<o.runs;++run) {
            json messages=json::array({{{"role","system"},{"content",nb::agent_system+"\n(session "+tag(std::to_string(run)+"-a")+")"}},
                {{"role","user"},{"content","I will show you files from a project one at a time. After each file, answer in one or two sentences what it is for. Do not call tools."}}});
            json turns=json::array();
            for(int turn=0;turn<6;++turn) {
                if(turn)messages.push_back({{"role","user"},{"content","File "+files[turn-1].name+":\n"+files[turn-1].body+"\n\nWhat is this file for? Two sentences."}});
                auto r=request("agent",messages,64,run,turn,nb::tools());auto d=r.digest();d["run"]=run;d["turn"]=turn;turns.push_back(d);
                if(!d["ok"].get<bool>())break;
                messages.push_back({{"role","assistant"},{"content",r.text.empty()?"Understood.":r.text}});
                if(turn)follow.push_back(d);else first.push_back(d);
            }
            sessions.push_back(turns);double reused=0,prompt_tokens=0,reread=0,read=0;bool counts=true;
            for(size_t i=1;i<turns.size();++i) {
                const auto & d=turns[i];const auto & previous=turns[i-1];
                if(!d["ok"].get<bool>() || !previous["ok"].get<bool>()) {counts=false;break;}
                for(const auto & key:{"reused_tokens","prompt_tokens","read_tokens"})if(!d[key].is_number())counts=false;
                if(!previous["prompt_tokens"].is_number() || !previous["generated_tokens"].is_number())counts=false;
                reused+=nb::numeric(d["reused_tokens"]);prompt_tokens+=nb::numeric(d["prompt_tokens"]);
                reread+=std::max(0.,nb::numeric(previous["prompt_tokens"])+nb::numeric(previous["generated_tokens"])-nb::numeric(d["reused_tokens"]));
                read+=nb::numeric(d["read_tokens"]);
            }
            if(turns.size()==6 && turns.back()["ok"].get<bool>())last.push_back(turns.back());
            if(counts && turns.size()==6) {hits.push_back(100*reused/std::max(1.,prompt_tokens));rereads.push_back(reread);fresh.push_back(read-reread);}
            std::cout<<"agent session "<<run+1<<": "<<turns.size()<<" attempted turns\n"<<std::flush;
        }
        result["agent"]={{"sessions",sessions},{"hit_pct",nb::stat(hits)},{"reread_tokens",nb::stat(rereads)},
            {"new_tokens",nb::stat(fresh)},{"turn_ttft_s",nb::column_stat(follow,"ttft_s")},
            {"turn_decode_tps",nb::column_stat(follow,"decode_tps")},{"last_turn_prompt_tokens",nb::column_stat(last,"prompt_tokens")},
            {"first_turn_ttft_s",nb::column_stat(first,"ttft_s")}};
    }
    void concurrency() {
        if(!o.clients)return;
        const std::array<std::string,8> topics={"the history of lighthouses and how their lenses worked","how a mechanical watch keeps time",
            "why bridges are built as arches and as suspension spans","the water cycle and what a drought does to a river valley",
            "how bread rises, from yeast to crust","the design of a medieval castle and why walls were stepped",
            "how a transistor switches, explained for a curious teenager","the life of a honey bee colony through one year"};
        json rounds=json::array();std::vector<double> rates,firsts,lats;int errors=0;
        for(int round=0;round<o.runs;++round) {
            std::vector<json> results(o.clients);std::vector<std::thread> workers;std::barrier start(o.clients+1);
            for(int i=0;i<o.clients;++i)workers.emplace_back([&,i] {
                json messages=json::array({{{"role","user"},{"content","[request "+std::to_string(i)+" "+tag("b-"+std::to_string(o.clients)+"-"+std::to_string(round))+"] Write a clear, well organised essay of about 400 words on "+topics[i%8]+". Use plain prose, no lists."}}});
                start.arrive_and_wait();try {auto r=client.chat(messages,128,o.think);results[i]=r.digest();}
                catch(const std::exception & e){results[i]={{"ok",false},{"err",e.what()},{"total_s",nullptr},{"generated_tokens",nullptr}};}
            });
            auto began=nb::Clock::now();start.arrive_and_wait();for(auto & thread:workers)thread.join();double wall=nb::seconds(began),tokens=0;
            bool known=true;int round_errors=0;json rows=json::array();
            for(auto row:results) {
                row["stage"]="clients";row["run"]=round;row["request_index"]=sequence++;
                journal<<redact.object(row).dump()<<'\n';journal.flush();rows.push_back(row);
                if(!row.value("ok",false))++round_errors;
                if(!row.contains("generated_tokens") || !row["generated_tokens"].is_number())known=false;else tokens+=nb::numeric(row["generated_tokens"]);
                if(row.value("ok",false)) {
                    if(row.contains("ttft_s") && row["ttft_s"].is_number())firsts.push_back(nb::numeric(row["ttft_s"]));
                    if(row["total_s"].is_number())lats.push_back(nb::numeric(row["total_s"]));
                }
            }
            errors+=round_errors;if(known && !round_errors)rates.push_back(tokens/wall);
            rounds.push_back({{"round",round},{"wall_s",wall},{"tokens",known?json(tokens):json(nullptr)},
                {"total_tps",known && !round_errors?json(tokens/wall):json(nullptr)},{"error_count",round_errors},{"requests",rows}});
        }
        if(errors)result["failures"].push_back({{"where","clients"},{"error",std::to_string(errors)+" failed concurrency requests"}});
        result["clients"]={{"clients",o.clients},{"rounds",rounds},{"total_tps",nb::stat(rates)},{"errors",errors},
            {"ttft_p50_s",firsts.empty()?json(nullptr):json(nb::percentile(firsts,.5))},
            {"ttft_p95_s",firsts.empty()?json(nullptr):json(nb::percentile(firsts,.95))},
            {"latency_p50_s",lats.empty()?json(nullptr):json(nb::percentile(lats,.5))},
            {"latency_p95_s",lats.empty()?json(nullptr):json(nb::percentile(lats,.95))}};
    }
    void run() {warmup();chat();long_prompt(4096,"4K");long_prompt(16384,"16K");agent();concurrency();if(o.long_test)long_prompt(32768,"32K");}
    json calibration() const {return {{"tokens_per_word",tokens_per_word},{"overhead_tokens",overhead}};}
};

std::string format(const json & stat) {
    if(!stat.is_object() || !stat.contains("median"))return "not measured";
    std::ostringstream out;out<<std::fixed<<std::setprecision(2)<<stat["median"].get<double>()<<" ("<<stat["min"].get<double>()<<"–"<<stat["max"].get<double>()<<"; n="<<stat["n"]<<")";return out.str();
}
json metrics(const json & results) {
    json out=json::object();auto chat=results.value("chat",json::object());
    out["chat decode tok/s"]=chat.value("decode_tps",json(nullptr));out["chat TTFT s"]=chat.value("ttft_s",json(nullptr));
    for(auto it=results["prompts"].begin();it!=results["prompts"].end();++it)out[it.key()+" prompt tok/s"]=it.value().value("prompt_tps",json(nullptr));
    out["agent follow-up TTFT s"]=results["agent"].value("turn_ttft_s",json(nullptr));
    out["agent decode tok/s"]=results["agent"].value("turn_decode_tps",json(nullptr));
    if(!results["clients"].empty())out["concurrency total tok/s"]=results["clients"].value("total_tps",json(nullptr));
    return out;
}
void write_report(const Options & o,const nb::Redactor & redact,const json & data) {
    std::ofstream structured(o.out/"results.json");structured<<redact.object(data).dump(2)<<'\n';
    if(!structured)throw std::runtime_error("Cannot write results.json");
    const auto & machine=data["machine"];const auto & result=data["results"];
    std::ostringstream out;out<<"# UnbiasedStrata community speed benchmark\n\n"
        <<"Native C++ port of the PolyStrata speed protocol; this is **not output from the original Python tool**. "
        <<"Speed only: no model-quality score or real tool execution.\n\n"
        <<"- Measured: "<<data["date"].get<std::string>()<<"\n- Run complete: "<<(data["complete"].get<bool>()?"yes":"NO")
        <<"; failed requests/stages: "<<result["failures"].size()<<"\n- Client revision: `"<<STRATA_BENCH_COMMIT<<"`\n"
        <<"- Measured engine commit (supplied): `"<<o.engine_commit<<"`\n- Model alias: `"<<o.model<<"`\n"
        <<"- Context: "<<data["context"]<<" tokens; thinking "<<(o.think?"enabled":"disabled")<<"; temperature0\n"
        <<"- GPU: "<<nb::text(machine,"gpu")<<"; VRAM MiB: "<<machine["gpu_vram_mib"]<<"\n"
        <<"- CPU: "<<nb::text(machine,"cpu")<<"; visible RAM GiB: "<<machine["visible_ram_gib"]<<"\n"
        <<"- Background workloads / power / caveats: "<<(o.notes.empty()?"not recorded":o.notes)<<"\n\n"
        <<"## Measurements\n\n| Metric | Median (min–max; observations) |\n| --- | ---: |\n";
    auto flat=metrics(result);for(auto it=flat.begin();it!=flat.end();++it)out<<"| "<<it.key()<<" | "<<format(it.value())<<" |\n";
    out<<"\n## Method and limitations\n\nThree fixed short questions, calibrated fresh 4K/16K prompts, "
        <<(o.long_test?"optional 32K prompts, ":"")<<"and a six-turn agent-style session; "<<o.runs<<" repeats. "
        <<"The warm-up chat, two calibration requests and 1K warm-up prompt are excluded. "
        <<"No tool is executed. Loading is excluded. Engine timings and client TTFT are separate. "
        <<"Missing token counters remain unknown; SSE chunks are never counted as tokens. "
        <<"Failed/incomplete streams are retained and excluded from valid summaries.\n\n"
        <<"Agent corpus mode: "<<(o.corpus.empty()?"deterministic generated fallback":"explicit repository documents with generated fallback")
        <<". File names, word counts and non-cryptographic content fingerprints are in results.json. "
        <<"Different corpora, engine settings, contexts or quantizations are not controlled A/B comparisons. "
        <<"Concurrent clients may queue on this single-sequence engine; throughput is over burst wall time.\n\n"
        <<"Skipped stages: `"<<result["skipped"].dump()<<"`\n\n"
        <<"Failure details: `"<<result["failures"].dump()<<"`\n\n"
        <<"See results.json and requests.jsonl for every attempted request, timings, counts and supplied metadata. "
        <<"Review all files for private information before submitting. Reproduce with the documented options in "
        <<"[docs/BENCHMARKING.md](../../../docs/BENCHMARKING.md); exact run options are in results.json.\n";
    std::ofstream markdown(o.out/"README.md");markdown<<redact(out.str());if(!markdown)throw std::runtime_error("Cannot write README.md");
}
} // namespace

int main(int argc,char ** argv) {
    try {
        Options o=parse(argc,argv);std::string key;
        if(const char * env=std::getenv(o.key_env.c_str()))key=env;
        nb::Redactor redact(key);redact.add(o.url,"<server>");
        nb::Client client(o.url,key,o.model,o.timeout);
        auto health=client.get("/health"),props=client.get("/props"),models=client.get("/v1/models");
        if(health.value("status",std::string())!="ok" || models.empty())throw std::runtime_error("Server health/models unavailable; check the URL and API key environment variable");
        int context=health.value("max_context",0);
        if(!context && props.contains("default_generation_settings"))context=props["default_generation_settings"].value("n_ctx",0);
        if(context && o.context && context!=o.context)throw std::runtime_error("--context disagrees with the server; do not mislabel the run");
        if(!context)context=o.context;
        if(!context)throw std::runtime_error("Server does not advertise context; provide --context");
        json metadata=public_metadata(o.metadata);auto machine=hardware();
        if(!o.gpu_name.empty())machine["gpu"]=o.gpu_name;
        if(metadata.contains("gpu"))machine["gpu"]=metadata["gpu"];
        if(metadata.contains("gpu_vram_mib"))machine["gpu_vram_mib"]=metadata["gpu_vram_mib"];
        if(metadata.contains("cpu"))machine["cpu"]=metadata["cpu"];
        if(fs::exists(o.out))throw std::runtime_error("Output directory already exists; choose a new --out");
        fs::create_directories(o.out);
        Suite suite(o,client,redact,context);auto started=nb::Clock::now();bool complete=false;
        try {suite.run();complete=true;}
        catch(const std::exception & e){suite.result["failures"].push_back({{"where","suite"},{"error",e.what()}});}
        json data={{"schema","unbiased-native-speed-1"},{"date",utc()},{"tool","unbiased-bench"},
            {"client_commit",STRATA_BENCH_COMMIT},{"upstream_protocol_commit","26e4cb5df7996edfb5ff549a63b82d8581e1e658"},
            {"engine_commit",o.engine_commit},{"complete",complete},{"elapsed_s",nb::seconds(started)},{"context",context},
            {"params",{{"runs",o.runs},{"long",o.long_test},{"clients",o.clients},{"think",o.think},{"temperature",0},
                {"chat_max_tokens",100},{"prompt_max_tokens",24},{"agent_max_tokens",64},{"agent_turns",6},
                {"agent_words",1500},{"burst_max_tokens",128},{"timeout_s",o.timeout},{"model",o.model}}},
            {"machine",machine},{"metadata",metadata},{"model_sha256",o.model_sha256.empty()?json(nullptr):json(o.model_sha256)},
            {"calibration",suite.calibration()},{"agent_corpus",suite.corpus_info},{"results",suite.result}};
        write_report(o,redact,data);
        if(!o.compare.empty()) {
            auto path=fs::is_directory(o.compare)?o.compare/"results.json":o.compare;
            auto previous=json::parse(nb::read_file(path));
            if(!previous.contains("schema") || !previous["schema"].is_string() || previous["schema"]!="unbiased-native-speed-1")throw std::runtime_error("--compare requires native protocol1 results; cross-protocol comparison must be reviewed separately");
            std::cout<<"Comparison (check model/settings/corpus before interpreting):\n";
            auto old=metrics(previous["results"]),now=metrics(suite.result);
            for(auto it=now.begin();it!=now.end();++it)std::cout<<it.key()<<": "<<format(old.value(it.key(),json(nullptr)))<<" -> "<<format(it.value())<<'\n';
        }
        std::cout<<"Report: "<<redact((o.out/"README.md").string())<<'\n';
        return complete && suite.result["failures"].empty()?0:1;
    } catch(const std::exception & e) {nb::Redactor redact;std::cerr<<redact(e.what())<<'\n';return 2;}
}
