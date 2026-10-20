// Native port of the speed protocol in VecSzn/PolyStrata tools/strata_bench.py,
// ab_engine.py, agent_turns_bench.py and serve_load.py at 26e4cb5 (MIT).
// No inference, tool execution, model downloads or Python dependency in this client.
#pragma once
#include "httplib.h"
#include "json.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <pwd.h>
#include <unistd.h>

namespace native_bench {
using json = nlohmann::ordered_json;
using Clock = std::chrono::steady_clock;
inline double seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now()-start).count(); }
inline std::string read_file(const std::filesystem::path & path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f), {});
}
inline std::string trim(std::string s) {
    const auto a=s.find_first_not_of(" \t\r\n");
    if (a==std::string::npos) return {};
    return s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
}
inline json number(const json & j,const std::string & key) {
    if(!j.contains(key) || !j[key].is_number())return nullptr;
    auto value=j[key].get<double>();return std::isfinite(value) && value>=0?j[key]:json(nullptr);
}
inline std::string text(const json & j,const std::string & key) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}
inline double numeric(const json & j,double fallback=0) { return j.is_number()?j.get<double>():fallback; }
inline double percentile(std::vector<double> v,double p) {
    if (v.empty()) throw std::runtime_error("percentile needs observations");
    std::sort(v.begin(),v.end()); double k=(v.size()-1)*p;
    auto lo=static_cast<size_t>(k),hi=std::min(lo+1,v.size()-1);
    return v[lo]+(v[hi]-v[lo])*(k-lo);
}
inline json stat(const std::vector<double> & v) {
    if (v.empty()) return nullptr;
    return {{"median",percentile(v,.5)},{"min",*std::min_element(v.begin(),v.end())},
            {"max",*std::max_element(v.begin(),v.end())},{"n",v.size()}};
}
inline json column_stat(const json & rows,const std::string & key) {
    std::vector<double> values;
    for (const auto & row:rows) if (row.value("ok",false)) {
        auto value=number(row,key); if (value.is_number()) values.push_back(value.get<double>());
    }
    return stat(values);
}

// CPython random.Random(7), integer seed, MT19937 init_by_array and _randbelow(20).
// std::mt19937(7) would generate a different workload. Checked against frozen upstream words.
class WordsRandom {
    std::array<uint32_t,624> mt{}; size_t index=624;
public:
    WordsRandom() {
        mt[0]=19650218U;
        for (size_t i=1;i<624;++i) mt[i]=1812433253U*(mt[i-1]^(mt[i-1]>>30))+uint32_t(i);
        size_t i=1;
        for (size_t k=0;k<624;++k) {
            mt[i]=(mt[i]^((mt[i-1]^(mt[i-1]>>30))*1664525U))+7U;
            if (++i>=624) {mt[0]=mt[623];i=1;}
        }
        for (size_t k=0;k<623;++k) {
            mt[i]=(mt[i]^((mt[i-1]^(mt[i-1]>>30))*1566083941U))-uint32_t(i);
            if (++i>=624) {mt[0]=mt[623];i=1;}
        }
        mt[0]=0x80000000U;
    }
    uint32_t next() {
        if (index>=624) {
            for (size_t i=0;i<624;++i) {
                uint32_t y=(mt[i]&0x80000000U)|(mt[(i+1)%624]&0x7fffffffU);
                mt[i]=mt[(i+397)%624]^(y>>1)^((y&1)?0x9908b0dfU:0);
            }
            index=0;
        }
        uint32_t y=mt[index++]; y^=y>>11; y^=(y<<7)&0x9d2c5680U; y^=(y<<15)&0xefc60000U; y^=y>>18;
        return y;
    }
    size_t choice() { uint32_t n; do {n=next()>>27;} while(n>=20); return n; }
};
inline const std::array<std::string,3> prompts={
    "Write a Python function that merges two sorted lists into one sorted list, with a docstring and two tests.",
    "Explain in two paragraphs how a refrigerator moves heat from inside to outside.",
    "List ten practical tips for learning a new language as an adult, one sentence each."};
inline std::string long_prompt(size_t words) {
    static const std::array<const char *,20> vocab={"river","stone","market","lantern","copper","winter","garden",
        "signal","harbor","needle","orbit","velvet","canyon","ember","ledger","quartz","meadow","anchor","violet","thunder"};
    WordsRandom rng; std::string result="Here is a list of words:\n";
    for (size_t i=0;i<words;++i) {if(i)result+=' ';result+=vocab[rng.choice()];}
    return result+"\n\nSummarize what kinds of words appear in the list, in five sentences.";
}
inline const std::string agent_system=
    "You are an autonomous coding agent working in the user's repository. You can read files, search the code, "
    "edit files and run commands with the tools below. Work step by step: read the code you need before changing "
    "it, make small and correct edits, keep the existing style, and explain briefly what you did. Never invent "
    "file contents: read them. When the task is done, give a short summary of the changes and how to test them.";
