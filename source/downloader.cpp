#include "downloader.hpp"
#include "catalog.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#if defined(__has_include)
#if __has_include(<sys/statvfs.h>)
#include <sys/statvfs.h>
#define XS_HAVE_STATVFS 1
#endif
#endif
namespace xs {
namespace {
struct Transfer {
    FILE* file=nullptr;
    uint64_t resume=0;
    const std::atomic<bool>* cancel=nullptr;
    const std::atomic<bool>* pause=nullptr;
    const DownloadProgress* progress=nullptr;
    std::string directory;
    uint64_t margin=0;
    bool first_chunk=true,space_checked=false;
    bool hls=false,invalid=false,write_failed=false;
    DownloadStatus abort_status=DownloadStatus::Network;
    bool aborted=false;
};
static size_t on_data(char* data,size_t size,size_t count,void* user){
    auto* t=static_cast<Transfer*>(user);
    const size_t bytes=size*count;
    if(!bytes)return 0;
    // Solo al empezar de cero: una lista HLS o una pagina de error no son un archivo de video.
    if(t->first_chunk&&t->resume==0){
        t->first_chunk=false;
        const std::string head(data,std::min<size_t>(bytes,16));
        if(head.rfind("#EXTM3U",0)==0){t->hls=true;return 0;}
        const auto first=head.find_first_not_of(" \r\n\t");
        if(first!=std::string::npos&&(head[first]=='<'||head[first]=='{')){t->invalid=true;return 0;}
    }
    t->first_chunk=false;
    if(std::fwrite(data,1,bytes,t->file)!=bytes){t->write_failed=true;return 0;}
    return bytes;
}
// Devuelve false si no se puede saber (sistema de archivos sin statvfs).
static bool free_space(const std::string& directory,uint64_t& out){
#ifdef XS_HAVE_STATVFS
    struct statvfs info{};
    if(statvfs(directory.empty()?".":directory.c_str(),&info)!=0)return false;
    out=static_cast<uint64_t>(info.f_bavail)*static_cast<uint64_t>(info.f_frsize?info.f_frsize:info.f_bsize);
    return true;
#else
    (void)directory;(void)out;return false;
#endif
}
static int on_progress(void* user,curl_off_t total,curl_off_t now,curl_off_t,curl_off_t){
    auto* t=static_cast<Transfer*>(user);
    if(t->cancel&&t->cancel->load()){t->abort_status=DownloadStatus::Cancelled;t->aborted=true;return 1;}
    if(t->pause&&t->pause->load()){t->abort_status=DownloadStatus::Paused;t->aborted=true;return 1;}
    if(total>0&&!t->space_checked){
        t->space_checked=true;uint64_t free_bytes=0;
        if(free_space(t->directory,free_bytes)&&free_bytes<static_cast<uint64_t>(total)+t->margin){
            t->abort_status=DownloadStatus::Space;t->aborted=true;return 1;
        }
    }
    if(t->progress&&*t->progress)(*t->progress)(t->resume+static_cast<uint64_t>(std::max<curl_off_t>(now,0)),total>0?t->resume+static_cast<uint64_t>(total):0);
    return 0;
}
static uint64_t file_size(const std::string& path){
    struct stat info{};
    return stat(path.c_str(),&info)==0&&S_ISREG(info.st_mode)?static_cast<uint64_t>(info.st_size):0;
}
}
std::string format_bytes(uint64_t bytes){
    char text[32];
    if(bytes>=(1ull<<30))std::snprintf(text,sizeof(text),"%.1f GB",static_cast<double>(bytes)/(1ull<<30));
    else if(bytes>=(1ull<<20))std::snprintf(text,sizeof(text),"%llu MB",static_cast<unsigned long long>(bytes>>20));
    else std::snprintf(text,sizeof(text),"%llu KB",static_cast<unsigned long long>(bytes>>10));
    return text;
}
std::string video_extension(const std::string& url){
    const auto scheme=url.find("://");
    const auto path_start=url.find('/',scheme==std::string::npos?0:scheme+3);
    if(path_start==std::string::npos)return "mp4";
    const auto path_end=url.find_first_of("?#",path_start);
    const std::string path=url.substr(path_start,path_end==std::string::npos?std::string::npos:path_end-path_start);
    const auto dot=path.rfind('.'),slash=path.rfind('/');
    if(dot==std::string::npos||(slash!=std::string::npos&&dot<slash))return "mp4";
    std::string ext=path.substr(dot+1);
    for(char& c:ext)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for(const char* known:{"mp4","m4v","mkv","mov","webm","ts","avi"})if(ext==known)return ext;
    return "mp4";
}
DownloadResult download_file(const DownloadRequest& request,const std::atomic<bool>& cancel,
                             const std::atomic<bool>& pause,const DownloadProgress& progress){
    DownloadResult out;
    const bool web=request.url.rfind("https://",0)==0||request.url.rfind("http://",0)==0;
    if(!web||!valid_source(request.url)||request.part_path.empty()||request.final_path.empty()){
        out.status=DownloadStatus::Invalid;out.message="Direccion de descarga no valida.";return out;
    }
    // Un .m3u8 se reproduce por segmentos; no hay un unico archivo que guardar.
    {const auto query=request.url.find_first_of("?#");
     const std::string path=request.url.substr(0,query);
     if(path.size()>5&&path.compare(path.size()-5,5,".m3u8")==0){
         out.status=DownloadStatus::Hls;out.message="Fuente HLS (.m3u8) no compatible con descarga.";return out;}}
    const auto cut=request.part_path.find_last_of('/');
    const std::string directory=cut==std::string::npos?std::string():request.part_path.substr(0,cut);
    for(int pass=0;pass<2;++pass){
        const uint64_t resume=file_size(request.part_path);
        Transfer t;t.resume=resume;t.cancel=&cancel;t.pause=&pause;t.progress=&progress;t.directory=directory;t.margin=request.free_margin;
        t.file=std::fopen(request.part_path.c_str(),resume?"ab":"wb");
        if(!t.file){out.status=DownloadStatus::Write;out.message="No se pudo crear el archivo en la SD.";return out;}
        std::vector<char> buffer(256*1024);std::setvbuf(t.file,buffer.data(),_IOFBF,buffer.size());
        CURL* curl=curl_easy_init();
        if(!curl){std::fclose(t.file);out.status=DownloadStatus::Network;out.message="No se pudo iniciar HTTP.";return out;}
        curl_slist* list=nullptr;
        for(const auto& header:request.headers){
            if(header.first.empty()||header.second.empty()||header.first.find_first_of("\r\n:")!=std::string::npos||
               header.second.find_first_of("\r\n")!=std::string::npos)continue;
            list=curl_slist_append(list,(header.first+": "+header.second).c_str());
        }
        curl_easy_setopt(curl,CURLOPT_URL,request.url.c_str());
        // Las cabeceras de autorizacion nunca se reenvian a otro servidor por una redireccion.
        curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,list?0L:1L);
        curl_easy_setopt(curl,CURLOPT_MAXREDIRS,5L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"http,https");
        curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS_STR,"http,https");
#else
        curl_easy_setopt(curl,CURLOPT_PROTOCOLS,CURLPROTO_HTTP|CURLPROTO_HTTPS);
        curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS,CURLPROTO_HTTP|CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
        if(!request.ca_file.empty()&&file_size(request.ca_file))curl_easy_setopt(curl,CURLOPT_CAINFO,request.ca_file.c_str());
        curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,15L);
        // Si la velocidad cae por debajo de 2 KB/s durante 30 s, se corta y se reanuda.
        curl_easy_setopt(curl,CURLOPT_LOW_SPEED_LIMIT,2048L);curl_easy_setopt(curl,CURLOPT_LOW_SPEED_TIME,30L);
        curl_easy_setopt(curl,CURLOPT_FAILONERROR,1L);
        curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(curl,CURLOPT_BUFFERSIZE,131072L);
        if(list)curl_easy_setopt(curl,CURLOPT_HTTPHEADER,list);
        if(resume)curl_easy_setopt(curl,CURLOPT_RESUME_FROM_LARGE,static_cast<curl_off_t>(resume));
        curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,on_data);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&t);
        curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,on_progress);curl_easy_setopt(curl,CURLOPT_XFERINFODATA,&t);
        const CURLcode rc=curl_easy_perform(curl);
        long http=0;curl_off_t length=-1;
        curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http);
        curl_easy_getinfo(curl,CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,&length);
        curl_slist_free_all(list);curl_easy_cleanup(curl);
        const bool flushed=std::fflush(t.file)==0&&!std::ferror(t.file);
        std::fclose(t.file);
        out.curl=static_cast<int>(rc);out.http=http;out.bytes=file_size(request.part_path);
        if(t.hls){std::remove(request.part_path.c_str());out.status=DownloadStatus::Hls;out.message="Fuente HLS (.m3u8) no compatible con descarga.";return out;}
        if(t.invalid){std::remove(request.part_path.c_str());out.status=DownloadStatus::Invalid;out.message="El servidor no devolvio un video.";out.retry=true;out.refresh_url=true;return out;}
        if(t.write_failed||!flushed){out.status=DownloadStatus::Write;out.message="Error al escribir en la SD (esta llena, o es FAT32 y el archivo supera 4 GB).";return out;}
        if(rc==CURLE_ABORTED_BY_CALLBACK&&t.aborted){
            out.status=t.abort_status;
            out.message=t.abort_status==DownloadStatus::Space?"No hay espacio suficiente en la SD.":
                        t.abort_status==DownloadStatus::Paused?"Pausada.":"Cancelada.";
            return out;
        }
        if(rc==CURLE_RANGE_ERROR&&resume&&pass==0){
            // El servidor no admite reanudar: se empieza de cero una sola vez.
            std::remove(request.part_path.c_str());continue;
        }
        if(rc==CURLE_HTTP_RETURNED_ERROR){
            out.status=DownloadStatus::Http;
            out.message="Error HTTP "+std::to_string(http)+".";
            out.refresh_url=http==401||http==403||http==404||http==410;
            out.retry=out.refresh_url||http>=500||http==429;
            return out;
        }
        if(rc!=CURLE_OK){
            out.status=DownloadStatus::Network;out.retry=true;
            out.message=std::string("Conexion: ")+curl_easy_strerror(rc);
            return out;
        }
        // 416 al reanudar: el archivo parcial ya estaba completo.
        const bool already=http==416&&resume>0;
        if(!already&&length>=0&&out.bytes!=resume+static_cast<uint64_t>(length)){
            out.status=DownloadStatus::Network;out.retry=true;out.message="La descarga quedo incompleta.";return out;
        }
        if(out.bytes==0){out.status=DownloadStatus::Invalid;out.message="El servidor devolvio un archivo vacio.";out.retry=true;out.refresh_url=true;return out;}
        // FAT no siempre deja renombrar sobre un destino existente.
        std::remove(request.final_path.c_str());
        if(std::rename(request.part_path.c_str(),request.final_path.c_str())!=0){
            out.status=DownloadStatus::Write;out.message="No se pudo instalar el archivo descargado.";return out;
        }
        out.status=DownloadStatus::Ok;out.message="Completa.";
        if(progress)progress(out.bytes,out.bytes);
        return out;
    }
    out.status=DownloadStatus::Network;out.retry=true;out.message="No se pudo reanudar la descarga.";return out;
}
}
