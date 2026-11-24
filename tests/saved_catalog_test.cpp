#include "saved_catalog.hpp"
#include <filesystem>
#include <cassert>
#include <chrono>
#include <iostream>
int main(){
    const auto dir=std::filesystem::temp_directory_path()/("xs-saved-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directory(dir);const auto path=(dir/"state.json").string();
    xs::SavedCatalog saved(path);xs::Entry channel;channel.kind=xs::EntryKind::Channel;channel.title="Noticias";channel.channel_code="c1";channel.column_id=20;channel.source="https://secret.invalid/video?token=private";channel.artwork="https://image.invalid/a?secret=test";channel.artwork_cache=xs::artwork_key(channel.artwork);
    assert(saved.toggle(channel)&&saved.contains(channel));assert(xs::SavedCatalog(path).favorites.size()==1);
    {   // series y peliculas (fichas del catalogo) tambien se guardan como favoritos, sin URLs
        xs::Entry serie;serie.kind=xs::EntryKind::Asset;serie.title="Mi serie";serie.content_id="s100";serie.program_type="0";serie.column_id=7;serie.artwork="https://image.invalid/p?secret=1";serie.artwork_cache=xs::artwork_key(serie.artwork);serie.source="https://secret.invalid/x?token=1";
        xs::Entry peli=serie;peli.title="Mi peli";peli.content_id="m200";peli.program_type="1";
        assert(xs::SavedCatalog::can_favorite(serie)&&xs::SavedCatalog::can_favorite(peli)&&xs::SavedCatalog::can_favorite(channel));
        xs::Entry bad=serie;bad.program_type="9";assert(!xs::SavedCatalog::can_favorite(bad)&&!saved.toggle(bad));
        bad=serie;bad.content_id.clear();assert(!xs::SavedCatalog::can_favorite(bad));
        xs::Entry other;other.kind=xs::EntryKind::Category;other.title="x";assert(!xs::SavedCatalog::can_favorite(other));
        assert(saved.toggle(serie)&&saved.toggle(peli)&&saved.contains(serie)&&saved.contains(peli));
        {xs::SavedCatalog again(path);assert(again.favorites.size()==3);
         bool found=false;for(const auto& f:again.favorites)if(f.kind==xs::EntryKind::Asset&&f.content_id=="s100"){found=true;assert(f.program_type=="0"&&f.column_id==7&&f.title=="Mi serie"&&f.artwork_cache==serie.artwork_cache&&f.source.empty()&&f.artwork.empty());}
         assert(found&&again.contains(serie)&&again.contains(peli));}
        {std::ifstream f2(path);const std::string raw((std::istreambuf_iterator<char>(f2)),{});assert(raw.find("secret")==std::string::npos&&raw.find("token")==std::string::npos);}
        assert(saved.toggle(serie)&&!saved.contains(serie)&&saved.contains(peli));assert(xs::SavedCatalog(path).favorites.size()==2);
        assert(saved.toggle(peli));assert(xs::SavedCatalog(path).favorites.size()==1);
        {std::ofstream f3(path,std::ios::trunc);f3<<R"({"favorites":[{"kind":"asset","title":"x","content":"a1"},{"kind":"asset","title":"ok","content":"a2","type":"1"}]})";}
        xs::SavedCatalog hostile(path);assert(hostile.favorites.size()==1&&hostile.favorites[0].content_id=="a2");   // sin type valido se descarta
        {std::ofstream f4(path,std::ios::trunc);}saved=xs::SavedCatalog(path);assert(saved.toggle(channel));
    }
    std::ifstream file(path);const std::string data((std::istreambuf_iterator<char>(file)),{});assert(data.find("secret")==std::string::npos&&data.find("token")==std::string::npos&&data.find("http")==std::string::npos);file.close();
    assert(saved.toggle(channel)&&!saved.contains(channel));assert(xs::SavedCatalog(path).favorites.empty());
    assert(saved.toggle(channel));std::filesystem::rename(path,path+".bak");xs::SavedCatalog restored(path);assert(restored.contains(channel));assert(restored.save());
    xs::Entry episode;episode.kind=xs::EntryKind::Episode;episode.title="Episodio 1";episode.content_id="e1";episode.series_id="s1";episode.program_type="0";assert(restored.remember(episode)&&restored.remember(episode));assert(xs::SavedCatalog(path).recent.size()==1);
    std::filesystem::create_directory(path+".tmp");assert(!restored.toggle(channel));assert(restored.contains(channel)&&xs::SavedCatalog(path).contains(channel));
    assert(xs::search_key("PrUeBa Ñ") == xs::search_key("prueba n"));assert(xs::search_key("Café HD")=="cafehd");
    std::filesystem::remove_all(dir);std::cout<<"favorites: persistence, remove, backup recovery, failed-write rollback, privacy, recents dedup and accent matching passed\n";
}
