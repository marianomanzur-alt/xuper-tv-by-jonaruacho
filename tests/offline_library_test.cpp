#include "offline_library.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
namespace fs=std::filesystem;
static void touch(const std::string& path,const std::string& data="x"){std::ofstream f(path,std::ios::binary);f<<data;}
int main(){
    const auto root=fs::temp_directory_path()/("xs-lib-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root/"descargas");
    const std::string dir=(root/"descargas").string(),path=(root/"lib.json").string();
    auto item=[&](const std::string& id,const std::string& series,int order,long long saved){
        xs::OfflineItem i;i.content_id=id;i.series_id=series;i.program_type=series.empty()?"1":"0";i.title="T "+id;i.file=dir+"/"+id+".mp4";i.size=3<<20;i.order=order;i.saved=saved;
        i.art=xs::artwork_key(id);i.subtitles={{"Espanol",dir+"/"+id+".1.srt"},{"English",dir+"/"+id+".2.srt"}};
        touch(i.file);for(const auto& s:i.subtitles)touch(s.file);return i;};
    {
        xs::OfflineLibrary lib(path,dir);
        assert(lib.add(item("b2","s1",2,100))&&lib.add(item("b1","s1",1,100))&&lib.add(item("m","",0,500))&&lib.add(item("c1","s2",1,300)));
        const auto e=lib.entries();
        // grupo mas reciente primero (pelicula 500, serie s2 300, serie s1 100), episodios en orden
        assert(e.size()==4&&e[0].content_id=="m"&&e[1].content_id=="c1"&&e[2].content_id=="b1"&&e[3].content_id=="b2");
        assert(e[0].kind==xs::EntryKind::Offline&&e[0].source==dir+"/m.mp4"&&e[0].artwork_cache==xs::artwork_key("m"));
        // sustituir uno existente no lo duplica
        auto again=item("b1","s1",1,900);again.size=7;assert(lib.add(again));assert(lib.items.size()==4&&lib.find("b1")->size==7);
        // rutas fuera de la carpeta o con ".." se rechazan
        auto bad=item("z","s1",1,1);bad.file=(root/"otra.mp4").string();assert(!lib.add(bad));
        bad.file=dir+"/../escape.mp4";assert(!lib.add(bad));
        bad.file="";assert(!lib.add(bad));
    }
    {   // persistencia y privacidad
        xs::OfflineLibrary lib(path,dir);assert(lib.items.size()==4&&lib.find("m")&&lib.find("c1")->subtitles.size()==2);
        std::ifstream f(path);const std::string data((std::istreambuf_iterator<char>(f)),{});
        assert(data.find("http")==std::string::npos&&data.find("token")==std::string::npos);
        // un JSON alterado a mano no puede apuntar fuera de la carpeta
        auto j=nlohmann::json::parse(data);j["items"][0]["file"]="/etc/passwd";j["items"][1]["subs"][0][1]="../../secreto.srt";
        std::ofstream o(path,std::ios::trunc);o<<j.dump();o.close();
        xs::OfflineLibrary hostile(path,dir);assert(hostile.items.size()==3);
        for(const auto& i:hostile.items)for(const auto& s:i.subtitles)assert(s.file.find("..")==std::string::npos);
    }
    {   // recuperacion desde .bak y archivos borrados a mano
        xs::OfflineLibrary lib(path,dir);assert(lib.add(item("n","",0,1)));
        fs::rename(path,path+".bak");xs::OfflineLibrary restored(path,dir);assert(restored.find("n"));
        fs::remove(dir+"/n.mp4");assert(restored.prune_missing()&&!restored.find("n"));
        assert(xs::OfflineLibrary(path,dir).find("n")==nullptr);
    }
    {   // borrar quita video y subtitulos
        xs::OfflineLibrary lib(path,dir);const auto gone=lib.items.back();assert(!gone.subtitles.empty());
        assert(fs::exists(gone.file)&&fs::exists(gone.subtitles[0].file));
        assert(lib.remove(gone.content_id)&&!fs::exists(gone.file)&&!fs::exists(gone.subtitles[0].file)&&!lib.find(gone.content_id)&&!lib.remove(gone.content_id));
    }
    {   // escritura fallida no corrompe lo existente
        xs::OfflineLibrary lib(path,dir);const auto count=lib.items.size();
        fs::create_directory(path+".tmp");assert(!lib.add(item("w","",0,1)));assert(lib.items.size()==count);fs::remove(path+".tmp");
        assert(xs::OfflineLibrary(path,dir).items.size()==count);
    }
    {   // subtitulo por defecto segun idioma recordado
        std::vector<xs::OfflineSubtitle> s{{"English","a"},{"Español (Latinoamérica)","b"},{"Portugues","c"}};
        assert(xs::default_subtitle(s,"spa")==1&&xs::default_subtitle(s,"es")==1&&xs::default_subtitle(s,"eng")==0&&xs::default_subtitle(s,"por")==2);
        assert(xs::default_subtitle(s,"none")==-1&&xs::default_subtitle(s,"")==-1&&xs::default_subtitle(s,"jpn")==-1&&xs::default_subtitle({},"spa")==-1);
    }
    fs::remove_all(root);
    std::cout<<"offline library: order, replace, path safety, hostile JSON, backup recovery, prune, delete, failed write and subtitle choice passed\n";
}
