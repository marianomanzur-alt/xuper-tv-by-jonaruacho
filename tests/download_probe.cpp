// Sonda de pruebas: ejecuta download_file contra un servidor local y escribe el resultado como JSON.
// Uso: download_probe URL PARTE FINAL [--cancel-at BYTES] [--pause-at BYTES] [--header Nombre:Valor] [--margin BYTES]
#include "downloader.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <thread>
int main(int argc,char** argv){
    if(argc<4)return 2;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    xs::DownloadRequest request;request.url=argv[1];request.part_path=argv[2];request.final_path=argv[3];request.free_margin=0;
    uint64_t cancel_at=0,pause_at=0;
    for(int i=4;i<argc;++i){
        if(!std::strcmp(argv[i],"--cancel-at")&&i+1<argc)cancel_at=std::strtoull(argv[++i],nullptr,10);
        else if(!std::strcmp(argv[i],"--pause-at")&&i+1<argc)pause_at=std::strtoull(argv[++i],nullptr,10);
        else if(!std::strcmp(argv[i],"--margin")&&i+1<argc)request.free_margin=std::strtoull(argv[++i],nullptr,10);
        else if(!std::strcmp(argv[i],"--header")&&i+1<argc){std::string h=argv[++i];const auto c=h.find(':');
            if(c!=std::string::npos)request.headers.push_back({h.substr(0,c),h.substr(c+1)});}
    }
    std::atomic<bool> cancel{false},pause{false};
    uint64_t last_done=0,last_total=0;
    const auto result=xs::download_file(request,cancel,pause,[&](uint64_t done,uint64_t total){
        last_done=done;last_total=total;
        if(cancel_at&&done>=cancel_at)cancel=true;
        if(pause_at&&done>=pause_at)pause=true;
    });
    std::printf("{\"status\":%d,\"http\":%ld,\"curl\":%d,\"bytes\":%llu,\"retry\":%d,\"refresh\":%d,\"progress_done\":%llu,\"progress_total\":%llu,\"message\":\"%s\"}\n",
        static_cast<int>(result.status),result.http,result.curl,static_cast<unsigned long long>(result.bytes),result.retry,result.refresh_url,
        static_cast<unsigned long long>(last_done),static_cast<unsigned long long>(last_total),result.message.c_str());
    curl_global_cleanup();return 0;
}
