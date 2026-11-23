#pragma once
// Explorador minimo de carpetas/archivos de subtitulos (tarjeta SD de la Switch).
// Solo cabecera, sin std::filesystem ni excepciones (compatible con -fno-exceptions).
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>
namespace xs {
struct BrowseItem {std::string name;bool dir=false;};

inline std::string lower_ascii(std::string value){
    for(auto& c:value)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
// Extensiones que libmpv sabe leer como subtitulos externos.
inline bool is_subtitle_name(const std::string& name){
    const size_t dot=name.rfind('.');
    if(dot==std::string::npos||dot+1>=name.size())return false;
    const std::string ext=lower_ascii(name.substr(dot+1));
    return ext=="srt"||ext=="ass"||ext=="ssa"||ext=="vtt"||ext=="sub";
}
inline std::string join_path(const std::string& dir,const std::string& name){
    if(dir.empty())return name;
    return dir.back()=='/'?dir+name:dir+"/"+name;
}
// Sube un nivel sin salir nunca de `root` (por ejemplo "sdmc:/").
inline std::string parent_path(const std::string& path,const std::string& root){
    std::string p=path;
    while(p.size()>root.size()&&p.back()=='/')p.pop_back();
    if(p.size()<=root.size())return root;
    const size_t cut=p.rfind('/');
    if(cut==std::string::npos)return root;
    std::string up=p.substr(0,cut);
    return up.size()<root.size()?root:up;
}
// Recorta a `max` bytes sin partir un caracter UTF-8.
inline std::string clip_utf8(std::string text,size_t max){
    if(text.size()<=max)return text;
    size_t n=max;
    while(n>0&&(static_cast<unsigned char>(text[n])&0xC0)==0x80)--n;
    text.resize(n);
    return text+"...";
}
// Carpetas primero, luego archivos de subtitulos; orden alfabetico sin distinguir mayusculas.
// Oculta entradas que empiezan por '.'. `ok` indica si la carpeta se pudo abrir.
inline std::vector<BrowseItem> list_subtitle_dir(const std::string& path,bool* ok=nullptr,size_t limit=2000){
    std::vector<BrowseItem> items;
    DIR* dir=opendir(path.c_str());
    if(ok)*ok=dir!=nullptr;
    if(!dir)return items;
    while(const dirent* entry=readdir(dir)){
        const std::string name=entry->d_name;
        if(name.empty()||name[0]=='.')continue;
        bool is_dir=entry->d_type==DT_DIR;
        if(entry->d_type==DT_UNKNOWN||entry->d_type==DT_LNK){
            struct stat info{};
            if(stat(join_path(path,name).c_str(),&info)==0)is_dir=S_ISDIR(info.st_mode);
        }
        if(!is_dir&&!is_subtitle_name(name))continue;
        items.push_back({name,is_dir});
        if(items.size()>=limit)break;
    }
    closedir(dir);
    std::sort(items.begin(),items.end(),[](const BrowseItem& a,const BrowseItem& b){
        if(a.dir!=b.dir)return a.dir;
        return lower_ascii(a.name)<lower_ascii(b.name);
    });
    return items;
}
}