inline json tools() {
    return json::parse(R"json([
      {"type":"function","function":{"name":"read_file","description":"Read a file of the repository (UTF-8 text).","parameters":{"type":"object","properties":{"path":{"type":"string","description":"path from the repository root"}},"required":["path"]}}},
      {"type":"function","function":{"name":"grep","description":"Search the repository for a regular expression; returns matching lines.","parameters":{"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string"}},"required":["pattern"]}}},
      {"type":"function","function":{"name":"edit_file","description":"Replace an exact string in a file with a new one.","parameters":{"type":"object","properties":{"path":{"type":"string"},"old":{"type":"string"},"new":{"type":"string"}},"required":["path","old","new"]}}},
      {"type":"function","function":{"name":"run","description":"Run a shell command in the repository and return its output.","parameters":{"type":"object","properties":{"command":{"type":"string"}},"required":["command"]}}}
    ])json");
}

class Redactor {
    std::vector<std::pair<std::string,std::string>> values;
public:
    explicit Redactor(const std::string & key={}) {
        if(!key.empty())values.emplace_back(key,"<key>");
        if(const char * home=std::getenv("HOME")) add(home,"<home>");
        add(std::filesystem::current_path().string(),"<checkout>");
        char host[256]{}; if(gethostname(host,sizeof(host)-1)==0)add(host,"<host>");
        if(auto * user=getpwuid(geteuid()))add(user->pw_name,"<user>");
    }
    void add(const std::string & value,const std::string & label) {if(value.size()>=4)values.emplace_back(value,label);}
    std::string operator()(std::string value) const {
        for(const auto & item:values) {
            size_t at=0; while((at=value.find(item.first,at))!=std::string::npos) {
                value.replace(at,item.first.size(),item.second);at+=item.second.size();
            }
        }
        value=std::regex_replace(value,std::regex(R"((^|[^0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9]))"),"$1<ip>");
        value=std::regex_replace(value,std::regex(R"(\[[0-9a-fA-F:]+\])"),"<ip>");
        value=std::regex_replace(value,std::regex(R"((?:/home/|/Users/|[A-Za-z]:[\\/]Users[\\/])[^\s"<>]+)"),"<home-path>");
        return value;
    }
    json object(const json & value) const {
        if(value.is_string())return (*this)(value.get<std::string>());
        if(value.is_array()) {json out=json::array();for(const auto & v:value)out.push_back(object(v));return out;}
        if(value.is_object()) {json out=json::object();for(auto it=value.begin();it!=value.end();++it)out[it.key()]=object(it.value());return out;}
        return value;
    }
};

struct Stream {
    Clock::time_point start=Clock::now();
    json first=nullptr,usage=json::object(),timings=json::object();
    std::string buffer,text,error; bool done=false; int pieces=0,http_status=0; double total=0;
    void line(std::string line) {
        line=trim(line);if(line.rfind("data:",0)!=0)return;
        auto data=trim(line.substr(5));
        if(data=="[DONE]") {done=true;total=seconds(start);return;}
        json event=json::parse(data,nullptr,false);
        if(event.is_discarded() || !event.is_object())return;
        if(event.contains("error"))error=event["error"].dump().substr(0,300);
        auto choices=event.value("choices",json::array());
        if(choices.is_array())for(const auto & ch:choices) {
            if(!ch.is_object())continue;
            auto delta=ch.value("delta",json::object());if(!delta.is_object())continue;
            auto content=native_bench::text(delta,"content"),reason=native_bench::text(delta,"reasoning_content");
            if(!content.empty() || !reason.empty()) {if(first.is_null())first=seconds(start);++pieces;text+=content;}
        }
        if(event.contains("usage") && event["usage"].is_object())usage=event["usage"];
        if(event.contains("timings") && event["timings"].is_object())timings=event["timings"];
    }
    bool feed(const char * data,size_t size) {
        buffer.append(data,size);if(buffer.size()>4*1024*1024){error="SSE frame exceeds 4 MiB";return false;}
        size_t at;
        while((at=buffer.find('\n'))!=std::string::npos) {
            auto current=buffer.substr(0,at);buffer.erase(0,at+1);line(current);if(done)return false;
        }
        return true;
    }
    json digest(int status=-1) {
        if(status<0)status=http_status;
        auto generated=number(usage,"completion_tokens");
        if(generated.is_null())generated=number(timings,"predicted_n");
        auto prompt=number(usage,"prompt_tokens");
        auto cached=usage.contains("prompt_tokens_details") && usage["prompt_tokens_details"].is_object()?
            number(usage["prompt_tokens_details"],"cached_tokens"):json(nullptr);
        if(cached.is_null())cached=number(timings,"cache_n");
        auto read=number(timings,"prompt_n");
        if(read.is_null() && prompt.is_number() && cached.is_number())read=numeric(prompt)-numeric(cached);
        auto decode=number(timings,"predicted_per_second");std::string source=decode.is_number()?"engine":"unavailable";
        if(decode.is_null() && generated.is_number() && numeric(generated)>1 && first.is_number() && total>numeric(first)) {
            decode=(numeric(generated)-1)/(total-numeric(first));source="client";
        }
        bool ok=status==200 && done && first.is_number() && error.empty();
        return {{"ok",ok},{"http_status",status},{"err",error.empty()?json(nullptr):json(error)},
            {"stream_complete",done},{"ttft_s",first},{"total_s",total},{"prompt_tokens",prompt},
            {"reused_tokens",cached},{"read_tokens",read},{"generated_tokens",generated},
            {"prompt_tps",number(timings,"prompt_per_second")},{"decode_tps",decode},{"decode_tps_source",source},
            {"prompt_ms",number(timings,"prompt_ms")},{"decode_ms",number(timings,"predicted_ms")},
            {"drafts_offered",number(timings,"draft_n")},{"drafts_accepted",number(timings,"draft_n_accepted")}};
    }
};

