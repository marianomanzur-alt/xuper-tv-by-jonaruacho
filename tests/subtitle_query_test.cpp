#include "subtitle_query.hpp"
#include <cassert>
#include <iostream>
using namespace xs;
static Entry ep(const std::string& title,const std::string& show,const std::string& series){Entry e;e.kind=EntryKind::Episode;e.title=title;e.show=show;e.series_id=series;e.content_id="c";return e;}
int main(){
    {auto q=subtitle_query_for(ep("Episodio 3 - El comienzo","Mi Serie","s1"),"Continuar viendo");
     assert(q.title=="Mi Serie"&&q.episode==3&&q.season==0);}                       // el caso que fallaba: lista generica
    {auto q=subtitle_query_for(ep("Episodio 3 - El comienzo","Mi Serie Temporada 2","s1"),"");assert(q.title=="Mi Serie"&&q.season==2&&q.episode==3);}
    {auto q=subtitle_query_for(ep("Episodio 3 - El comienzo","","s1"),"Otra Serie (2019)");assert(q.title=="Otra Serie"&&q.episode==3&&q.year==2019);}  // sin show: nombre de la lista
    {auto q=subtitle_query_for(ep("Reproducir: Mi Peli","Mi Peli (2020)",""),"Resultados: peli");assert(q.title=="Mi Peli"&&q.episode==0&&q.season==0&&q.year==2020);}
    {auto q=subtitle_query_for(ep("Reproducir: Mi Peli","",""),"Resultados: peli");assert(q.title=="Mi Peli");}
    {auto q=subtitle_query_for(ep("Episodio 3 - El comienzo","","s1"),"Continuar viendo");assert(!q.title.empty()&&q.title=="El comienzo"&&q.episode==3);}  // sin ningun dato de serie: nunca queda vacio
    {Entry off;off.kind=EntryKind::Offline;off.title="Mi Serie - Episodio 5 - Final";auto q=subtitle_query_for(off,"Descargas");assert(q.title=="Mi Serie"&&q.episode==5);}
    {auto q=subtitle_query_for(ep("Episodio 12","Mi Serie","s1"),"");assert(q.title=="Mi Serie"&&q.episode==12);}
    std::cout<<"subtitle query passed\n";
}
