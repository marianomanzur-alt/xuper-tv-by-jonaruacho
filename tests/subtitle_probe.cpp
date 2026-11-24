#include "subtitle_search.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
// Uso: subtitle_probe guess "<titulo>"            -> imprime titulo/temporada/episodio/anio
//      subtitle_probe zip <archivo.zip>           -> extrae y muestra tamano/primera linea
//      subtitle_probe search <puerto> <keys> <titulo> <temp> <ep> [indice]
int main(int argc,char** argv){
    if(argc>=3&&!std::strcmp(argv[1],"guess")){const auto q=xs::guess_subtitle_query(argv[2]);
        std::printf("title=[%s] season=%d episode=%d year=%d\n",q.title.c_str(),q.season,q.episode,q.year);return 0;}
    if(argc>=3&&!std::strcmp(argv[1],"zip")){std::ifstream f(argv[2],std::ios::binary);std::stringstream ss;ss<<f.rdbuf();
        std::string data,error;if(!xs::extract_subtitle_zip(ss.str(),data,error)){std::printf("error=%s\n",error.c_str());return 0;}
        std::printf("size=%zu first=[%s]\n",data.size(),data.substr(0,data.find('\n')).c_str());return 0;}
    if(argc>=3&&!std::strcmp(argv[1],"normalize")){std::ifstream f(argv[2],std::ios::binary);std::stringstream ss;ss<<f.rdbuf();std::string d=ss.str(),e;xs::SubtitleInfo info;
        if(!xs::normalize_subtitle(d,info,e)){std::printf("error=%s\n",e.c_str());return 0;}
        std::printf("cues=%zu last=%.1f bytes=%zu text=[%s]\n",info.cues,info.last_end,d.size(),(d.find("-->")==std::string::npos?std::string():d.substr(d.find('\n',d.find("-->"))+1,12)).c_str());return 0;}
    if(argc>=7&&!std::strcmp(argv[1],"search")){
        const std::string base=std::string("http://127.0.0.1:")+argv[2];const std::string keys=argv[3];
        xs::SubtitleSearchConfig c;c.allow_http=true;c.languages={"es","en"};
        c.opensubtitles_base=base+"/os";c.subdl_base=base+"/sd";c.subdl_download_base=base+"/dl";
        if(keys.find("os")!=std::string::npos){c.opensubtitles_key="OSKEY";c.opensubtitles_user="user";c.opensubtitles_password="pass";}
        if(keys.find("sd")!=std::string::npos)c.subdl_key="SDKEY";
        if(keys=="nologin"){c.opensubtitles_key="OSKEY";}
        xs::SubtitleSearch s(c);xs::SubtitleQuery q;q.title=argv[4];q.season=std::atoi(argv[5]);q.episode=std::atoi(argv[6]);
        xs::SubtitleContext ctx;if(const char* v=std::getenv("XS_FPS"))ctx.fps=std::atof(v);if(const char* v=std::getenv("XS_DUR"))ctx.duration=std::atof(v);
        std::atomic<bool> cancel{std::getenv("XS_CANCEL")!=nullptr};
        if(!std::strcmp(argv[1],"search")&&std::getenv("XS_AUTO")){
            xs::SubtitleHit chosen;std::string data,error;
            const bool ok=s.auto_fetch(q,ctx,chosen,data,error,&cancel);
            std::printf("auto=%d chosen=[%s] id=%s size=%zu error=[%s]\n",ok?1:0,chosen.label.c_str(),chosen.id.c_str(),data.size(),error.c_str());return 0;}
        std::vector<xs::SubtitleHit> hits;std::string error;
        const bool ok=s.search(q,hits,error,ctx,&cancel);
        std::printf("search=%d used=%s count=%zu error=[%s]\n",ok?1:0,s.used_provider().c_str(),hits.size(),error.c_str());
        for(const auto& h:hits)std::printf("hit %s|%s|%s|score=%d\n",h.provider.c_str(),h.language.c_str(),h.label.c_str(),h.score);
        if(ok&&!hits.empty()){const size_t i=argc>=8?static_cast<size_t>(std::atoi(argv[7])):0;std::string data,e2;
            const bool f=s.fetch(hits[i],data,e2);std::printf("fetch=%d size=%zu error=[%s] first=[%s]\n",f?1:0,data.size(),e2.c_str(),data.substr(0,data.find('\n')).c_str());}
        return 0;}
    return 2;}
