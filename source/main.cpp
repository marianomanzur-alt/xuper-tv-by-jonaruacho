#include "catalog.hpp"
#include "player.hpp"
#include "service.hpp"
#include "view.hpp"
#include "trace.hpp"
#include <fstream>
#include <cstdio>
#include <algorithm>
#include <utility>
#include <vector>
#include <cstring>
#include <cmath>
#include "playback_state.hpp"
#include "saved_catalog.hpp"
#include "download_manager.hpp"
#include "offline_library.hpp"
#include "subtitle_search.hpp"
#include "variant_pick.hpp"
#include "subtitle_query.hpp"
#include <sys/stat.h>
#include <cctype>
#include <thread>
#include <atomic>
#include <memory>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>

#endif
static bool search_keyboard(xs::View& view,const xs::Catalog& catalog,size_t selected,bool full_memory,std::string& value) {
#ifdef __SWITCH__
    (void)view;(void)catalog;(void)selected;(void)full_memory;
    SwkbdConfig keyboard;char buffer[241]{};
    if(R_FAILED(swkbdCreate(&keyboard,0)))return false;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard,"Buscar canales, peliculas y series");
    swkbdConfigSetGuideText(&keyboard,"Escribe parte del titulo");
    swkbdConfigSetStringLenMax(&keyboard,80);
    const Result result=swkbdShow(&keyboard,buffer,sizeof(buffer));swkbdClose(&keyboard);
    if(R_FAILED(result))return false;
    value=buffer;return !value.empty();
#else
    value.clear();SDL_StartTextInput();bool accepted=false,running=true;
    while(running){view.draw(catalog,selected,"Buscar: "+value+" | Enter: buscar; Esc: cancelar",full_memory);
        SDL_Event event;while(SDL_PollEvent(&event)){
            if(event.type==SDL_QUIT)running=false;
            if(event.type==SDL_TEXTINPUT&&value.size()+std::strlen(event.text.text)<=240)value+=event.text.text;
            if(event.type==SDL_KEYDOWN){if(event.key.keysym.sym==SDLK_ESCAPE)running=false;
                if(event.key.keysym.sym==SDLK_RETURN){accepted=!value.empty();running=false;}
                if(event.key.keysym.sym==SDLK_BACKSPACE&&!value.empty()){
                    size_t cut=value.size()-1;while(cut>0&&(static_cast<unsigned char>(value[cut])&0xc0)==0x80)--cut;value.resize(cut);
                }
            }
        }SDL_Delay(10);
    }SDL_StopTextInput();return accepted;
