#include "download_manager.hpp"
#include "downloader.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
namespace xs {
namespace {
// Nombre de archivo seguro para FAT: solo ASCII, letras/numeros y guiones.
std::string slug(const std::string& text,size_t max){
    std::string out;bool dash=false;
    for(size_t i=0;i<text.size()&&out.size()<max;++i){
        const unsigned char c=text[i];char mapped=0;
        if(c==0xc3&&i+1<text.size()){
            const unsigned char d=text[++i];
            if(d==0x81||d==0xa1||d==0x80||d==0xa0||d==0x84||d==0xa4)mapped='a';
            else if(d==0x89||d==0xa9||d==0x88||d==0xa8)mapped='e';
            else if(d==0x8d||d==0xad)mapped='i';
            else if(d==0x93||d==0xb3||d==0x96||d==0xb6)mapped='o';
            else if(d==0x9a||d==0xba||d==0x9c||d==0xbc)mapped='u';
            else if(d==0x91||d==0xb1)mapped='n';
        }else if(c<128&&std::isalnum(c))mapped=static_cast<char>(std::tolower(c));
        if(mapped){out+=mapped;dash=false;}
        else if(!dash&&!out.empty()){out+='-';dash=true;}
    }
    while(!out.empty()&&out.back()=='-')out.pop_back();
    return out.empty()?"video":out;
}
std::string clip(std::string text,size_t max){
    if(text.size()<=max)return text;
    size_t n=max;while(n>0&&(static_cast<unsigned char>(text[n])&0xC0)==0x80)--n;
    text.resize(n);return text+"...";
}
void make_directory(const std::string& path){
    if(path.empty())return;
    ::mkdir(path.c_str(),0777);
}
uint64_t size_of(const std::string& path){
    struct stat info{};
    return stat(path.c_str(),&info)==0&&S_ISREG(info.st_mode)?static_cast<uint64_t>(info.st_size):0;
}
}
DownloadManager::DownloadManager(Config c):config(std::move(c)),library(config.library_path,config.directory){
    while(config.directory.size()>1&&config.directory.back()=='/')config.directory.pop_back();
    make_directory(config.directory);
    library.prune_missing();
    worker=std::thread([this]{run();});
}
DownloadManager::~DownloadManager(){
    stop.store(true);cancel_active.store(true);
    {std::lock_guard<std::mutex> lock(mutex);}
    wake.notify_all();
    if(worker.joinable())worker.join();
}
void DownloadManager::set_resolver(SourceResolver r,SubtitleFetcher f){
    {std::lock_guard<std::mutex> lock(mutex);resolver=std::move(r);fetcher=std::move(f);}
    wake.notify_all();
}
EnqueueResult DownloadManager::enqueue(const Entry& e,const std::string& show,int order){
    if(e.kind!=EntryKind::Episode||e.content_id.empty()||e.content_id.size()>512)return EnqueueResult::NotSupported;
    std::lock_guard<std::mutex> lock(mutex);
    if(library.find(e.content_id)&&size_of(library.find(e.content_id)->file))return EnqueueResult::AlreadyDownloaded;
    for(auto& job:jobs)if(job.entry.content_id==e.content_id){
        if(job.state!=Job::State::Failed)return EnqueueResult::AlreadyQueued;
        job.state=Job::State::Queued;job.error.clear();++batch_total;
        wake.notify_all();return EnqueueResult::Retrying;
    }
    if(!resolver)return EnqueueResult::NoSession;
    if(jobs.size()>=200)return EnqueueResult::QueueFull;
    Job job;job.entry=e;job.order=std::max(order,0);
    // Pelicula: el nombre del titulo. Serie: "Serie - Episodio N - nombre".
    job.title=show.empty()?e.title:e.program_type=="1"?show:show+" - "+e.title;
    job.title=clip(job.title,200);
    job.base=slug(job.title,40)+"-"+artwork_key(e.content_id).substr(0,8);
    jobs.push_back(std::move(job));++batch_total;
    wake.notify_all();return EnqueueResult::Added;
}
bool DownloadManager::cancel(const std::string& id){
    std::lock_guard<std::mutex> lock(mutex);
    const auto it=std::find_if(jobs.begin(),jobs.end(),[&](const Job& j){return j.entry.content_id==id;});
    if(it==jobs.end())return false;
    if(it->state==Job::State::Active){cancel_active.store(true);return true;}  // el hilo la retira al terminar
    std::remove((config.directory+"/"+it->base+".part").c_str());
    if(it->state==Job::State::Queued&&batch_total>0)--batch_total;
    jobs.erase(it);return true;
}
bool DownloadManager::remove_downloaded(const std::string& id){
    std::lock_guard<std::mutex> lock(mutex);
    return library.remove(id);
}
void DownloadManager::set_paused(bool value){
    paused.store(value);wake.notify_all();
}
bool DownloadManager::downloaded(const std::string& id)const{
    std::lock_guard<std::mutex> lock(mutex);
    return library.find(id)!=nullptr;
}
bool DownloadManager::find_downloaded(const std::string& id,OfflineItem& out)const{
    std::lock_guard<std::mutex> lock(mutex);
    const auto* item=library.find(id);if(!item||!size_of(item->file))return false;
    out=*item;return true;
}
size_t DownloadManager::downloaded_count()const{
    std::lock_guard<std::mutex> lock(mutex);return library.items.size();
}
std::string DownloadManager::status_text(const std::string& id)const{
    std::lock_guard<std::mutex> lock(mutex);
    for(const auto& job:jobs)if(job.entry.content_id==id){
        if(job.state==Job::State::Failed)return "ERROR: "+clip(job.error,60);
        if(job.state==Job::State::Queued)return paused.load()?"EN PAUSA":"EN COLA";
        const uint64_t total=active_total.load(),done=active_done.load();
        if(paused.load())return "EN PAUSA";
        return total?"DESCARGANDO "+std::to_string(static_cast<int>(std::min<uint64_t>(100,done*100/total)))+"%":"DESCARGANDO...";
    }
    if(const auto* item=library.find(id))return "DESCARGADO "+format_bytes(item->size);
    return {};
}
std::vector<Entry> DownloadManager::entries()const{
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<Entry> out;
    for(const auto& job:jobs){
        Entry e;e.kind=EntryKind::Offline;e.title=job.title;e.content_id=job.entry.content_id;e.series_id=job.entry.series_id;
        e.program_type=job.entry.program_type;e.artwork_cache=job.entry.artwork_cache;  // source vacio: aun no se puede reproducir
        out.push_back(std::move(e));
    }
    auto saved=library.entries();
    out.insert(out.end(),std::make_move_iterator(saved.begin()),std::make_move_iterator(saved.end()));
    return out;
}
bool DownloadManager::busy()const{
    std::lock_guard<std::mutex> lock(mutex);
    return std::any_of(jobs.begin(),jobs.end(),[](const Job& j){return j.state!=Job::State::Failed;});
}
std::string DownloadManager::overlay()const{
    std::lock_guard<std::mutex> lock(mutex);
    size_t pending=0;const Job* current=nullptr;
    for(const auto& j:jobs){if(j.state==Job::State::Failed)continue;++pending;if(j.state==Job::State::Active)current=&j;}
    if(!pending)return {};
    const size_t index=std::min(batch_done+1,std::max(batch_total,pending));
    const std::string count=std::to_string(index)+"/"+std::to_string(std::max(batch_total,pending));
    if(paused.load())return "Descargas en pausa ("+count+") mientras se reproduce";
    if(!current)return "Descarga en cola "+count;
    const uint64_t total=active_total.load(),done=active_done.load();
    std::string text="Descargando "+count+"  ";
    if(total)text+=std::to_string(static_cast<int>(std::min<uint64_t>(100,done*100/total)))+"%  "+format_bytes(done)+" / "+format_bytes(total);
    else text+=format_bytes(done);
    return text;
}
std::string DownloadManager::take_event(){
    std::lock_guard<std::mutex> lock(mutex);
    if(events.empty())return {};
    std::string e=std::move(events.front());events.erase(events.begin());return e;
}
void DownloadManager::prune(){
    std::lock_guard<std::mutex> lock(mutex);library.prune_missing();
}
void DownloadManager::sleep_interruptible(int ms){
    for(int waited=0;waited<ms&&!stop.load()&&!cancel_active.load()&&!paused.load();waited+=100)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
void DownloadManager::run(){
    for(;;){
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex);
            auto next=[&]{return std::find_if(jobs.begin(),jobs.end(),[](const Job& j){return j.state==Job::State::Queued;});};
            wake.wait(lock,[&]{return stop.load()||(!paused.load()&&resolver&&next()!=jobs.end());});
            if(stop.load())return;
            auto it=next();it->state=Job::State::Active;job=*it;active_id=job.entry.content_id;
            cancel_active.store(false);active_done.store(0);active_total.store(0);
        }
        std::string message;
        const Outcome outcome=process(job,message);
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it=std::find_if(jobs.begin(),jobs.end(),[&](const Job& j){return j.entry.content_id==job.entry.content_id;});
            active_id.clear();
            if(it!=jobs.end()){
                if(outcome==Outcome::Paused){it->state=Job::State::Queued;}
                else if(outcome==Outcome::Failed){it->state=Job::State::Failed;it->error=message;++batch_done;
                    events.push_back("Descarga fallida: "+clip(job.title,60)+" - "+message);}
                else{
                    if(outcome==Outcome::Done){
                        const auto* item=library.find(job.entry.content_id);
                        events.push_back("Descarga completa: "+clip(job.title,60)+(item?" ("+format_bytes(item->size)+")":""));
                    }else if(!stop.load()){
                        std::remove((config.directory+"/"+job.base+".part").c_str());
                        events.push_back("Descarga cancelada: "+clip(job.title,60));
                    }
                    jobs.erase(it);++batch_done;
                }
            }
            const bool pending=std::any_of(jobs.begin(),jobs.end(),[](const Job& j){return j.state!=Job::State::Failed;});
            if(!pending){batch_done=0;batch_total=0;}
        }
    }
}
DownloadManager::Outcome DownloadManager::process(const Job& job,std::string& message){
    SourceResolver resolve;SubtitleFetcher fetch;
    {std::lock_guard<std::mutex> lock(mutex);resolve=resolver;fetch=fetcher;}
    if(!resolve){message="Sin sesion: abre la app con conexion.";return Outcome::Failed;}
    const std::string part=config.directory+"/"+job.base+".part";
    std::vector<OfflineSubtitle> subtitles;bool subtitles_done=false;
    auto drop_subtitles=[&]{for(const auto& s:subtitles)std::remove(s.file.c_str());subtitles.clear();};
    SourceResult source;bool have_source=false;std::string last_error="Sin respuesta del servicio.";
    for(int attempt=0;attempt<std::max(1,config.max_attempts);++attempt){
        if(stop.load()||cancel_active.load()){drop_subtitles();return Outcome::Cancelled;}
        if(paused.load()){drop_subtitles();return Outcome::Paused;}
        if(attempt>0){sleep_interruptible(config.retry_delay_ms*attempt);
            if(stop.load()||cancel_active.load()){drop_subtitles();return Outcome::Cancelled;}
            if(paused.load()){drop_subtitles();return Outcome::Paused;}}
        if(!have_source){
            source=resolve(job.entry);
            if(!source.ok||source.source.empty()){
                last_error=source.status.empty()?"El servicio no entrego la fuente.":source.status;
                if(attempt+1<std::max(1,config.max_attempts))continue;
                message=last_error;drop_subtitles();return Outcome::Failed;
            }
            have_source=true;
        }
        if(!subtitles_done){
            subtitles_done=true;
            if(fetch)for(size_t i=0;i<source.subtitles.size()&&i<8;++i){
                std::string data,error;
                if(!fetch(source.subtitles[i],data,error)||data.empty())continue;
                const std::string file=config.directory+"/"+job.base+"."+std::to_string(i+1)+".srt";
                std::ofstream out(file,std::ios::binary|std::ios::trunc);out.write(data.data(),static_cast<std::streamsize>(data.size()));out.close();
                if(!out.good()){std::remove(file.c_str());continue;}
                std::string label=source.subtitles[i].language.empty()?"Subtitulo "+std::to_string(i+1):source.subtitles[i].language;
                if(label.size()>80)label.resize(80);
                subtitles.push_back({label,file});
            }
        }
        const std::string final_path=config.directory+"/"+job.base+"."+video_extension(source.source);
        DownloadRequest request;request.url=source.source;request.part_path=part;request.final_path=final_path;
        request.ca_file=config.ca_file;request.headers=source.headers;request.free_margin=config.free_margin;
        const auto result=download_file(request,cancel_active,paused,[this](uint64_t done,uint64_t total){
            active_done.store(done);active_total.store(total);});
        switch(result.status){
            case DownloadStatus::Ok:{
                OfflineItem item;item.content_id=job.entry.content_id;item.series_id=job.entry.series_id;item.program_type=job.entry.program_type;
                item.title=job.title;item.file=final_path;item.art=job.entry.artwork_cache;item.size=result.bytes;item.order=job.order;
                item.saved=static_cast<long long>(std::time(nullptr));item.subtitles=subtitles;
                std::lock_guard<std::mutex> lock(mutex);
                if(!library.add(std::move(item))){message="No se pudo guardar el registro de descargas.";return Outcome::Failed;}
                return Outcome::Done;}
            case DownloadStatus::Cancelled:drop_subtitles();return Outcome::Cancelled;
            case DownloadStatus::Paused:drop_subtitles();return Outcome::Paused;
            default:break;
        }
        last_error=result.message;
        if(result.retry&&attempt+1<std::max(1,config.max_attempts)){
            if(result.refresh_url)have_source=false;  // la URL firmada pudo caducar: se pide otra
            continue;
        }
        message=last_error;drop_subtitles();
        if(result.status==DownloadStatus::Hls||result.status==DownloadStatus::Space)std::remove(part.c_str());
        return Outcome::Failed;
    }
    message=last_error;drop_subtitles();return Outcome::Failed;
}
}
