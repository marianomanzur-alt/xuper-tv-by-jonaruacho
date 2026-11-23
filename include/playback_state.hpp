#pragma once
#define JSON_NOEXCEPTION 1
#include "json.hpp"
#include <fstream>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <utility>
#include <cerrno>
#include <string>
#include <cctype>
// Only content IDs and playback preferences are persisted; never stream URLs or tokens.
namespace xs {
struct PlaybackHistory {
    using Json=nlohmann::json;
#ifdef __SWITCH__
    static constexpr const char* default_path="sdmc:/switch/xuperswitch/playback_state.json";
#else
    static constexpr const char* default_path="playback_state.json";
#endif
    std::string path;
    const char* last_error="none";
    Json positions=Json::object();double speed=1,volume=100;bool auto_next=true;
    // Preferencias que se recuerdan entre videos: tamano de subtitulos, calidad de imagen
    // (0..2), idioma preferido de audio/subtitulos y el archivo .srt elegido por contenido.
    double sub_scale=1;int quality=0;std::string alang,slang;
    Json subtitle_files=Json::object(),series_subtitles=Json::object();
    static bool lang_ok(const std::string& v){
        if(v.size()<2||v.size()>8)return false;
        return std::all_of(v.begin(),v.end(),[](unsigned char c){return std::isalnum(c)||c=='-';});
    }
    static bool path_ok(const std::string& v){
        return !v.empty()&&v.size()<=512&&std::none_of(v.begin(),v.end(),[](unsigned char c){return c<32||c==127;});
    }
    static Json clean_paths(const Json& source){
        Json out=Json::object();
        if(!source.is_object())return out;
        for(auto it=source.begin();it!=source.end();++it)
            if(it.key().size()<=512&&it.value().is_string()&&path_ok(it.value().get<std::string>()))out[it.key()]=it.value();
        return out;
    }
    std::string subtitle_for(const std::string& content_id)const{
        const auto i=subtitle_files.find(content_id);
        return i!=subtitle_files.end()&&i->is_string()?i->get<std::string>():"";
    }
    std::string series_hint(const std::string& series_id)const{
        const auto i=series_subtitles.find(series_id);
        return i!=series_subtitles.end()&&i->is_string()?i->get<std::string>():"";
    }
    // file vacio: el usuario quito el subtitulo externo de este contenido.
    void remember_subtitle(const std::string& content_id,const std::string& series_id,const std::string& file){
        if(content_id.empty()||content_id.size()>512)return;
        if(file.empty()){subtitle_files.erase(content_id);return;}
        if(!path_ok(file))return;
        subtitle_files[content_id]=file;
        if(!series_id.empty()&&series_id.size()<=512)series_subtitles[series_id]=file;
        while(subtitle_files.size()>300)subtitle_files.erase(subtitle_files.begin());
        while(series_subtitles.size()>300)series_subtitles.erase(series_subtitles.begin());
    }
    explicit PlaybackHistory(std::string filename=default_path):path(std::move(filename)){
        // Recover a save interrupted between the two renames.
        auto read=[](const std::string& filename){
            std::ifstream f(filename,std::ios::binary|std::ios::ate);const auto length=f.tellg();
            if(length<0||length>1024*1024)return Json();
            f.seekg(0);std::string data(static_cast<size_t>(length),'\0');f.read(data.data(),length);
            if(!f)return Json();
            return Json::parse(data,nullptr,false);
        };
        auto j=read(path);if(!j.is_object())j=read(path+".bak");if(!j.is_object())return;
        if(j.contains("positions")&&j["positions"].is_object())positions=j["positions"];
        if(j.contains("speed")&&j["speed"].is_number())speed=std::clamp(j["speed"].get<double>(),0.5,2.0);
        if(j.contains("volume")&&j["volume"].is_number())volume=std::clamp(j["volume"].get<double>(),0.0,100.0);
        if(j.contains("auto_next")&&j["auto_next"].is_boolean())auto_next=j["auto_next"].get<bool>();
        if(j.contains("sub_scale")&&j["sub_scale"].is_number())sub_scale=std::clamp(j["sub_scale"].get<double>(),0.5,3.0);
        if(j.contains("quality")&&j["quality"].is_number_integer())quality=std::clamp(j["quality"].get<int>(),0,2);
        if(j.contains("alang")&&j["alang"].is_string()&&lang_ok(j["alang"].get<std::string>()))alang=j["alang"].get<std::string>();
        if(j.contains("slang")&&j["slang"].is_string()&&lang_ok(j["slang"].get<std::string>()))slang=j["slang"].get<std::string>();
        if(j.contains("subtitle_files"))subtitle_files=clean_paths(j["subtitle_files"]);
        if(j.contains("series_subtitles"))series_subtitles=clean_paths(j["series_subtitles"]);
    }
    double position(const std::string& id)const{const auto i=positions.find(id);
        if(i==positions.end()||!i->is_number())return 0;
        const double n=i->get<double>();return std::isfinite(n)&&n>5&&n<86400?n:0;}
    bool save(const std::string& id,double position_value,double speed_value,double volume_value,bool next,bool ended){
        last_error="invalid_preferences";
        if(!std::isfinite(speed_value)||!std::isfinite(volume_value))return false;
        speed=std::clamp(speed_value,0.5,2.0);volume=std::clamp(volume_value,0.0,100.0);auto_next=next;
        if(!id.empty()&&id.size()<=512){if(ended)positions.erase(id);else if(std::isfinite(position_value)&&position_value>5&&position_value<86400)positions[id]=position_value;else if(position_value>=0&&position_value<=5)positions.erase(id);}
        while(positions.size()>500)positions.erase(positions.begin());
        const std::string temp=path+".tmp";std::ofstream file(temp,std::ios::trunc);
        file<<Json({{"positions",positions},{"speed",speed},{"volume",volume},{"auto_next",auto_next},{"sub_scale",sub_scale},{"quality",quality},{"alang",alang},{"slang",slang},{"subtitle_files",subtitle_files},{"series_subtitles",series_subtitles}}).dump();file.close();
        if(!file.good()){last_error="write";return false;}
        const std::string backup=path+".bak";
        // FAT may not support replacing an existing destination by rename.
        // Keep the last complete file until the new file is installed.
        std::ifstream existing(path,std::ios::binary);const bool primary=existing.good();existing.close();
        if(primary){
            if(std::remove(backup.c_str())!=0&&errno!=ENOENT){last_error="backup_cleanup";return false;}
            if(std::rename(path.c_str(),backup.c_str())!=0){last_error="backup";return false;}
        }
        if(std::rename(temp.c_str(),path.c_str())!=0){
            last_error="install";
            if(primary&&std::rename(backup.c_str(),path.c_str())!=0)last_error="rollback";
            return false;
        }
        std::remove(backup.c_str());last_error="none";return true;
    }
};
}
