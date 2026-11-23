#pragma once
#include <string>
#include <vector>
#include <utility>
#include <SDL.h>
#include <SDL_ttf.h>
#include <functional>
namespace xs {
struct PlaybackOptions {
    TTF_Font* font=nullptr;
    std::string title;
    std::vector<std::string> variants,items;
    std::vector<std::string> subtitles;
    std::function<bool(size_t,std::string&,std::string&)> subtitle_loader;
    int variant=0,item=0;
    double start=0;
    double speed=1,volume=100;
    bool auto_next=true;
    bool paused=false;
    // Preferencias recordadas (ver PlaybackHistory) y datos para precargar subtitulos.
    double sub_scale=1;
    int quality=0;                 // 0=640x360, 1=854x480, 2=960x540
    int buffer_mb=32;              // buffer de red de libmpv
    std::string alang,slang;       // idiomas preferidos (codigo mpv, p. ej. "spa")
    std::string subtitle_file;     // archivo .srt/.ass a cargar al abrir el video
    std::string subtitle_hint;     // ultimo archivo usado en la serie: sugerencia en el explorador
};
struct PlaybackOutcome {
    int next_item=-1,next_variant=-1;
    double position=0,speed=1,volume=100;
    double audio_delay=0,sub_delay=0,sub_scale=1,aspect=-1;
    int64_t audio=-1,subtitle=-1;
    bool ended=false,paused=false;
    bool auto_next=true;
    int quality=0;
    std::string alang,slang;       // idioma de las pistas internas elegidas (vacio: sin cambios)
    std::string subtitle_file;     // archivo externo activo al salir (vacio: ninguno)
};
// Playback uses libmpv's CPU render API and the persistent SDL2 renderer.
// Returns a diagnostic, or an empty string on normal exit.
std::string play(const std::string& source,SDL_Renderer* renderer,const std::vector<std::pair<std::string,std::string>>& headers={},const PlaybackOptions& options={},PlaybackOutcome* outcome=nullptr);
}