#endif
}
// Mientras se reproduce un video las descargas se pausan (la consola tiene poca CPU y la SD
// se comparte). Ponlo en false si prefieres que sigan bajando mientras ves algo.
static constexpr bool kPauseDownloadsWhilePlaying=true;
static std::string downloads_directory(){
#ifdef __SWITCH__
    return "sdmc:/switch/xuperswitch/descargas";
#else
    char cwd[1024]{};
    return (getcwd(cwd,sizeof(cwd))?std::string(cwd):std::string("."))+"/descargas";
#endif
}
// Subtitulo descargado junto al video: lo lee el menu de subtitulos del reproductor.
static bool read_local_subtitle(const std::string& file,std::string& data,std::string& error){
    std::ifstream f(file,std::ios::binary|std::ios::ate);const auto n=f.tellg();
    if(!f.good()||n<=0||n>1024*1024){error="No se pudo leer el subtitulo guardado.";return false;}
    f.seekg(0);data.assign(static_cast<size_t>(n),'\0');f.read(&data[0],n);
    if(!f){error="No se pudo leer el subtitulo guardado.";return false;}
    return true;
}
static std::string subtitles_directory(){
#ifdef __SWITCH__
    return "sdmc:/switch/xuperswitch/subtitulos";
#else
    char cwd[1024]{};
    return (getcwd(cwd,sizeof(cwd))?std::string(cwd):std::string("."))+"/subtitulos";
#endif
}
// Guarda el subtitulo (ya en UTF-8) en la SD con un nombre legible. Devuelve la ruta o "" si no pudo.
static std::string save_subtitle_file(const std::string& base,const xs::SubtitleHit& hit,const std::string& data){
    const std::string dir=subtitles_directory();mkdir(dir.c_str(),0777);
    const std::string path=dir+"/"+base+"."+hit.language+"."+(hit.provider=="SubDL"?"SubDL":"OpenSubtitles")+".srt";
    std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(data.data(),static_cast<std::streamsize>(data.size()));out.close();
    return out?path:std::string();
}
static std::string safe_name(const std::string& text,size_t limit){
    std::string out;for(unsigned char c:text){if(std::isalnum(c)||c=='-'||c=='_')out+=static_cast<char>(c);else if(!out.empty()&&out.back()!='_')out+='_';if(out.size()>=limit)break;}
    while(!out.empty()&&out.back()=='_')out.pop_back();
    return out.empty()?std::string("video"):out;
}
int main(int argc,char** argv){
    xs::trace_reset();xs::trace("main:begin");
    const char* path=argc>1?argv[1]:
#ifdef __SWITCH__
        "sdmc:/switch/xuperswitch/catalog.m3u";
    Result rc=socketInitializeDefault();const bool network=R_SUCCEEDED(rc);
    PadState pad;padConfigureInput(1,HidNpadStyleSet_NpadStandard);padInitializeDefault(&pad);
    const auto type=appletGetAppletType();
    const bool full_memory=type==AppletType_Application||type==AppletType_SystemApplication;
#else
        "catalog.m3u";
    const bool full_memory=true;
#endif
    std::ifstream input(path);xs::Catalog catalog=xs::read_m3u(input);
    std::string status="Conectando...";
    if(!input.is_open())status="No se pudo abrir catalog.m3u.";
    xs::View view;std::string error;
    if(!view.open(error)){xs::trace("main:view_failed");std::fprintf(stderr,"Pantalla: %s\n",error.c_str());
#ifdef __SWITCH__
        if(network)socketExit();
#endif
        return 1;
    }
    xs::PlaybackHistory playback_history;
    xs::SavedCatalog saved(
#ifdef __SWITCH__
        "sdmc:/switch/xuperswitch/library_state.json"
#else
        "library_state.json"
#endif
    );
    // Descargas para ver sin conexion: se guardan en la SD y se registran en offline_library.json.
    std::unique_ptr<xs::DownloadManager> downloads;
    {xs::DownloadManager::Config config;config.directory=downloads_directory();
#ifdef __SWITCH__
        config.library_path="sdmc:/switch/xuperswitch/offline_library.json";config.ca_file="sdmc:/switch/xuperswitch/cacert.pem";
#else
        config.library_path="offline_library.json";config.ca_file="";
#endif
        downloads=std::make_unique<xs::DownloadManager>(config);}
    // Busqueda de subtitulos en internet (claves en subtitulos.json; ver LEEME-SUBTITULOS.txt).
    std::shared_ptr<xs::SubtitleSearch> subtitle_search;std::string subtitle_search_problem;
    std::vector<std::string> subtitle_languages;bool config_auto_default=false;
    {xs::SubtitleSearchConfig sconfig;
#ifdef __SWITCH__
        sconfig.ca_file="sdmc:/switch/xuperswitch/cacert.pem";
        const std::string spath="sdmc:/switch/xuperswitch/subtitulos.json";
#else
        const std::string spath="subtitulos.json";
#endif
        xs::load_subtitle_config(spath,sconfig,subtitle_search_problem);
        subtitle_languages=sconfig.languages;config_auto_default=sconfig.auto_default;
        subtitle_search=std::make_shared<xs::SubtitleSearch>(std::move(sconfig));}
    view.badge=[&](const xs::Entry& e){return e.content_id.empty()?std::string():downloads->status_text(e.content_id);};
    std::string confirm_delete;Uint32 last_tick=0;bool keep_awake=false;(void)keep_awake;
    size_t selected=0;bool running=true,redraw=true,autoload=true,donating=false;
    xs::Catalog root_catalog;
    std::atomic<bool> cancel_task{false};std::atomic<size_t> task_done{0},task_total{0};
    std::shared_ptr<xs::ServiceSession> session;
    std::vector<std::pair<xs::Catalog,size_t>> history;
    auto home=[&]{xs::Catalog c;c.title="Inicio";
        xs::Entry tv;tv.title="TV en vivo";tv.kind=xs::EntryKind::LiveSection;c.entries.push_back(tv);
        for(const char* type:{"1","0"}){auto i=std::find_if(root_catalog.entries.begin(),root_catalog.entries.end(),[&](const xs::Entry& e){return e.kind==xs::EntryKind::Section&&e.program_type==type;});if(i!=root_catalog.entries.end())c.entries.push_back(*i);}
        xs::Entry resume;resume.title="Continuar viendo";resume.kind=xs::EntryKind::Resume;c.entries.push_back(resume);
        xs::Entry fav;fav.title="Favoritos";fav.kind=xs::EntryKind::Favorites;c.entries.push_back(fav);
        xs::Entry saved_videos;saved_videos.title="Descargas";saved_videos.kind=xs::EntryKind::Downloads;c.entries.push_back(saved_videos);
        xs::Entry donate;donate.title="Apoyar el desarrollo";donate.kind=xs::EntryKind::Donation;c.entries.push_back(donate);return c;
    };
    // Sin conexion: solo lo que se puede ver desde la SD.
    auto offline_home=[&]{xs::Catalog c;c.title="Sin conexion";
        xs::Entry saved_videos;saved_videos.title="Descargas";saved_videos.kind=xs::EntryKind::Downloads;c.entries.push_back(saved_videos);
        xs::Entry resume;resume.title="Continuar viendo";resume.kind=xs::EntryKind::Resume;c.entries.push_back(resume);
        xs::Entry donate;donate.title="Apoyar el desarrollo";donate.kind=xs::EntryKind::Donation;c.entries.push_back(donate);return c;};
    // Vuelve a leer la pantalla Descargas conservando el elemento elegido.
    auto refresh_downloads=[&]{
        if(catalog.title!="Descargas"||(!catalog.entries.empty()&&catalog.entries[0].kind!=xs::EntryKind::Offline&&catalog.entries[0].kind!=xs::EntryKind::Downloads))return;
        const std::string keep=catalog.entries.empty()?"":catalog.entries[std::min(selected,catalog.entries.size()-1)].content_id;
        catalog.entries=downloads->entries();
        selected=0;for(size_t i=0;i<catalog.entries.size();++i)if(!keep.empty()&&catalog.entries[i].content_id==keep){selected=i;break;}
        if(selected>=catalog.entries.size())selected=catalog.entries.empty()?0:catalog.entries.size()-1;
        redraw=true;
    };
    auto load=[&](auto task,const std::string& message,bool splash=false){
        cancel_task=false;task_done=0;task_total=0;std::atomic<bool> complete{false};
        const auto started=SDL_GetTicks();if(splash)view.splash(message);
        std::thread worker([&]{task();complete.store(true);});
        while(!complete.load()||(splash&&SDL_GetTicks()-started<300)){
            SDL_Event e;while(SDL_PollEvent(&e)){if(e.type==SDL_QUIT){running=false;cancel_task=true;}if(e.type==SDL_KEYDOWN&&(e.key.keysym.sym==SDLK_ESCAPE||e.key.keysym.sym==SDLK_b))cancel_task=true;}
#ifdef __SWITCH__
            if(!appletMainLoop()){running=false;cancel_task=true;}padUpdate(&pad);if(padGetButtonsDown(&pad)&HidNpadButton_B)cancel_task=true;
#endif
            const auto progress=task_total.load()?"  TV "+std::to_string(task_done.load())+"/"+std::to_string(task_total.load()):"";
            if(splash)view.splash(message);else view.draw(catalog,selected,message+progress+(cancel_task.load()?" - terminando consulta...":""),full_memory);
            SDL_Delay(16);
        }worker.join();
#ifdef __SWITCH__
        padUpdate(&pad);
#endif
    };
    while(running){
#ifdef __SWITCH__
        if(!appletMainLoop())break;
#endif
        if(view.poll_images())redraw=true;
        {   // Descargas: avisos de fin, progreso en la barra superior y refresco mientras hay trabajo.
            const std::string event=downloads->take_event();
            if(!event.empty()){status=event;refresh_downloads();redraw=true;}
            const Uint32 now=SDL_GetTicks();
            if(downloads->busy()&&now-last_tick>700){last_tick=now;redraw=true;}
            const std::string summary=downloads->overlay();
            if(summary!=view.overlay){view.overlay=summary;redraw=true;}
#ifdef __SWITCH__
            // Mientras hay descargas pendientes se evita que la consola entre en reposo y las corte.
            const bool want_awake=downloads->busy();
            if(want_awake!=keep_awake){appletSetMediaPlaybackState(want_awake);keep_awake=want_awake;}
#endif
        }
        if(redraw){view.favorite=!catalog.entries.empty()&&saved.contains(catalog.entries[std::min(selected,catalog.entries.size()-1)]);if(donating)view.donation();else view.draw(catalog,selected,status,full_memory);redraw=false;}
        bool up=false,down=false,left=false,right=false,play=false,reload=false,login=autoload,search=false,back=false,next=false,previous=false,toggle_favorite=false,download_one=false,download_rest=false;int nav_action=-1;autoload=false;SDL_Event event;
        while(SDL_PollEvent(&event)){
            if(event.type==SDL_QUIT)running=false;
            if(event.type==SDL_WINDOWEVENT)redraw=true;
            if(event.type==SDL_MOUSEWHEEL){previous|=event.wheel.y>0;next|=event.wheel.y<0;}
            if(event.type==SDL_MOUSEBUTTONDOWN||event.type==SDL_FINGERDOWN){
                const int x=event.type==SDL_FINGERDOWN?int(event.tfinger.x*1280):event.button.x,y=event.type==SDL_FINGERDOWN?int(event.tfinger.y*720):event.button.y;
                if(donating){back=true;continue;}const int hit=view.hit(catalog,selected,x,y);
                if(hit==-20)toggle_favorite=true;else if(hit<=-2&&hit>=-8)nav_action=-hit-2;else if(hit>=0){selected=static_cast<size_t>(hit);view.nav_focus=-1;play=true;}
            }
            if(event.type==SDL_KEYDOWN){
                switch(event.key.keysym.sym){
                    case SDLK_ESCAPE:running=false;break;
                    case SDLK_UP:up=true;break;
                    case SDLK_DOWN:down=true;break;
                    case SDLK_LEFT:left=true;break;
                    case SDLK_RIGHT:right=true;break;
                    case SDLK_f:toggle_favorite=true;break;
                    case SDLK_d:nav_action=6;break;
                    case SDLK_g:download_one=true;break;
                    case SDLK_h:download_rest=true;break;
                    case SDLK_RETURN:play=true;break;
                    case SDLK_x:search=true;break;
                    case SDLK_l:reload=true;break;
                    case SDLK_y:login=true;break;
                    case SDLK_b:back=true;break;
                    case SDLK_PAGEDOWN:next=true;break;
                    case SDLK_PAGEUP:previous=true;break;
                }
            }
        }
#ifdef __SWITCH__
        padUpdate(&pad);u64 buttons=padGetButtonsDown(&pad);
        if(buttons&HidNpadButton_Plus)running=false;
        up|=(buttons&(HidNpadButton_Up|HidNpadButton_StickLUp))!=0;down|=(buttons&(HidNpadButton_Down|HidNpadButton_StickLDown))!=0;
        left|=(buttons&(HidNpadButton_Left|HidNpadButton_StickLLeft))!=0;right|=(buttons&(HidNpadButton_Right|HidNpadButton_StickLRight))!=0;
        toggle_favorite|=(buttons&HidNpadButton_StickR)!=0;if(buttons&HidNpadButton_Minus)nav_action=6;
        download_one|=(buttons&HidNpadButton_ZR)!=0;download_rest|=(buttons&HidNpadButton_StickL)!=0;
        play|=(buttons&HidNpadButton_A)!=0;reload|=(buttons&HidNpadButton_ZL)!=0;search|=(buttons&HidNpadButton_X)!=0;
        login|=(buttons&HidNpadButton_Y)!=0;
        back|=(buttons&HidNpadButton_B)!=0;
        next|=(buttons&HidNpadButton_R)!=0;previous|=(buttons&HidNpadButton_L)!=0;
#endif
        if(donating){if(back||play||login){donating=false;redraw=true;}SDL_Delay(10);continue;}
        if(up||down||left||right||next||previous||back||play||search||login)confirm_delete.clear();
        if(view.nav_focus>=0){
            if(up){view.nav_focus=(view.nav_focus+6)%7;redraw=true;}if(down){view.nav_focus=(view.nav_focus+1)%7;redraw=true;}
            if(right||back){view.nav_focus=-1;redraw=true;back=false;}if(play){nav_action=view.nav_focus;play=false;}
            up=down=left=right=false;
        }else if(left&&selected%static_cast<size_t>(view.columns(catalog))==0){view.nav_focus=view.active_nav;left=false;redraw=true;}
        if(nav_action==0)login=true;
        if(login){
            if(!session){
                xs::LoginAttempt attempt;xs::trace("bootstrap:begin");
                load([&]{attempt=xs::service_bootstrap(
#ifdef __SWITCH__
                    "sdmc:/switch/xuperswitch/service.json"
#else
                    "service.json"
#endif
                );},"Cargando tu catalogo...",true);
                xs::trace(attempt.authenticated?"bootstrap:accepted":"bootstrap:failed");
                session=attempt.session;status=attempt.status;root_catalog=std::move(attempt.catalog);
                if(session){
                    // El hilo de descargas pide su propia fuente (URL firmada) cuando le toca cada video.
                    const auto active=session;
                    downloads->set_resolver([active](const xs::Entry& e){return xs::service_source(active,e,0);},
                        [active](const xs::SubtitleSource& sub,std::string& data,std::string& error){return xs::service_subtitle(active,sub,data,error);});
                }
            }
            if(session&&root_catalog.entries.empty()){
                xs::BrowseResult result;load([&]{result=xs::service_root(session);},"Reintentando carga del catalogo...");status=result.status;if(result.ok)root_catalog=std::move(result.catalog);
            }
            if(session){catalog=home();if(!root_catalog.entries.empty())status="Elige que quieres ver. X: buscar en todo el catalogo.";else status+=" Pulsa Y para reintentar el catalogo.";}else {catalog=offline_home();status+=" Puedes ver tus Descargas. Pulsa Y para reintentar la conexion.";}
            selected=0;history.clear();view.active_nav=0;view.nav_focus=-1;redraw=true;
        }
        if(nav_action>0){
            if(nav_action==6){donating=true;redraw=true;continue;}
            if(nav_action==5){history.emplace_back(catalog,selected);catalog={};catalog.title="Descargas";catalog.entries=downloads->entries();selected=0;view.active_nav=5;view.nav_focus=-1;
                status=catalog.entries.empty()?"Aun no hay descargas.":"Descargas. A: reproducir sin conexion; ZR: borrar o cancelar; B: volver.";redraw=true;continue;}
            if(nav_action==4){history.emplace_back(catalog,selected);catalog={};catalog.title="Favoritos";catalog.entries=saved.favorites;selected=0;view.active_nav=4;view.nav_focus=-1;redraw=true;}
            else if(session){
                if(nav_action==1){history.emplace_back(catalog,selected);catalog={};catalog.title="TV en vivo";for(const auto& e:root_catalog.entries)if(e.kind==xs::EntryKind::Category)catalog.entries.push_back(e);selected=0;}
                else{const std::string type=nav_action==2?"1":"0";const auto i=std::find_if(root_catalog.entries.begin(),root_catalog.entries.end(),[&](const xs::Entry& e){return e.kind==xs::EntryKind::Section&&e.program_type==type;});
                    if(i!=root_catalog.entries.end()){xs::BrowseResult result;load([&]{result=xs::service_browse(session,*i);},"Cargando categorias...");status=result.status;if(result.ok){history.emplace_back(catalog,selected);catalog=std::move(result.catalog);catalog.title=i->title;selected=0;}}
                    else status="Esta seccion no esta disponible en el servicio.";
                }view.active_nav=nav_action;view.nav_focus=-1;redraw=true;
            }else{status="Sin sesion. Pulsa Y para reintentar la conexion.";redraw=true;}
        }
        if(toggle_favorite&&!catalog.entries.empty()){
            const auto entry=catalog.entries[selected];
            if(!xs::SavedCatalog::can_favorite(entry))status="Selecciona un canal, serie o pelicula para guardarlo en Favoritos.";
            else if(!saved.toggle(entry))status="No se pudo guardar Favoritos en la SD.";
            else{const char* what=entry.kind==xs::EntryKind::Channel?"Canal":entry.program_type=="0"?"Serie":"Pelicula";
                status=std::string(what)+(saved.contains(entry)?" guardado en Favoritos.":" quitado de Favoritos.");if(catalog.title=="Favoritos"){catalog.entries=saved.favorites;selected=catalog.entries.empty()?0:std::min(selected,catalog.entries.size()-1);}}
            redraw=true;
        }
        if(download_one||download_rest){
            if(catalog.entries.empty())status="Abre una pelicula o un episodio para descargarlo.";
            else{
                const size_t index=std::min(selected,catalog.entries.size()-1);const xs::Entry entry=catalog.entries[index];
                if(entry.kind==xs::EntryKind::Offline){
                    // En Descargas: ZR cancela lo que se esta bajando o, con confirmacion, borra lo guardado.
                    if(entry.source.empty()){status=downloads->cancel(entry.content_id)?"Descarga cancelada.":"No se pudo cancelar esa descarga.";confirm_delete.clear();}
                    else if(confirm_delete==entry.content_id){status=downloads->remove_downloaded(entry.content_id)?"Borrado de la SD.":"No se pudo borrar el archivo.";confirm_delete.clear();}
                    else{confirm_delete=entry.content_id;status="Pulsa ZR otra vez para borrar de la SD: "+entry.title;}
                    refresh_downloads();
                }else if(entry.kind==xs::EntryKind::Episode){
                    const bool generic=catalog.title=="Continuar viendo"||catalog.title.rfind("Resultados",0)==0;
                    const std::string show=generic?std::string():catalog.title;
                    size_t queued=0;xs::EnqueueResult last_result=xs::EnqueueResult::NotSupported;
                    for(size_t i=index;i<(download_rest?catalog.entries.size():index+1);++i){
                        const xs::Entry& e=catalog.entries[i];
                        if(e.kind!=xs::EntryKind::Episode||(download_rest&&e.series_id!=entry.series_id))continue;
                        last_result=downloads->enqueue(e,show.empty()?e.show:show,static_cast<int>(i));
                        if(last_result==xs::EnqueueResult::Added||last_result==xs::EnqueueResult::Retrying)++queued;
                        if(last_result==xs::EnqueueResult::NoSession||last_result==xs::EnqueueResult::QueueFull)break;
                    }
                    if(queued)status="En cola para descargar: "+std::to_string(queued)+(queued==1?" video":" videos")+". El avance se ve arriba; la lista esta en Descargas.";
                    else switch(last_result){
                        case xs::EnqueueResult::AlreadyDownloaded:status="Ya esta descargado. Esta en Descargas.";break;
                        case xs::EnqueueResult::AlreadyQueued:status="Ya esta en la cola de descargas.";break;
                        case xs::EnqueueResult::NoSession:status="Necesitas conexion para iniciar una descarga. Pulsa Y para reintentar.";break;
                        case xs::EnqueueResult::QueueFull:status="La cola de descargas esta llena (200).";break;
                        default:status="Este contenido no se puede descargar.";break;
                    }
                }else status="Abre una pelicula o serie y elige el episodio que quieres descargar.";
            }
            redraw=true;
        }
        if(search){
            if(!session)status="Pulsa Y para activar antes de buscar.";
            else{
                std::string query;
                if(search_keyboard(view,catalog,selected,full_memory,query)){
                    xs::BrowseResult result;xs::trace("catalog:search_begin");
                    load([&]{result=xs::service_search_all(session,query,[&](size_t done,size_t total){task_done=done;task_total=total;return !cancel_task.load();});},"Buscando canales, peliculas y series... B: detener TV");status=result.status;
                    xs::trace(result.ok?"catalog:search_loaded":"catalog:search_failed");
                    if(result.ok){history.emplace_back(std::move(catalog),selected);catalog=std::move(result.catalog);selected=0;}
                }
            }
#ifdef __SWITCH__
            padUpdate(&pad); // Consume the keyboard's confirmation button state.
#endif
            redraw=true;
        }
        if(back&&!history.empty()) {
            catalog=std::move(history.back().first);selected=history.back().second;history.pop_back();
            status="Elige un titulo o canal.";if(history.empty())view.active_nav=0;redraw=true;
        }
        if(!catalog.entries.empty()){
            const size_t cols=view.columns(catalog),page=view.page_size(catalog);
            if(up){selected=selected>=cols?selected-cols:selected;redraw=true;}
            if(down){selected=std::min(selected+cols,catalog.entries.size()-1);redraw=true;}
            if(left){if(selected>0)--selected;redraw=true;}if(right){selected=std::min(selected+1,catalog.entries.size()-1);redraw=true;}
            if(next){selected=std::min(selected+page,catalog.entries.size()-1);redraw=true;}
            if(previous){selected=selected>=page?selected-page:0;redraw=true;}
            if(play){
                const xs::Entry entry=catalog.entries[selected];
                if(entry.kind==xs::EntryKind::Donation){donating=true;redraw=true;continue;}
                if(entry.kind==xs::EntryKind::LiveSection||entry.kind==xs::EntryKind::Favorites||entry.kind==xs::EntryKind::Resume||entry.kind==xs::EntryKind::Downloads){
                    history.emplace_back(catalog,selected);catalog={};catalog.title=entry.title;
                    if(entry.kind==xs::EntryKind::LiveSection){for(const auto& e:root_catalog.entries)if(e.kind==xs::EntryKind::Category)catalog.entries.push_back(e);view.active_nav=1;}
                    else if(entry.kind==xs::EntryKind::Favorites){catalog.entries=saved.favorites;view.active_nav=4;}
                    else if(entry.kind==xs::EntryKind::Downloads){catalog.entries=downloads->entries();view.active_nav=5;
                        status=catalog.entries.empty()?"Aun no hay descargas.":"Descargas. A: reproducir sin conexion; ZR: borrar o cancelar; B: volver.";}
                    else for(const auto& e:saved.recent)if(e.kind==xs::EntryKind::Episode&&playback_history.position(e.content_id)>0&&(session||downloads->downloaded(e.content_id)))catalog.entries.push_back(e);
                    selected=0;redraw=true;continue;
                }
                if(entry.kind==xs::EntryKind::Offline&&entry.source.empty()){status="Esta descarga aun no termina. ZR: cancelarla.";redraw=true;continue;}
                if(entry.kind==xs::EntryKind::Category||entry.kind==xs::EntryKind::Section||
                   entry.kind==xs::EntryKind::VodCategory||entry.kind==xs::EntryKind::Asset||
                   entry.kind==xs::EntryKind::VodPage||entry.kind==xs::EntryKind::SearchPage){
                    status="Cargando contenido...";view.draw(catalog,selected,status,full_memory);
                    xs::trace("catalog:browse_begin");
                    xs::BrowseResult result;load([&]{result=entry.kind==xs::EntryKind::Category?xs::service_channels(session,entry.column_id):xs::service_browse(session,entry);},"Cargando "+entry.title+"...");status=result.status;
                    if(result.catalog.title.empty())result.catalog.title=entry.title;
                    xs::trace(result.ok?"catalog:browse_loaded":"catalog:browse_failed");
                    if(result.ok){history.emplace_back(std::move(catalog),selected);catalog=std::move(result.catalog);selected=0;}
                }else if(!full_memory)status="Abre hbmenu manteniendo R sobre un juego para probar el video.";
                else{
                    int variant=0;bool variant_pinned=false;double start=0,speed=playback_history.speed,volume=playback_history.volume;bool auto_next=playback_history.auto_next,keep_paused=false;
                    std::vector<size_t> queue;
                    for(size_t i=0;i<catalog.entries.size();++i)if(catalog.entries[i].kind==entry.kind&&(entry.series_id.empty()||catalog.entries[i].series_id==entry.series_id)&&(entry.kind!=xs::EntryKind::Offline||!catalog.entries[i].source.empty()))queue.push_back(i);
                    bool continue_playback=true;
                    while(continue_playback){continue_playback=false;const auto current=catalog.entries[selected];
                        std::string source=current.source;std::vector<std::pair<std::string,std::string>> headers;
                        xs::PlaybackOptions options;options.font=view.font;options.title=current.title;
                        options.variant=variant;options.start=start>0?start:(current.kind==xs::EntryKind::Episode||current.kind==xs::EntryKind::Offline)?playback_history.position(current.content_id):0;options.speed=speed;options.volume=volume;options.auto_next=auto_next;options.paused=keep_paused;
                        options.sub_scale=playback_history.sub_scale;options.quality=playback_history.quality;options.buffer_mb=full_memory?64:32;
                        options.alang=playback_history.alang;options.slang=playback_history.slang;options.service_subtitle_pref=playback_history.service_sub;
                        {const std::string& series_key=current.series_id.empty()?current.content_id:current.series_id;
                         if(current.kind==xs::EntryKind::Episode||current.kind==xs::EntryKind::Offline){options.subtitle_file=playback_history.subtitle_for(current.content_id);options.subtitle_hint=playback_history.series_hint(series_key);}}
                        // Si el contenido esta guardado en la SD se reproduce desde ahi (sin red ni consumo de datos).
                        xs::OfflineItem local;bool use_local=false;
                        if(current.kind==xs::EntryKind::Episode||current.kind==xs::EntryKind::Offline)use_local=downloads->find_downloaded(current.content_id,local);
                        if(current.kind==xs::EntryKind::Offline&&!use_local){status="El archivo ya no esta en la SD. ZR lo quita de la lista.";downloads->prune();break;}
                        if(use_local){
                            source=local.file;
                            for(const auto& sub:local.subtitles)options.subtitles.push_back(sub.label);
                            options.subtitle_loader=[files=local.subtitles](size_t i,std::string& data,std::string& error){
                                if(i>=files.size()){error="Subtitulo no disponible.";return false;}
                                return read_local_subtitle(files[i].file,data,error);};
                            if(options.subtitle_file.empty()||!std::ifstream(options.subtitle_file).good()){
                                const int pick=xs::default_subtitle(local.subtitles,playback_history.slang);
                                options.subtitle_file=pick>=0?local.subtitles[static_cast<size_t>(pick)].file:std::string();}
                            status="Reproduciendo desde la SD (sin conexion).";
                        }else
                        if(current.kind==xs::EntryKind::Channel||current.kind==xs::EntryKind::Episode){
                            status="Solicitando fuente del video...";view.draw(catalog,selected,status,full_memory);
                            xs::SourceResult result;xs::trace("catalog:source_begin");load([&]{result=xs::service_source(session,current,variant);},"Preparando reproduccion...");status=result.status;
                            // Misma fuente (calidad / idioma de audio) que elegiste antes, si este capitulo la ofrece.
                            if(result.ok&&!variant_pinned&&variant==0&&current.kind==xs::EntryKind::Episode){
                                const int wanted=xs::pick_variant(result.variants,playback_history.variant_label);
                                if(wanted>0){xs::SourceResult again;load([&]{again=xs::service_source(session,current,wanted);},"Aplicando tu fuente preferida...");
                                    if(again.ok){result=std::move(again);variant=wanted;options.variant=variant;status=result.status;}}}
                            xs::trace(result.ok?"catalog:source_received":"catalog:source_unavailable");
                            source=result.ok?std::move(result.source):"";headers=std::move(result.headers);options.variants=std::move(result.variants);
                            const auto subtitles=result.subtitles;
                            for(const auto& sub:subtitles)options.subtitles.push_back(sub.language);
                            options.subtitle_loader=[session,subtitles](size_t i,std::string& data,std::string& error){
                                return i<subtitles.size()&&xs::service_subtitle(session,subtitles[i],data,error);
                            };
                        }
                        // Buscar subtitulos en internet (manual desde el menu y automatico al abrir el video).
                        if(subtitle_search&&(current.kind==xs::EntryKind::Episode||current.kind==xs::EntryKind::Offline||current.kind==xs::EntryKind::Media)){
                            auto hits=std::make_shared<std::vector<xs::SubtitleHit>>();
                            const xs::SubtitleQuery query=xs::subtitle_query_for(current,catalog.title);
                            std::string readable=safe_name(query.title.empty()?current.title:query.title,40);
                            if(query.episode>0)readable+=(query.season>0?"_S"+std::to_string(query.season):std::string())+"_E"+std::to_string(query.episode);
                            const std::string file_base=readable;
                            const std::string problem_at_start=subtitle_search_problem;
                            options.online_search=[subtitle_search,hits,query,problem_at_start](const xs::MediaInfo& media,std::vector<std::string>& labels,std::string& error){
                                if(!problem_at_start.empty()){error=problem_at_start;return false;}
                                xs::SubtitleContext context;context.fps=media.fps;context.duration=media.duration;
                                if(!subtitle_search->search(query,*hits,error,context))return false;
                                for(const auto& h:*hits)labels.push_back(h.label);
                                return true;};
                            options.online_loader=[subtitle_search,hits,file_base](size_t i,std::string& data,std::string& saved_or_error){
                                if(i>=hits->size()){saved_or_error="Subtitulo no disponible.";return false;}
                                std::string error;
                                if(!subtitle_search->fetch((*hits)[i],data,error)){saved_or_error=error;return false;}
                                // UTF-8 limpio y validado: si el archivo viene vacio, roto o en otra codificacion se corrige aqui.
                                xs::SubtitleInfo info;if(!xs::normalize_subtitle(data,info,error)){saved_or_error=error;return false;}
                                saved_or_error=save_subtitle_file(file_base,(*hits)[i],data);   // vacio: el reproductor usa un temporal
                                return true;};
                            // Automatico: interruptor recordado; si nunca se toco, vale "auto_buscar" de subtitulos.json.
                            options.auto_subtitles=playback_history.auto_subs<0?config_auto_default:playback_history.auto_subs==1;
                            options.auto_skip_lang=subtitle_languages.empty()?std::string():subtitle_languages[0];
                            options.auto_subtitle=[subtitle_search,query,file_base,problem_at_start](const xs::MediaInfo& media,const std::atomic<bool>& cancel,std::string& label,std::string& path,std::string& message){
                                if(!problem_at_start.empty()){message=problem_at_start;return false;}
                                xs::SubtitleContext context;context.fps=media.fps;context.duration=media.duration;
                                xs::SubtitleHit chosen;std::string data,error;
                                if(!subtitle_search->auto_fetch(query,context,chosen,data,error,&cancel)){message=error;return false;}
                                path=save_subtitle_file(file_base,chosen,data);
                                if(path.empty()){message="No se pudo guardar el subtitulo en la SD.";return false;}
                                label=chosen.label;return true;};}
                        for(size_t i=0;i<queue.size();++i){options.items.push_back(catalog.entries[queue[i]].title);if(queue[i]==selected)options.item=static_cast<int>(i);}
                        if(!source.empty()){
                            xs::PlaybackOutcome outcome;
                            if(kPauseDownloadsWhilePlaying)downloads->set_paused(true);
                            status=xs::play(source,view.renderer,headers,options,&outcome);xs::trace("main:video_returned");
                            downloads->set_paused(false);keep_awake=false;  // play() libera el estado de reproduccion al cerrar
                            speed=outcome.speed;volume=outcome.volume;auto_next=outcome.auto_next;
                            if(status.empty()){ // si la reproduccion fallo, outcome no es confiable
                                playback_history.sub_scale=outcome.sub_scale;playback_history.quality=outcome.quality;if(options.auto_subtitle)playback_history.auto_subs=outcome.auto_subtitles?1:0;
                                playback_history.service_sub=outcome.service_subtitle;
                                if(outcome.next_variant>=0&&static_cast<size_t>(outcome.next_variant)<options.variants.size())playback_history.variant_label=xs::variant_key(options.variants[static_cast<size_t>(outcome.next_variant)]);
                                if(!outcome.alang.empty()&&xs::PlaybackHistory::lang_ok(outcome.alang))playback_history.alang=outcome.alang;
                                if(!outcome.slang.empty()&&xs::PlaybackHistory::lang_ok(outcome.slang))playback_history.slang=outcome.slang;
                                if(current.kind==xs::EntryKind::Episode||current.kind==xs::EntryKind::Offline)playback_history.remember_subtitle(current.content_id,current.series_id.empty()?current.content_id:current.series_id,outcome.subtitle_file);
                            }
                            if(status.empty()&&(outcome.position>3||outcome.ended)){
                                // "Continuar viendo" guarda episodios; lo visto desde la SD cuenta igual.
                                xs::Entry recent=current;if(recent.kind==xs::EntryKind::Offline){recent.kind=xs::EntryKind::Episode;recent.source.clear();}
                                if(!saved.remember(recent))xs::trace("catalog:recent_save_failed");
                            }
                            if(status.empty()&&!playback_history.save((current.kind==xs::EntryKind::Episode||current.kind==xs::EntryKind::Offline)?current.content_id:"",outcome.position,outcome.speed,outcome.volume,outcome.auto_next,outcome.ended))xs::trace((std::string("play:history_save_failed stage=")+playback_history.last_error).c_str());
                            if(outcome.ended&&auto_next&&!current.series_id.empty()&&static_cast<size_t>(options.item+1)<queue.size())outcome.next_item=options.item+1;
                            if(outcome.next_variant>=0){variant=outcome.next_variant;variant_pinned=true;start=outcome.position;keep_paused=outcome.paused;continue_playback=true;}
                            else if(outcome.next_item>=0&&static_cast<size_t>(outcome.next_item)<queue.size()){
                                selected=queue[outcome.next_item];variant=0;variant_pinned=false;start=0;keep_paused=false;continue_playback=true;
                            }
                            if(status.empty())status="Video finalizado. A: reproducir; X: buscar.";
                        }
                    }

                }
                redraw=true;
            }
        }
        if(reload){std::ifstream file(path);catalog=xs::read_m3u(file);selected=0;history.clear();
            status=file.is_open()?"Lista local recargada.":"No se pudo abrir catalog.m3u.";redraw=true;}
        SDL_Delay(10);
    }
    downloads.reset();  // detiene el hilo de descargas (conserva lo bajado a medias) antes de cerrar la red
    xs::trace("main:close_view");view.close();
#ifdef __SWITCH__
    if(network)socketExit();
#endif
    xs::trace("main:exit");return 0;
}
