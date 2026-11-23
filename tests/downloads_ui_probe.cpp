// Dibuja las pantallas de descargas con SDL real (driver "dummy") y guarda capturas BMP.
#include "view.hpp"
#include <cassert>
#include <iostream>
static void snapshot(xs::View& view,const char* file){auto* s=SDL_CreateRGBSurfaceWithFormat(0,1280,720,32,SDL_PIXELFORMAT_RGBA32);assert(s);assert(SDL_RenderReadPixels(view.renderer,nullptr,s->format->format,s->pixels,s->pitch)==0);assert(SDL_SaveBMP(s,file)==0);SDL_FreeSurface(s);}
int main(){
    xs::View view;std::string error;assert(view.open(error));
    view.badge=[](const xs::Entry& e){
        if(e.content_id=="1")return std::string("DESCARGADO 1.4 GB");
        if(e.content_id=="2")return std::string("DESCARGANDO 37%");
        if(e.content_id=="3")return std::string("EN COLA");
        if(e.content_id=="4")return std::string("ERROR: El servidor respondio HTTP 404.");
        return std::string();};
    xs::Catalog home;home.title="Inicio";
    for(auto pair:{std::pair<const char*,xs::EntryKind>{"TV en vivo",xs::EntryKind::LiveSection},{"Peliculas",xs::EntryKind::Section},{"Series",xs::EntryKind::Section},{"Continuar viendo",xs::EntryKind::Resume},{"Favoritos",xs::EntryKind::Favorites},{"Descargas",xs::EntryKind::Downloads},{"Apoyar el desarrollo",xs::EntryKind::Donation}}){xs::Entry e;e.title=pair.first;e.kind=pair.second;home.entries.push_back(e);}
    view.draw(home,5,"Elige que quieres ver.",true);snapshot(view,"dl_home.bmp");
    // sidebar: siete filas, la de Descargas es la sexta (-7) y Apoyar la septima (-8)
    assert(view.hit(home,0,40,150)==-2&&view.hit(home,0,40,150+5*64)==-7&&view.hit(home,0,40,150+6*64)==-8&&view.hit(home,0,40,150+7*64)==-1);
    xs::Catalog episodes;episodes.title="Horizonte";episodes.description="Una aventura para descubrir nuevos mundos.";
    for(int i=0;i<6;++i){xs::Entry e;e.kind=xs::EntryKind::Episode;e.content_id=std::to_string(i+1);e.title="Episodio "+std::to_string(i+1)+" - Un nuevo comienzo";episodes.entries.push_back(e);}
    view.overlay="Descargando 2/3  37%  512 MB / 1.4 GB";
    view.draw(episodes,1,"En cola para descargar: 3 videos. El avance se ve arriba; la lista esta en Descargas.",true);snapshot(view,"dl_episodes.bmp");
    xs::Catalog list;list.title="Descargas";view.active_nav=5;
    for(int i=0;i<5;++i){xs::Entry e;e.kind=xs::EntryKind::Offline;e.content_id=std::to_string(i+1);e.title=i==0?"Una Pelicula":"Horizonte - Episodio "+std::to_string(i)+" - Un nuevo comienzo";e.source=i<1?"/x":"";list.entries.push_back(e);}
    view.draw(list,0,"Descargas. A: reproducir sin conexion; ZR: borrar o cancelar; B: volver.",true);snapshot(view,"dl_list.bmp");
    xs::Catalog empty;empty.title="Descargas";view.overlay.clear();view.draw(empty,0,"Aun no hay descargas.",true);snapshot(view,"dl_empty.bmp");
    std::cout<<"UI descargas: inicio, episodios con estado, lista, vacio y regiones tactiles del menu lateral\n";
}
