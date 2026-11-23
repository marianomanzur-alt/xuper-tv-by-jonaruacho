#pragma once
// Busqueda de subtitulos en internet con varias fuentes y respaldo:
// se consulta cada fuente en orden y se usa la primera que devuelva resultados.
#include <functional>
#include <string>
#include <vector>
namespace xs {
struct SubtitleQuery {
    std::string title;         // titulo limpio (sin "S01E02", anio ni corchetes)
    int season=0,episode=0;    // 0/0: pelicula
    int year=0;
};
struct SubtitleHit {
    std::string provider;      // "OpenSubtitles" | "SubDL"
    std::string language;      // codigo de 2 letras en minusculas ("es")
    std::string label;         // texto para el menu
    std::string id;            // file_id (OpenSubtitles) o ruta del zip (SubDL)
    int downloads=0;
    int season=0,episode=0;    // 0 si la fuente no lo informa
};
struct SubtitleSearchConfig {
    std::vector<std::string> languages{"es","en"};   // por orden de preferencia
    std::string opensubtitles_key,opensubtitles_user,opensubtitles_password;
    std::string subdl_key;
    std::string ca_file;
    // Direcciones base; solo se cambian en pruebas contra un servidor local.
    std::string opensubtitles_base="https://api.opensubtitles.com/api/v1";
    std::string subdl_base="https://api.subdl.com/api/v1";
    std::string subdl_download_base="https://dl.subdl.com";
    bool allow_http=false;     // solo pruebas locales
    bool any_provider()const{return !opensubtitles_key.empty()||!subdl_key.empty();}
};
// Lee subtitulos.json (claves de API). Devuelve false con mensaje si el archivo existe pero esta mal.
bool load_subtitle_config(const std::string& path,SubtitleSearchConfig& config,std::string& error);
// Extrae titulo/temporada/episodio de un nombre como "Serie S02E05 - Titulo (2019)" o "Serie 2x05".
SubtitleQuery guess_subtitle_query(const std::string& title);
// Extrae el primer .srt/.ass/.vtt de un zip (stored o deflate). Limite 2 MB descomprimido.
bool extract_subtitle_zip(const std::string& zip,std::string& data,std::string& error);

class SubtitleSearch {
public:
    explicit SubtitleSearch(SubtitleSearchConfig config):config_(std::move(config)){}
    // Consulta las fuentes en orden; se queda con la primera que tenga resultados.
    bool search(const SubtitleQuery& query,std::vector<SubtitleHit>& hits,std::string& error);
    bool fetch(const SubtitleHit& hit,std::string& data,std::string& error);
    const std::string& used_provider()const{return used_;}
    // Textos de cada fuente que fallo (sin claves ni URLs) para mostrar si no hay resultados.
    const std::vector<std::string>& provider_notes()const{return notes_;}
private:
    bool search_opensubtitles(const SubtitleQuery&,std::vector<SubtitleHit>&,std::string&);
    bool search_subdl(const SubtitleQuery&,std::vector<SubtitleHit>&,std::string&);
    bool login(std::string& error);
    SubtitleSearchConfig config_;
    std::string token_,used_;
    std::vector<std::string> notes_;
};
}
