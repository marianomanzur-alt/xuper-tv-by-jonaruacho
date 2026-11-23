#pragma once
// Indice de canales en la SD: evita volver a pedir todas las categorias de TV al buscar.
// Solo guarda metadatos de listado (titulo, codigo de canal, URL de portada); nunca
// URL de video, tokens ni datos de cuenta. Se ignora si es mas viejo que max_age.
#include "catalog.hpp"
#define JSON_NOEXCEPTION 1
#include "json.hpp"
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <vector>
namespace xs {
using ChannelIndex=std::map<int,std::vector<Entry>>;
inline bool load_channel_index(const std::string& path,long max_age_seconds,ChannelIndex& out,time_t now=std::time(nullptr)){
    using Json=nlohmann::json;
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f.good())return false;
    const auto length=f.tellg();
    if(length<=0||length>16*1024*1024)return false;
    f.seekg(0);std::string data(static_cast<size_t>(length),'\0');f.read(data.data(),length);
    if(!f)return false;
    const Json j=Json::parse(data,nullptr,false);
    if(!j.is_object()||!j.contains("saved")||!j["saved"].is_number_integer()||!j.contains("categories")||!j["categories"].is_object())return false;
    const long age=static_cast<long>(now)-j["saved"].get<long>();
    if(age<0||age>max_age_seconds)return false;
    ChannelIndex loaded;size_t total=0;
    for(auto it=j["categories"].begin();it!=j["categories"].end();++it){
        char* end=nullptr;const long id=std::strtol(it.key().c_str(),&end,10);
        if(!end||*end!='\0'||id<0||id>2147483647||!it.value().is_array())continue;
        std::vector<Entry> entries;
        for(const auto& row:it.value()){
            if(!row.is_array()||row.size()<2||!row[0].is_string()||!row[1].is_string())continue;
            Entry e;e.kind=EntryKind::Channel;e.column_id=static_cast<int>(id);
            e.title=row[0].get<std::string>();e.channel_code=row[1].get<std::string>();
            if(e.title.empty()||e.channel_code.empty()||e.channel_code.size()>512||e.title.size()>512)continue;
            if(row.size()>=3&&row[2].is_string()&&row[2].get_ref<const std::string&>().size()<=2048){e.artwork=row[2].get<std::string>();e.artwork_cache=artwork_key(e.artwork);}
            entries.push_back(std::move(e));
            if(++total>60000)return false;
        }
        loaded[static_cast<int>(id)]=std::move(entries);
    }
    if(loaded.empty())return false;
    out=std::move(loaded);return true;
}
inline bool save_channel_index(const std::string& path,const ChannelIndex& index,time_t now=std::time(nullptr)){
    using Json=nlohmann::json;
    Json categories=Json::object();
    for(const auto& category:index){
        Json rows=Json::array();
        for(const auto& e:category.second){
            if(e.kind!=EntryKind::Channel||e.channel_code.empty())continue;
            rows.push_back(Json::array({e.title,e.channel_code,e.artwork}));
        }
        categories[std::to_string(category.first)]=std::move(rows);
    }
    const std::string temp=path+".tmp",backup=path+".bak";
    {std::ofstream file(temp,std::ios::binary|std::ios::trunc);
        file<<Json({{"saved",static_cast<long long>(now)},{"categories",categories}}).dump();file.close();
        if(!file.good()){std::remove(temp.c_str());return false;}}
    // FAT no siempre permite renombrar sobre un destino existente: se conserva la copia anterior.
    std::ifstream existing(path,std::ios::binary);const bool primary=existing.good();existing.close();
    if(primary){
        if(std::remove(backup.c_str())!=0&&errno!=ENOENT)return false;
        if(std::rename(path.c_str(),backup.c_str())!=0)return false;
    }
    if(std::rename(temp.c_str(),path.c_str())!=0){
        if(primary)std::rename(backup.c_str(),path.c_str());
        return false;
    }
    std::remove(backup.c_str());return true;
}
}
