// Native tests: frozen workload, stream framing/counts/errors, privacy and full CLI against a native HTTP mock.
#include "native_bench.hpp"
#include <iostream>
#include <thread>
#include <sys/wait.h>

namespace nb=native_bench;
using nb::json;
namespace fs=std::filesystem;
namespace {
void check(bool value,const std::string & message) {if(!value)throw std::runtime_error(message);}
int command(const std::vector<std::string> & args) {
    auto pid=fork();if(pid<0)throw std::runtime_error("fork failed");
    if(!pid) {
        std::vector<char *> argv;for(const auto & arg:args)argv.push_back(const_cast<char *>(arg.c_str()));argv.push_back(nullptr);
        execv(argv[0],argv.data());_exit(127);
    }
    int status;while(waitpid(pid,&status,0)<0){};
    return WIFEXITED(status)?WEXITSTATUS(status):128;
}
std::string event(const json & value) {return "data: "+value.dump()+"\n\n";}
std::string response(int prompt=100,int generated=42,int cached=60) {
    return ": keep-alive\n\n"+event({{"choices",json::array({{{"delta",{{"content","Hello"}}}}})}})+
        event({{"choices",json::array({{{"delta",{{"content"," world"}}}}})}})+
        event({{"choices",json::array()}, {"usage",{{"prompt_tokens",prompt},{"completion_tokens",generated},
            {"prompt_tokens_details",{{"cached_tokens",cached}}}}},
            {"timings",{{"prompt_n",prompt-cached},{"predicted_n",generated},{"predicted_per_second",84.0},{"prompt_per_second",500.0}}}})+
        "data: [DONE]\n\n";
}
struct Mock {
    httplib::Server server;std::thread thread;int port;
    Mock() {
        server.Get("/health",[](auto &,auto & res){res.set_content(R"({"status":"ok","loaded":true,"service":"strata","max_context":65536})","application/json");});
        server.Get("/props",[](auto &,auto & res){res.set_content("{}","application/json");});
        server.Get("/v1/models",[](auto &,auto & res){res.set_content(R"({"data":[{"id":"test"}]})","application/json");});
        server.Post("/v1/chat/completions",[](const auto & req,auto & res) {
            auto body=json::parse(req.body);auto model=body.value("model",std::string());
            if(model=="slow")std::this_thread::sleep_for(std::chrono::seconds(2));
            if(model=="http-error") {res.status=503;res.set_content("backend unavailable at192.168.1.3 with secret-canary","text/plain");return;}
            if(model=="bad-stream") {res.set_content(event({{"choices",json::array({{{"delta",{{"content","partial"}}}}})}}),"text/event-stream");return;}
            if(model=="sse-error") {res.set_content(event({{"error",{{"message","failed"}}}})+"data: [DONE]\n\n","text/event-stream");return;}
            if(model=="missing-counts") {res.set_content(event({{"choices",json::array({{{"delta",{{"content","two words"}}}}})}})+"data: [DONE]\n\n","text/event-stream");return;}
            auto messages=body["messages"];std::string content=messages.back().value("content",std::string());
            int count=100;auto list=content.find("Here is a list of words:\n");
            if(list!=std::string::npos) {
                auto start=list+24,end=content.find("\n\nSummarize",start);std::istringstream words(content.substr(start,end-start));std::string word;count=20;
                while(words>>word)++count;
            }
            res.set_content(response(count,42,0),"text/event-stream");
        });
        port=server.bind_to_any_port("127.0.0.1");check(port>0,"bind mock");
        thread=std::thread([&]{server.listen_after_bind();});
        auto start=nb::Clock::now();while(!server.is_running() && nb::seconds(start)<2)std::this_thread::yield();
        check(server.is_running(),"start mock");
    }
    ~Mock(){server.stop();if(thread.joinable())thread.join();}
    std::string url()const{return "http://127.0.0.1:"+std::to_string(port);}
};
}
int main(int argc,char ** argv) {
    fs::path temp;
    try {
        auto expected="orbit copper canyon stone market anchor lantern velvet violet stone meadow garden stone market ember ember market signal market anchor ember stone violet lantern signal violet stone violet violet canyon stone signal stone anchor copper needle ember copper anchor lantern";
        check(nb::long_prompt(40)==std::string("Here is a list of words:\n")+expected+"\n\nSummarize what kinds of words appear in the list, in five sentences.","upstream Random(7) workload");
        check(nb::stat({}).is_null(),"empty statistics stay unknown");
        check(std::abs(nb::percentile({1,3,9},.95)-8.4)<1e-9,"interpolated p95");
        nb::Stream parser;auto wire=response();for(size_t i=0;i<wire.size();i+=7)parser.feed(wire.data()+i,std::min(size_t(7),wire.size()-i));
        auto row=parser.digest(200);check(row["ok"].get<bool>() && row["generated_tokens"]==42,"counts come from usage, not2 chunks");
        check(parser.text=="Hello world" && row["reused_tokens"]==60 && row["read_tokens"]==40,"SSE text and cache accounting");
        nb::Redactor redact("secret-canary");redact.add("test-host-name","<host>");
        auto clean=redact("secret-canary at192.168.1.3 on test-host-name /home/alice/private.json");
        check(clean.find("secret-canary")==std::string::npos && clean.find("192.168.1.3")==std::string::npos && clean.find("alice")==std::string::npos,"redaction");
        Mock mock;auto messages=json::array({{{"role","user"},{"content","hello"}}});
        nb::Client good(mock.url(),"","test",3);auto ok=good.chat(messages,64).digest();check(ok["ok"].get<bool>(),"native HTTP success");
        nb::Client bad(mock.url(),"","bad-stream",3);check(!bad.chat(messages,64).digest()["ok"].get<bool>(),"reject truncated stream");
        nb::Client err(mock.url(),"","http-error",3);auto failed=err.chat(messages,64).digest();check(!failed["ok"].get<bool>() && failed["http_status"]==503,"retain HTTP failure");
        nb::Client sse(mock.url(),"","sse-error",3);check(!sse.chat(messages,64).digest()["ok"].get<bool>(),"retain SSE error");
        nb::Client missing(mock.url(),"","missing-counts",3);auto unknown=missing.chat(messages,64).digest();
        check(unknown["generated_tokens"].is_null() && unknown["decode_tps"].is_null(),"never invent token counts or chunk-derived throughput");
        nb::Client slow(mock.url(),"","slow",1);auto began=nb::Clock::now();auto deadline=slow.chat(messages,64).digest();
        check(!deadline["ok"].get<bool>() && nb::seconds(began)<1.8,"request deadline is enforced");
        check(argc==2,"pass the benchmark executable");
        temp=fs::temp_directory_path()/("native-bench-test-"+std::to_string(getpid()));check(fs::create_directory(temp),"fresh test directory");
        auto output=temp/"success";
        check(command({argv[1],"--url",mock.url(),"--model","test","--out",output.string(),"--runs","1","--long","--clients","2","--notes","test-host-name 192.168.1.3"})==0,"complete CLI suite");
        auto result=json::parse(nb::read_file(output/"results.json"));
        check(result["complete"].get<bool>() && result["results"]["failures"].empty(),"complete successful result");
        check(result["results"]["chat"]["requests"].size()==3 && result["results"]["prompts"].size()==3,"chat and prompt workload counts");
        check(result["results"]["agent"]["sessions"][0].size()==6 && result["results"]["clients"]["rounds"][0]["requests"].size()==2,"agent/concurrency workload counts");
        check(nb::read_file(output/"results.json").find("192.168.1.3")==std::string::npos,"public report scrubs IP");
        check(command({argv[1],"--url",mock.url(),"--out",output.string()})!=0,"refuse overwrite");
        auto broken=temp/"failed";
        check(command({argv[1],"--url",mock.url(),"--model","bad-stream","--out",broken.string(),"--runs","1"})!=0,"failed suite returns nonzero");
        auto failed_suite=json::parse(nb::read_file(broken/"results.json"));
        check(!failed_suite["complete"].get<bool>() && !failed_suite["results"]["failures"].empty(),"failed suite preserves diagnostics");
        setenv("STRATA_KEY","secret-canary",1);auto private_error=temp/"private-error";
        check(command({argv[1],"--url",mock.url(),"--model","http-error","--out",private_error.string(),"--runs","1"})!=0,"HTTP failure CLI returns nonzero");
        auto saved=nb::read_file(private_error/"results.json")+nb::read_file(private_error/"requests.jsonl");
        check(saved.find("secret-canary")==std::string::npos && saved.find("192.168.1.3")==std::string::npos,"persisted error redaction");
        fs::remove_all(temp);std::cout<<"native benchmark tests: all passed\n";return 0;
    } catch(const std::exception & e) {std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
