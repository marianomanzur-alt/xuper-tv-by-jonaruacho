// Sonda de pruebas del gestor de descargas. Uso: manager_probe BASE_URL DIRECTORIO ESCENARIO
#include "download_manager.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <curl/curl.h>
#include <thread>
using namespace std::chrono_literals;
static xs::Entry episode(const std::string& id,const std::string& title,const std::string& series="s1",const std::string& type="0"){
    xs::Entry e;e.kind=xs::EntryKind::Episode;e.content_id=id;e.title=title;e.series_id=series;e.program_type=type;e.artwork_cache=xs::artwork_key(id);return e;
}
static bool wait_idle(xs::DownloadManager& m,int seconds){
    for(int i=0;i<seconds*20;++i){if(!m.busy())return true;std::this_thread::sleep_for(50ms);}return false;
}
int main(int argc,char** argv){
    if(argc<4)return 2;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const std::string base=argv[1],dir=argv[2],scenario=argv[3];
    xs::DownloadManager::Config config;config.directory=dir;config.library_path=dir+"/../offline_library.json";config.free_margin=0;config.retry_delay_ms=50;
    xs::DownloadManager m(config);
    int resolves=0;
    auto resolver=[&](const xs::Entry& e){
        ++resolves;xs::SourceResult r;r.ok=true;
        std::string path="/movie.mp4";
        if(e.content_id=="hls")path="/list.m3u8";else if(e.content_id=="bad")path="/error.mp4";else if(e.content_id=="gone")path="/gone.mp4";
        else if(e.content_id=="cut")path="/cut.mp4";else if(e.content_id=="slow")path="/slow.mp4";else if(e.content_id=="mkv")path="/movie.mkv?x=1";
        else if(e.content_id=="auth"){path="/auth.mp4";r.headers={{"Content-Auth","ok"}};}
        else if(e.content_id=="nosource"){r.ok=false;r.status="El servicio no entrego la fuente.";return r;}
        r.source=base+path;
        r.subtitles={{"Espanol",base+"/sub1.srt",""},{"English",base+"/missing.srt",""},{"Portugues",base+"/sub2.srt",""}};
        return r;};
    auto fetch=[&](const xs::SubtitleSource& s,std::string& data,std::string& error){
        CURL* c=curl_easy_init();if(!c)return false;
        curl_easy_setopt(c,CURLOPT_URL,s.url.c_str());curl_easy_setopt(c,CURLOPT_FAILONERROR,1L);
        curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,+[](char* p,size_t n,size_t k,void* u){static_cast<std::string*>(u)->append(p,n*k);return n*k;});
        curl_easy_setopt(c,CURLOPT_WRITEDATA,&data);const auto rc=curl_easy_perform(c);curl_easy_cleanup(c);
        if(rc!=CURLE_OK){error="fallo";return false;}return true;};
    if(scenario=="nosession"){
        std::printf("nosession=%d\n",static_cast<int>(m.enqueue(episode("a","Episodio 1 - Uno"),"Serie",0)));
        return 0;
    }
    m.set_resolver(resolver,fetch);
    if(scenario=="basic"){
        std::printf("r1=%d\n",static_cast<int>(m.enqueue(episode("e1","Episodio 1 - Piloto"),"Mi Serie: Ñandú",0)));
        std::printf("r2=%d\n",static_cast<int>(m.enqueue(episode("e2","Episodio 2 - Otro"),"Mi Serie: Ñandú",1)));
        std::printf("dup=%d\n",static_cast<int>(m.enqueue(episode("e1","Episodio 1 - Piloto"),"Mi Serie: Ñandú",0)));
        std::printf("movie=%d\n",static_cast<int>(m.enqueue(episode("m1","Reproducir: Pelicula","","1"),"Una Pelicula",0)));
        std::printf("notep=%d\n",static_cast<int>(m.enqueue(xs::Entry{"x","",xs::EntryKind::Channel},"",0)));
        std::printf("idle=%d\n",wait_idle(m,30));
        std::printf("again=%d\n",static_cast<int>(m.enqueue(episode("e1","Episodio 1 - Piloto"),"Mi Serie: Ñandú",0)));
        for(std::string ev;!(ev=m.take_event()).empty();)std::printf("event=%s\n",ev.c_str());
        for(const auto& e:m.entries())std::printf("entry=%s|%s|%s|%s\n",e.content_id.c_str(),e.title.c_str(),e.source.c_str(),m.status_text(e.content_id).c_str());
        xs::OfflineItem item;std::printf("found=%d\n",m.find_downloaded("e1",item));
        if(m.find_downloaded("e1",item)){std::printf("subs=%zu size=%llu file=%s\n",item.subtitles.size(),static_cast<unsigned long long>(item.size),item.file.c_str());
            for(const auto& s:item.subtitles)std::printf("sub=%s|%s\n",s.label.c_str(),s.file.c_str());}
        std::printf("resolves=%d\n",resolves);
    }else if(scenario=="failures"){
        for(const char* id:{"hls","bad","gone","nosource"})m.enqueue(episode(id,std::string("Ep ")+id),"S",0);
        std::printf("idle=%d\n",wait_idle(m,30));
        for(std::string ev;!(ev=m.take_event()).empty();)std::printf("event=%s\n",ev.c_str());
        for(const auto& e:m.entries())std::printf("entry=%s|%s\n",e.content_id.c_str(),m.status_text(e.content_id).c_str());
        std::printf("resolves=%d\n",resolves);
        std::printf("retry=%d\n",static_cast<int>(m.enqueue(episode("gone","Ep gone"),"S",0)));
        std::printf("cancel_failed=%d\n",m.cancel("hls"));
    }else if(scenario=="retry_cut"){
        m.enqueue(episode("cut","Corte"),"S",0);
        std::printf("idle=%d\n",wait_idle(m,30));
        for(std::string ev;!(ev=m.take_event()).empty();)std::printf("event=%s\n",ev.c_str());
        std::printf("downloaded=%d resolves=%d\n",m.downloaded("cut"),resolves);
    }else if(scenario=="auth"){
        m.enqueue(episode("auth","Con cabecera"),"S",0);
        std::printf("idle=%d\n",wait_idle(m,30));
        std::printf("downloaded=%d\n",m.downloaded("auth"));
    }else if(scenario=="ext"){
        m.enqueue(episode("mkv","MKV"),"S",0);wait_idle(m,30);
        xs::OfflineItem item;m.find_downloaded("mkv",item);std::printf("file=%s\n",item.file.c_str());
    }else if(scenario=="cancel_pause"){
        m.enqueue(episode("slow","Lenta"),"S",0);m.enqueue(episode("e9","Otra"),"S",1);
        for(int i=0;i<100&&m.status_text("slow").rfind("DESCARGANDO",0)!=0;++i)std::this_thread::sleep_for(50ms);
        std::printf("active=%s\n",m.status_text("slow").c_str());
        m.set_paused(true);std::this_thread::sleep_for(1500ms);
        std::printf("paused=%s|%s\n",m.status_text("slow").c_str(),m.overlay().c_str());
        m.set_paused(false);
        for(int i=0;i<100&&m.status_text("slow").rfind("DESCARGANDO",0)!=0;++i)std::this_thread::sleep_for(50ms);
        std::printf("resumed=%s\n",m.status_text("slow").c_str());
        std::this_thread::sleep_for(300ms);
        std::printf("overlay=%s\n",m.overlay().c_str());
        std::printf("cancel_queued=%d\n",m.cancel("e9"));
        std::printf("cancel_active=%d\n",m.cancel("slow"));
        std::printf("idle=%d\n",wait_idle(m,20));
        std::printf("left=%zu downloaded=%d\n",m.entries().size(),m.downloaded("slow"));
        for(std::string ev;!(ev=m.take_event()).empty();)std::printf("event=%s\n",ev.c_str());
    }else if(scenario=="remove"){
        m.enqueue(episode("e1","Episodio 1 - Piloto"),"S",0);wait_idle(m,30);
        xs::OfflineItem item;m.find_downloaded("e1",item);
        std::printf("before=%d\n",m.downloaded("e1"));
        const bool removed=m.remove_downloaded("e1");std::printf("removed=%d after=%d\n",removed,m.downloaded("e1"));
    }
    curl_global_cleanup();return 0;
}
