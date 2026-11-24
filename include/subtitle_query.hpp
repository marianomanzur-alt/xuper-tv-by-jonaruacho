#pragma once
// Que buscar al pedir subtitulos para un video: nombre de la serie/pelicula, temporada y episodio.
#include "catalog.hpp"
#include "subtitle_search.hpp"
namespace xs {
inline SubtitleQuery subtitle_query_for(const Entry& entry,const std::string& list_title){
    SubtitleQuery own=guess_subtitle_query(entry.title);
    if(entry.kind==EntryKind::Episode){
        // El nombre de la serie viaja con el episodio; si falta (favoritos viejos) se usa el de la lista abierta.
        std::string name=entry.show;
        const bool generic=list_title.empty()||list_title=="Continuar viendo"||list_title=="Descargas"||list_title.rfind("Resultados",0)==0;
        if(name.empty()&&!generic)name=list_title;
        if(!name.empty()){
            SubtitleQuery show=guess_subtitle_query(name);
            if(!show.title.empty()){own.title=show.title;if(!own.season)own.season=show.season;if(!own.year)own.year=show.year;
                if(entry.series_id.empty()){own.season=0;own.episode=0;}}   // pelicula: sin temporada ni episodio
        }
    }
    if(own.title.empty()){   // "Episodio 3 - Titulo" sin serie conocida: el titulo del episodio es lo unico que hay
        const size_t cut=entry.title.rfind(" - ");
        SubtitleQuery rest=guess_subtitle_query(cut==std::string::npos?entry.title:entry.title.substr(cut+3));
        own.title=rest.title;if(!own.year)own.year=rest.year;
    }
    return own;
}
}