class Client {
    std::string base,key,model;int timeout;
    void configure(httplib::Client & client) const {
        client.set_connection_timeout(10);client.set_read_timeout(timeout);client.set_write_timeout(timeout);
        client.set_max_timeout(timeout*1000);client.set_follow_location(false);
    }
public:
    Client(std::string base,std::string key,std::string model,int timeout):base(std::move(base)),key(std::move(key)),
        model(std::move(model)),timeout(timeout) {
        if(this->base.rfind("http://",0)!=0)throw std::runtime_error("--url must use http:// (TLS is not built into this client)");
        while(!this->base.empty() && this->base.back()=='/')this->base.pop_back();
        if(this->base.size()>3 && this->base.substr(this->base.size()-3)=="/v1")this->base.resize(this->base.size()-3);
        if(this->base.find('@')!=std::string::npos || this->base.find('?')!=std::string::npos || this->base.find('#')!=std::string::npos)
            throw std::runtime_error("Use a plain origin URL; put the API key in its environment variable");
    }
    json get(const std::string & path) const {
        httplib::Client client(base);configure(client);httplib::Headers headers;
        if(!key.empty())headers.emplace("Authorization","Bearer "+key);
        auto response=client.Get(path,headers);
        if(!response || response->status!=200)return json::object();
        auto out=json::parse(response->body,nullptr,false);return out.is_object()?out:json::object();
    }
    Stream chat(const json & messages,int cap,bool thinking=false,const json & tools=json::array()) const {
        json body={{"model",model},{"stream",true},{"max_tokens",cap},{"temperature",0},{"messages",messages}};
        if(!thinking) {body["reasoning_effort"]="none";body["chat_template_kwargs"]={{"enable_thinking",false}};}
        if(!tools.empty())body["tools"]=tools;
        httplib::Client client(base);configure(client);httplib::Request req;req.method="POST";req.path="/v1/chat/completions";
        req.body=body.dump();req.headers.emplace("Content-Type","application/json");
        if(!key.empty())req.headers.emplace("Authorization","Bearer "+key);
        Stream stream;int status=0;std::string error_body;
        req.response_handler=[&](const httplib::Response & r) {status=r.status;stream.http_status=status;return true;};
        req.content_receiver=[&](const char * data,size_t n,uint64_t,uint64_t) {
            if(status!=200) {error_body.append(data,std::min(n,size_t(300)-error_body.size()));return error_body.size()<300;}
            return stream.feed(data,n);
        };
        auto response=client.send(req);
        if(!stream.done) {
            if(!stream.buffer.empty())stream.line(stream.buffer);
            stream.total=seconds(stream.start);
        }
        if(status!=200)stream.error="HTTP "+std::to_string(status)+": "+error_body;
        else if(!stream.done && stream.error.empty())stream.error="Stream ended without [DONE]";
        if(!response && !stream.done && status==0)stream.error="HTTP transport failed: "+httplib::to_string(response.error());
        return stream;
    }
};
} // namespace native_bench
