#pragma once
// Biblioteca de contenido guardado en la SD para verlo sin conexion.
// Solo guarda titulo, identificadores y rutas de archivos locales; nunca URL de video,
// cabeceras, tokens ni datos de cuenta.
#include "catalog.hpp"
#define JSON_NOEXCEPTION 1
#include "json.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>
namespace xs {
struct OfflineSubtitle {std::string label,file;};
struct OfflineItem {
    std::string content_id,series_id,program_type,title,file,art;
    uint64_t size=0;
    int order=0;            // posicion dentro de la serie (para ordenar episodios)
    long long saved=0;      // instante de la descarga (segundos Unix)
    std::vector<OfflineSubtitle> subtitles;
};
// Elige que subtitulo descargado cargar al abrir un video: el que coincide con el idioma
// que el usuario eligio la ultima vez (codigo mpv como "spa" o "eng"). Devuelve el indice
// o -1 si no hay coincidencia clara; "none" significa que el usuario los desactivo.
inline int default_subtitle(const std::vector<OfflineSubtitle>& subtitles,const std::string& preferred){
    if(subtitles.empty()||preferred.empty()||preferred=="none")return -1;
    auto lower=[](std::string v){for(char& c:v)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return v;};
    const std::string want=lower(preferred);
    std::vector<std::string> words{want};
    const std::string head=want.substr(0,2);
    if(head=="sp"||head=="es")words.insert(words.end(),{"espa","spa","cast","latin"});
    else if(head=="en")words.insert(words.end(),{"engl","ingl","eng"});
    else if(head=="po"||head=="pt")words.insert(words.end(),{"port","por"});
    else if(head=="fr")words.insert(words.end(),{"fran","fre","fra"});
    else if(head=="it")words.insert(words.end(),{"ital"});
    else if(head=="de"||head=="ge")words.insert(words.end(),{"deut","germ","alem"});
    for(size_t i=0;i<subtitles.size();++i){
        const std::string label=lower(subtitles[i].label);
        for(const auto& w:words)if(w.size()>=3&&label.find(w)!=std::string::npos)return static_cast<int>(i);
    }
    return -1;
}
class OfflineLibrary {
    using Json=nlohmann::json;
    std::string path,directory;
    static std::string str(const Json& j,const char* key,size_t max=512){
        const auto i=j.find(key);
        return i!=j.end()&&i->is_string()&&i->get_ref<const std::string&>().size()<=max?i->get<std::string>():"";
    }
    static bool safe(const std::string& v){
        return !v.empty()&&v.size()<=512&&v.find("..")==std::string::npos&&
               std::none_of(v.begin(),v.end(),[](unsigned char c){return c<32||c==127;});
    }
    bool inside(const std::string& file)const{
        return safe(file)&&file.size()>directory.size()+1&&file.compare(0,directory.size(),directory)==0&&file[directory.size()]=='/';
    }
    static Json read(const std::string& filename){
        std::ifstream f(filename,std::ios::binary|std::ios::ate);const auto n=f.tellg();
        if(n<0||n>4*1024*1024)return Json();
        f.seekg(0);std::string s(static_cast<size_t>(n),'\0');f.read(s.data(),n);
        return f?Json::parse(s,nullptr,false):Json();
    }
    static bool exists(const std::string& file){std::ifstream f(file,std::ios::binary);return f.good();}
public:
    std::vector<OfflineItem> items;
    // `dir` es la carpeta de descargas: solo se aceptan archivos que esten dentro de ella.
    OfflineLibrary(std::string filename,std::string dir):path(std::move(filename)),directory(std::move(dir)){
        while(directory.size()>1&&directory.back()=='/')directory.pop_back();
        Json j=read(path);if(!j.is_object())j=read(path+".bak");
        if(!j.is_object())return;
        const auto list=j.find("items");if(list==j.end()||!list->is_array())return;
        for(const auto& v:*list){
            if(!v.is_object())continue;
            OfflineItem item;item.content_id=str(v,"id");item.series_id=str(v,"series");item.program_type=str(v,"type",8);
            item.title=str(v,"title",300);item.file=str(v,"file");item.art=str(v,"art",16);
            if(item.art.find_first_not_of("0123456789abcdef")!=std::string::npos)item.art.clear();
            if(item.content_id.empty()||item.title.empty()||!inside(item.file))continue;
            const auto size=v.find("size");if(size!=v.end()&&size->is_number_unsigned())item.size=size->get<uint64_t>();
            const auto order=v.find("order");if(order!=v.end()&&order->is_number_integer()&&order->get<int64_t>()>=0&&order->get<int64_t>()<=1000000)item.order=order->get<int>();
            const auto saved=v.find("saved");if(saved!=v.end()&&saved->is_number_integer())item.saved=saved->get<long long>();
            const auto subs=v.find("subs");
            if(subs!=v.end()&&subs->is_array())for(const auto& s:*subs){
                if(!s.is_array()||s.size()<2||!s[0].is_string()||!s[1].is_string())continue;
                OfflineSubtitle sub{s[0].get<std::string>(),s[1].get<std::string>()};
                if(sub.label.size()<=80&&inside(sub.file)&&item.subtitles.size()<32)item.subtitles.push_back(std::move(sub));
            }
            items.push_back(std::move(item));if(items.size()>=5000)break;
        }
    }
    const std::string& dir()const{return directory;}
    const OfflineItem* find(const std::string& content_id)const{
        const auto i=std::find_if(items.begin(),items.end(),[&](const OfflineItem& x){return x.content_id==content_id;});
        return i==items.end()?nullptr:&*i;
    }
    // Descarta los elementos cuyo video ya no esta en la SD (por ejemplo, borrado a mano).
    bool prune_missing(){
        const auto before=items.size();
        items.erase(std::remove_if(items.begin(),items.end(),[&](const OfflineItem& x){return !exists(x.file);}),items.end());
        return items.size()!=before&&save();
    }
    bool add(OfflineItem item){
        if(item.content_id.empty()||!inside(item.file))return false;
        const auto previous=items;
        items.erase(std::remove_if(items.begin(),items.end(),[&](const OfflineItem& x){return x.content_id==item.content_id;}),items.end());
        items.push_back(std::move(item));
        if(save())return true;
        items=previous;return false;
    }
    // Borra los archivos del contenido y lo quita de la biblioteca.
    bool remove(const std::string& content_id){
        const auto i=std::find_if(items.begin(),items.end(),[&](const OfflineItem& x){return x.content_id==content_id;});
        if(i==items.end())return false;
        const OfflineItem gone=*i;
        std::remove(gone.file.c_str());
        for(const auto& sub:gone.subtitles)std::remove(sub.file.c_str());
        items.erase(i);return save();
    }
    uint64_t total_size()const{uint64_t n=0;for(const auto& x:items)n+=x.size;return n;}
    // Lista para la interfaz: series agrupadas (la mas reciente primero), episodios en orden.
    std::vector<Entry> entries()const{
        std::map<std::string,long long> newest;
        auto group=[](const OfflineItem& x){return x.series_id.empty()?x.content_id:x.series_id;};
        for(const auto& x:items){auto& t=newest[group(x)];t=std::max(t,x.saved);}
        std::vector<const OfflineItem*> order;for(const auto& x:items)order.push_back(&x);
        std::stable_sort(order.begin(),order.end(),[&](const OfflineItem* a,const OfflineItem* b){
            const auto ga=group(*a),gb=group(*b);
            if(ga!=gb){const auto ta=newest[ga],tb=newest[gb];return ta!=tb?ta>tb:ga<gb;}
            return a->order<b->order;
        });
        std::vector<Entry> out;
        for(const auto* x:order){
            Entry e;e.kind=EntryKind::Offline;e.title=x->title;e.source=x->file;e.content_id=x->content_id;
            e.series_id=x->series_id;e.program_type=x->program_type;e.artwork_cache=x->art;
            e.description=format_size(x->size);
            out.push_back(std::move(e));
        }
        return out;
    }
    static std::string format_size(uint64_t bytes){
        char text[32];
        if(bytes>=(1ull<<30))std::snprintf(text,sizeof(text),"%.1f GB",static_cast<double>(bytes)/(1ull<<30));
        else if(bytes>=(1ull<<20))std::snprintf(text,sizeof(text),"%llu MB",static_cast<unsigned long long>(bytes>>20));
        else std::snprintf(text,sizeof(text),"%llu KB",static_cast<unsigned long long>(bytes>>10));
        return text;
    }
    bool save()const{
        Json list=Json::array();
        for(const auto& x:items){
            Json subs=Json::array();for(const auto& s:x.subtitles)subs.push_back(Json::array({s.label,s.file}));
            list.push_back({{"id",x.content_id},{"series",x.series_id},{"type",x.program_type},{"title",x.title},{"file",x.file},
                            {"art",x.art},{"size",x.size},{"order",x.order},{"saved",x.saved},{"subs",subs}});
        }
        const std::string tmp=path+".tmp",bak=path+".bak";
        {std::ofstream f(tmp,std::ios::binary|std::ios::trunc);f<<Json({{"items",list}}).dump();f.close();
            if(!f.good()){std::remove(tmp.c_str());return false;}}
        // FAT no siempre permite renombrar sobre un destino existente: se conserva la copia anterior.
        std::ifstream old(path);const bool had=old.good();old.close();
        if(had){
            if(std::remove(bak.c_str())!=0&&errno!=ENOENT)return false;
            if(std::rename(path.c_str(),bak.c_str())!=0)return false;
        }
        if(std::rename(tmp.c_str(),path.c_str())!=0){if(had)std::rename(bak.c_str(),path.c_str());return false;}
        std::remove(bak.c_str());return true;
    }
};
}
