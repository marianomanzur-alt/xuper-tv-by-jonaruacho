#include "player.hpp"
#include "catalog.hpp"
#include "trace.hpp"
#include "playback_error.hpp"
#include "subfile_browser.hpp"
#include <mpv/client.h>
#include <mpv/render.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace xs {
// "es" coincide con las etiquetas de pista "es", "spa" o "esl" (y lo mismo para otros idiomas comunes).
static bool lang_matches(const std::string& track,const std::string& wanted){
    static const char* groups[][4]={{"es","spa","esl",""},{"en","eng","",""},{"pt","por","",""},{"fr","fre","fra",""},{"it","ita","",""},{"de","ger","deu",""}};
    auto low=[](std::string v){for(char& c:v)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return v;};
    const std::string a=low(track),b=low(wanted);if(a.empty()||b.empty())return false;
    for(const auto& g:groups){bool has_a=false,has_b=false;for(const char* x:g){if(!*x)continue;if(a==x||a.rfind(std::string(x)+"-",0)==0)has_a=true;if(b==x)has_b=true;}if(has_b)return has_a;}
    return a==b;
}
static void update(void* p){static_cast<std::atomic<bool>*>(p)->store(true);}
std::string play(const std::string& source,SDL_Renderer* renderer,const std::vector<std::pair<std::string,std::string>>& headers,const PlaybackOptions& options,PlaybackOutcome* outcome) {
    if(!renderer||!valid_source(source))return "Origen o pantalla no admitidos.";
    trace("play:begin");
    mpv_handle* mpv=mpv_create();mpv_render_context* context=nullptr;SDL_Texture* texture=nullptr;
    void* pixels=nullptr;std::atomic<bool> dirty{true};std::string error;bool subtitle_written=false;
#ifdef __SWITCH__
    const char* subtitle_cache="sdmc:/switch/xuperswitch/subtitle_cache.srt";
#else
    const char* subtitle_cache="subtitle_cache.srt";
#endif
    auto cleanup=[&]{
        trace("play:free_context_begin");
        if(context)mpv_render_context_free(context);
        trace("play:free_context_end");
        if(mpv)mpv_terminate_destroy(mpv);
        trace("play:mpv_destroyed");
        if(texture)SDL_DestroyTexture(texture);
        std::free(pixels);
        if(subtitle_written)std::remove(subtitle_cache);
#ifdef __SWITCH__
        appletSetMediaPlaybackState(false);
#endif
        trace("play:return_to_menu");
    };
    if(!mpv){cleanup();return "No se pudo iniciar libmpv.";}
    auto option=[&](const char* name,const char* value){int rc=mpv_set_option_string(mpv,name,value);
        if(rc<0&&error.empty())error=std::string(name)+": "+mpv_error_string(rc);};
    option("config","no");option("load-scripts","no");option("terminal","no");
    option("vo","libmpv");option("hwdec","no");
    option("osd-level","0");option("sub-visibility","yes");
    option("sub-auto","no");option("audio-pitch-correction","yes");
    option("speed",std::to_string(options.speed).c_str());option("volume",std::to_string(options.volume).c_str());
    option("pause",options.paused?"yes":"no");
    option("network-timeout","15");option("tls-verify","yes");
    option("demuxer-max-bytes",std::to_string(std::clamp(options.buffer_mb,16,128)*1024*1024).c_str());
    // Sin este limite libmpv guarda hasta 150 MB de video ya visto y las peliculas pesadas agotan la memoria.
    option("demuxer-max-back-bytes",options.buffer_mb>=64?"16777216":"8388608");
    option("sub-scale",std::to_string(std::clamp(options.sub_scale,0.5,3.0)).c_str());
    auto lang_ok=[](const std::string& v){return v.size()>=2&&v.size()<=8&&std::all_of(v.begin(),v.end(),[](unsigned char c){return std::isalnum(c)||c=='-';});};
    if(lang_ok(options.alang))option("alang",options.alang.c_str());
    if(lang_ok(options.slang))option("slang",options.slang.c_str());
#ifdef __SWITCH__
    option("ao","hos");option("sub-fonts-dir","sdmc:/switch/xuperswitch");option("sub-font","DejaVu Sans");const char* ca="sdmc:/switch/xuperswitch/cacert.pem";
    if(std::ifstream(ca).good())option("tls-ca-file",ca);
#elif defined(XUPERSWITCH_TEST)
    option("ao","null");
#endif
    if(headers.size()>8){cleanup();return "Demasiadas cabeceras de video.";}
    std::vector<std::string> header_lines;std::vector<mpv_node> header_nodes;
    for(const auto& header:headers){
        if((header.first!="Content-Auth"&&header.first!="Content-License"&&header.first!="App"&&header.first!="App-Version"&&header.first!="Ranger-Id"&&header.first!="Pragma")||header.second.empty()||header.second.size()>16384||
           std::any_of(header.second.begin(),header.second.end(),[](unsigned char c){return c<32||c==127;})){cleanup();return "Cabecera de video no admitida.";}
        header_lines.push_back(header.first+": "+header.second);
    }
    if(!header_lines.empty()){
        header_nodes.resize(header_lines.size());
        for(size_t i=0;i<header_lines.size();++i){header_nodes[i].format=MPV_FORMAT_STRING;header_nodes[i].u.string=header_lines[i].data();}
        mpv_node_list list{static_cast<int>(header_nodes.size()),header_nodes.data(),nullptr};
        mpv_node node{};node.format=MPV_FORMAT_NODE_ARRAY;node.u.list=&list;
        const int result=mpv_set_option(mpv,"http-header-fields",MPV_FORMAT_NODE,&node);
        if(result<0){cleanup();return "No se pudieron configurar las cabeceras de video.";}
    }
    if(!error.empty()){cleanup();return error;}
    int rc=mpv_initialize(mpv);
        if(rc<0){error=mpv_error_string(rc);cleanup();return error;}
    trace("play:mpv_initialized");
    mpv_request_log_messages(mpv,"v");
    mpv_render_param init[]={{MPV_RENDER_PARAM_API_TYPE,const_cast<char*>(MPV_RENDER_API_TYPE_SW)},{MPV_RENDER_PARAM_INVALID,nullptr}};
    rc=mpv_render_context_create(&context,mpv,init);
    if(rc<0){error=std::string("Render software: ")+mpv_error_string(rc);cleanup();return error;}
    mpv_render_context_set_update_callback(context,update,&dirty);
    int width=640,height=360,quality=std::clamp(options.quality,0,2);size_t stride=0;
    // El buffer nuevo se crea antes de liberar el anterior: si falta memoria se conserva la resolución actual.
    auto allocate_frame=[&](int q)->bool{
        static const int widths[]={640,854,960},heights[]={360,480,540};
        const int nw=widths[q],nh=heights[q];const size_t nstride=static_cast<size_t>(nw)*4;
        const size_t bytes=(nstride*static_cast<size_t>(nh)+63)&~static_cast<size_t>(63);
        void* np=std::aligned_alloc(64,bytes);
        SDL_Texture* nt=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ABGR8888,SDL_TEXTUREACCESS_STREAMING,nw,nh);
        if(!np||!nt){std::free(np);if(nt)SDL_DestroyTexture(nt);return false;}
        SDL_SetTextureBlendMode(nt,SDL_BLENDMODE_NONE);
        std::free(pixels);if(texture)SDL_DestroyTexture(texture);
        pixels=np;texture=nt;width=nw;height=nh;stride=nstride;return true;
    };
    if(!allocate_frame(quality)&&!(quality=0,allocate_frame(0))){error="No se pudo crear el buffer de video.";cleanup();return error;}
    const char* load[]={"loadfile",source.c_str(),nullptr};rc=mpv_command(mpv,load);
    if(rc<0){error=mpv_error_string(rc);cleanup();return error;}
    trace("play:file_requested");
#ifdef __SWITCH__
    appletSetMediaPlaybackState(true);
    PadState pad;padConfigureInput(1,HidNpadStyleSet_NpadStandard);padInitializeDefault(&pad);padUpdate(&pad);
#endif
    bool running=true,paused=false,first_frame=true,menu=false,loaded=false;
    int page=0,selection=0,aspect=0;double pending_start=options.start;
    int64_t external_id=-1,external_index=-1;
    std::string pref_service_sub=options.service_subtitle_pref;   // ultimo subtitulo del servicio elegido
    // Subtitulos automaticos: hilo propio para no frenar el video; el resultado se aplica desde este hilo.
    bool auto_enabled=options.auto_subtitles&&static_cast<bool>(options.auto_subtitle),auto_tried=false;
    std::thread auto_worker;std::atomic<bool> auto_cancel{false};std::atomic<int> auto_state{0};  // 0 libre, 1 buscando, 2 ok, 3 fallo
    std::mutex auto_mutex;std::string auto_label,auto_path,auto_message;
    std::string loaded_sub_file,pref_alang,pref_slang;  // archivo externo activo y preferencias de idioma elegidas
    const std::vector<std::string> qualities{"640x360 (rápida)","854x480 (equilibrada)","960x540 (nítida)"};
#ifdef __SWITCH__
    const std::string browse_root="sdmc:/",browse_pref="sdmc:/switch/xuperswitch/subtitulos";
#else
    const std::string browse_root=".",browse_pref="./subtitulos";
#endif
    static std::string last_browse;  // recuerda la ultima carpeta durante la sesion
    std::string browse_path=browse_root;std::vector<BrowseItem> browse_items;
    const std::vector<double> speeds{0.5,0.75,1,1.25,1.5,1.75,2};
    const std::vector<std::string> ratios{"Original","16:9","4:3","Rellenar"};
    PlaybackOutcome result;bool auto_next=options.auto_next;std::string notice,media_hint;
    Uint32 visible_until=SDL_GetTicks()+5000,last_draw=0;
    auto property=[&](const char* name){double value=0;mpv_get_property(mpv,name,MPV_FORMAT_DOUBLE,&value);return value;};
    auto flag=[&](const char* name){int value=0;mpv_get_property(mpv,name,MPV_FORMAT_FLAG,&value);return value!=0;};
    auto set_number=[&](const char* name,double value){if(mpv_set_property(mpv,name,MPV_FORMAT_DOUBLE,&value)<0)notice="Esta opcion no esta disponible en este video.";};
    auto command=[&](const char* name,const std::string& value,const char* mode=nullptr){const char* args[]={name,value.c_str(),mode,nullptr};
        if(mpv_command(mpv,args)<0)notice="No se pudo aplicar esa opcion al video.";};
    auto show=[&]{visible_until=SDL_GetTicks()+4000;};
    auto media_info=[&]{MediaInfo m;m.fps=property("container-fps");m.duration=property("duration");m.height=static_cast<int>(property("height"));
        if(!std::isfinite(m.fps)||m.fps<0)m.fps=0;
        if(!std::isfinite(m.duration)||m.duration<0)m.duration=0;
        return m;};
    std::vector<std::string> online_labels;
    auto browse_refresh=[&]{bool ok=true;browse_items=list_subtitle_dir(browse_path,&ok);
        if(!ok){notice="No se pudo leer esa carpeta.";browse_path=browse_root;browse_items=list_subtitle_dir(browse_path);}
        last_browse=browse_path;};
    auto seek_to=[&](double amount){if(!flag("seekable")){notice="Esta emisión no permite adelantar o retroceder.";show();return;}
        command("seek",std::to_string(amount),"relative+exact");show();};
    auto toggle_pause=[&]{int value=!flag("pause");
        if(mpv_set_property(mpv,"pause",MPV_FORMAT_FLAG,&value)<0)notice="No se pudo pausar.";
        show();};
    struct Track {int64_t id;std::string type,label;bool selected;bool external;std::string lang;};
    auto tracks=[&]{std::vector<Track> rows;mpv_node node{};
        if(mpv_get_property(mpv,"track-list",MPV_FORMAT_NODE,&node)>=0){
            if(node.format==MPV_FORMAT_NODE_ARRAY)for(int i=0;i<node.u.list->num;++i){const auto& n=node.u.list->values[i];
        if(n.format!=MPV_FORMAT_NODE_MAP)continue;
                Track t{0,"","",false,false,""};std::string lang,title,codec;
                for(int j=0;j<n.u.list->num;++j){const std::string key=n.u.list->keys[j];const auto& v=n.u.list->values[j];
                    if(key=="id"&&v.format==MPV_FORMAT_INT64)t.id=v.u.int64;
                    if(key=="selected"&&v.format==MPV_FORMAT_FLAG)t.selected=v.u.flag;
                    if(key=="external"&&v.format==MPV_FORMAT_FLAG)t.external=v.u.flag;
                    if(v.format==MPV_FORMAT_STRING){const std::string value=v.u.string;
                        if(key=="type")t.type=value;
        if(key=="lang")lang=value;
        if(key=="title")title=value;
        if(key=="codec")codec=value;}
                }
                t.lang=lang;
                t.label=(lang.empty()?"Pista "+std::to_string(t.id):lang)+(title.empty()?"":" - "+title)+(codec.empty()?"":" ["+codec+"]");
                if(t.label.size()>120)t.label.resize(120);
                rows.push_back(std::move(t));
            }
            mpv_free_node_contents(&node);
        }return rows;};
    // Recuerda el idioma de la pista elegida por el usuario (no el de archivos externos).
    auto note_track=[&](const std::string& type,int64_t id){
        if(type=="sub")pref_service_sub.clear();
        if(id<0){if(type=="sub")pref_slang="none";return;}
        for(const auto& t:tracks())if(t.id==id&&t.type==type){
            if(t.lang.empty()||!lang_ok(t.lang))return;
            if(type=="audio")pref_alang=t.lang;else if(!t.external)pref_slang=t.lang;
            return;}
    };
    auto start_auto=[&](bool forced){
        if(!auto_enabled||auto_state.load()==1||!options.auto_subtitle)return;
        if(!forced){
            if(auto_tried||!loaded_sub_file.empty()||(!pref_service_sub.empty()&&external_index>=0))return;
            // Ya hay una pista en el idioma preferido dentro del video: no hace falta buscar.
            for(const auto& t:tracks())if(t.type=="sub"&&!options.auto_skip_lang.empty()&&lang_matches(t.lang,options.auto_skip_lang))return;
        }
        auto_tried=true;
        if(auto_worker.joinable())auto_worker.join();
        auto_cancel=false;auto_state=1;notice="Buscando subtítulos automáticamente...";show();
        const MediaInfo info=media_info();auto fn=options.auto_subtitle;
        auto_worker=std::thread([&,info,fn]{std::string label,path,message;const bool ok=fn(info,auto_cancel,label,path,message);
            {std::lock_guard<std::mutex> lock(auto_mutex);auto_label=label;auto_path=path;auto_message=message;}
            auto_state=ok&&!path.empty()?2:3;});
    };
    auto keep_file_only_if=[&](int64_t chosen){if(chosen<0||chosen!=external_id||external_index>=0)loaded_sub_file.clear();};
    auto cycle_track=[&](const std::string& type){auto rows=tracks();std::vector<Track> choices;
        if(type=="sub")choices.push_back({-1,"sub","Desactivados",!flag("sub-visibility"),false,""});
        for(const auto& t:rows)if(t.type==type)choices.push_back(t);
        if(choices.empty()){notice="No hay pistas alternativas en este video.";show();return;}
        size_t selected=0;for(size_t i=0;i<choices.size();++i)if(choices[i].selected)selected=i;
        const auto& next=choices[(selected+1)%choices.size()];
        mpv_set_property_string(mpv,type=="audio"?"aid":"sid",next.id<0?"no":std::to_string(next.id).c_str());
        if(type=="sub"){int on=next.id>=0;mpv_set_property(mpv,"sub-visibility",MPV_FORMAT_FLAG,&on);keep_file_only_if(next.id);}
        note_track(type,next.id);
        notice=next.label;show();};
    auto cycle_speed=[&](int direction){double speed=property("speed");size_t i=0;for(size_t k=0;k<speeds.size();++k)if(std::abs(speeds[k]-speed)<0.01)i=k;
        set_number("speed",speeds[(i+speeds.size()+direction)%speeds.size()]);show();};
    auto cycle_aspect=[&]{aspect=(aspect+1)%4;set_number("video-aspect-override",aspect==1?16.0/9:aspect==2?4.0/3:-1);
        set_number("panscan",aspect==3?1:0);notice="Pantalla: "+ratios[aspect];show();};
    auto request_item=[&](int index){if(index>=0&&static_cast<size_t>(index)<options.items.size()){result.next_item=index;running=false;}};
    auto draw_text=[&](int x,int y,const std::string& message,SDL_Color color,int width=1150){if(!options.font||message.empty())return;
        SDL_Surface* surface=TTF_RenderUTF8_Blended_Wrapped(options.font,message.c_str(),color,width);
        if(!surface)return;
        SDL_Texture* label=SDL_CreateTextureFromSurface(renderer,surface);SDL_Rect dst{x,y,surface->w,surface->h};
        if(label){SDL_RenderCopy(renderer,label,nullptr,&dst);SDL_DestroyTexture(label);}SDL_FreeSurface(surface);};
    auto load_service_sub=[&](size_t index,bool splash)->bool{
        if(index>=options.subtitles.size()||!options.subtitle_loader)return false;
        if(splash){SDL_SetRenderDrawColor(renderer,10,17,28,255);SDL_RenderClear(renderer);SDL_RenderCopy(renderer,texture,nullptr,nullptr);
            SDL_SetRenderDrawColor(renderer,10,17,28,255);SDL_Rect loading{20,550,1240,140};SDL_RenderFillRect(renderer,&loading);
            draw_text(48,580,"Cargando subtítulos...",{238,243,250,255});SDL_RenderPresent(renderer);}
        std::string data,subtitle_error;
        if(!options.subtitle_loader(index,data,subtitle_error)){notice=subtitle_error.empty()?"No se pudo descargar el subtítulo.":subtitle_error;return false;}
        if(external_id>=0){command("sub-remove",std::to_string(external_id));external_id=-1;}
        loaded_sub_file.clear();
        std::ofstream file(subtitle_cache,std::ios::binary|std::ios::trunc);file.write(data.data(),static_cast<std::streamsize>(data.size()));file.close();
        if(!file){notice="No se pudo guardar el subtítulo temporal.";return false;}
        subtitle_written=true;
        const char* args[]={"sub-add",subtitle_cache,"select",options.subtitles[index].c_str(),nullptr};
        if(mpv_command(mpv,args)<0){notice="Formato de subtítulos no compatible.";return false;}
        external_index=static_cast<int64_t>(index);mpv_get_property(mpv,"sid",MPV_FORMAT_INT64,&external_id);
        int on=1;mpv_set_property(mpv,"sub-visibility",MPV_FORMAT_FLAG,&on);pref_service_sub=options.subtitles[index];return true;
    };
    auto clock=[](double value){int n=std::max(0,static_cast<int>(value));char b[32];std::snprintf(b,sizeof(b),"%02d:%02d:%02d",n/3600,n/60%60,n%60);return std::string(b);};
    struct Row {std::string label;int action;int64_t value=0;};
    auto menu_rows=[&]{std::vector<Row> rows;
        if(page==0){rows={{flag("pause")?"Continuar":"Pausar",1},{"Audio / idioma",2},{"Subtítulos",3},{"Calidad / fuente",4},
            {"Episodios / canales",5},{"Velocidad: "+std::to_string(property("speed")).substr(0,4)+"x",6},
            {"Pantalla: "+ratios[aspect],7},{"Volumen: "+std::to_string(static_cast<int>(property("volume"))),8},
            {flag("mute")?"Activar sonido":"Silenciar",9},{"Sincronía audio: "+std::to_string(property("audio-delay")).substr(0,5)+" s",10},
            {"Sincronía subtítulos: "+std::to_string(property("sub-delay")).substr(0,5)+" s",11},
            {"Tamaño subtítulos: "+std::to_string(property("sub-scale")).substr(0,4),12},{"Brillo: "+std::to_string(static_cast<int>(property("brightness"))),13},
            {"Retroceder 60 segundos",14},{"Adelantar 60 segundos",15},{"Reiniciar video",16},{"Información del video",17},{std::string("Siguiente episodio automático: ")+(auto_next?"Si":"No"),24},{"Calidad de imagen: "+qualities[quality],28},{"Cerrar reproductor",18}};}
        if(page==1||page==2){if(page==2){rows.push_back({"Desactivar subtítulos",20,-1});rows.push_back({"Buscar archivo en la consola (SD)...",27});if(options.online_search)rows.push_back({"Buscar subtítulos en internet...",29});if(options.auto_subtitle)rows.push_back({std::string("Subtítulos automáticos: ")+(auto_enabled?"Sí":"No"),31});}for(const auto& t:tracks())if(t.type==(page==1?"audio":"sub"))rows.push_back({(t.selected?"* ":"  ")+t.label,page==1?19:20,t.id});}
        if(page==2)for(size_t i=0;i<options.subtitles.size();++i)rows.push_back({"Servicio: "+options.subtitles[i],23,static_cast<int64_t>(i)});
        if(page==5){
            if(browse_path!=browse_root)rows.push_back({"[..]  Subir una carpeta",25,-1});
            for(size_t i=0;i<browse_items.size();++i)rows.push_back({(browse_items[i].dir?"[CARPETA] ":"          ")+clip_utf8(browse_items[i].name,90),browse_items[i].dir?25:26,static_cast<int64_t>(i)});
            if(browse_items.empty())rows.push_back({"(no hay carpetas ni subtítulos .srt/.ass/.vtt aquí)",0});}
        if(page==6)for(size_t i=0;i<online_labels.size();++i)rows.push_back({clip_utf8(online_labels[i],100),30,static_cast<int64_t>(i)});
        if(page==3)for(size_t i=0;i<options.variants.size();++i)rows.push_back({(static_cast<int>(i)==options.variant?"* ":"  ")+options.variants[i],21,static_cast<int64_t>(i)});
        if(page==4)for(size_t i=0;i<options.items.size();++i)rows.push_back({(static_cast<int>(i)==options.item?"* ":"  ")+options.items[i],22,static_cast<int64_t>(i)});
        if(rows.empty())rows.push_back({"No hay opciones disponibles para este video",0});
        return rows;};
    while(running){
#ifdef __SWITCH__
        if(!appletMainLoop())break;
#endif
        bool activate=false,back=false,settings=false,audio=false,subs=false,slower=false,faster=false,ratio=false,mute=false;
        int horizontal=0,vertical=0,jump=0,item_delta=0;bool tap=false;int tap_x=0,tap_y=0;SDL_Event event;
        while(SDL_PollEvent(&event)){
            if(event.type==SDL_QUIT)running=false;
            // Toque (Switch) o clic de raton (PC); los clics sinteticos generados por un toque se ignoran.
            if(event.type==SDL_FINGERDOWN){tap=true;tap_x=static_cast<int>(event.tfinger.x*1280);tap_y=static_cast<int>(event.tfinger.y*720);}
            if(event.type==SDL_MOUSEBUTTONDOWN&&event.button.which!=SDL_TOUCH_MOUSEID){tap=true;tap_x=event.button.x;tap_y=event.button.y;}
            if(event.type==SDL_KEYDOWN&&!event.key.repeat)switch(event.key.keysym.sym){
                case SDLK_ESCAPE:back=true;break;case SDLK_SPACE:activate=true;break;case SDLK_RETURN:activate=true;break;
                case SDLK_x:case SDLK_TAB:settings=true;break;case SDLK_y:audio=true;break;case SDLK_s:subs=true;break;
                case SDLK_LEFT:horizontal-=1;break;case SDLK_RIGHT:horizontal+=1;break;case SDLK_UP:vertical-=1;break;case SDLK_DOWN:vertical+=1;break;
                case SDLK_PAGEUP:item_delta=-1;break;case SDLK_PAGEDOWN:item_delta=1;break;
                case SDLK_LEFTBRACKET:slower=true;break;case SDLK_RIGHTBRACKET:faster=true;break;case SDLK_f:ratio=true;break;case SDLK_m:mute=true;break;
                case SDLK_j:jump=-60;break;case SDLK_l:jump=60;break;
            }
        }
#ifdef __SWITCH__
        padUpdate(&pad);u64 down=padGetButtonsDown(&pad);
        back|=down&HidNpadButton_B;activate|=down&HidNpadButton_A;settings|=down&(HidNpadButton_X|HidNpadButton_Plus);audio|=down&HidNpadButton_Y;
        if(down&HidNpadButton_Left)horizontal=-1;
        if(down&HidNpadButton_Right)horizontal=1;
        if(down&HidNpadButton_Up)vertical=-1;
        if(down&HidNpadButton_Down)vertical=1;
        if(down&HidNpadButton_ZL)jump=-60;
        if(down&HidNpadButton_ZR)jump=60;
        if(down&HidNpadButton_L)item_delta=-1;
        if(down&HidNpadButton_R)item_delta=1;
#endif
        if(tap){
            const double total=property("duration");
            if(total>0&&tap_y>=595&&tap_y<=665&&tap_x>=300&&tap_x<=1230){  // barra de progreso: saltar
                if(flag("seekable")){command("seek",std::to_string(std::clamp((tap_x-330)/870.0,0.0,1.0)*total),"absolute+exact");notice.clear();}
                else notice="Esta emisión no permite adelantar o retroceder.";
                show();
            }else if(menu){
                const int first=selection/10*10;
                if(tap_y>=128&&tap_y<540&&tap_x>=35&&tap_x<=1242){
                    const int index=first+(tap_y-130)/41;
                    if(index>=0&&index<static_cast<int>(menu_rows().size())){selection=index;activate=true;}
                }else if(tap_y<112)back=true;
            }else{
                if(tap_y<360)settings=true;            // mitad superior: abrir menu
                else if(SDL_GetTicks()<visible_until)activate=true;  // controles visibles: pausa
                else show();
            }
        }
        if(back){if(menu){
            if(page==5){if(browse_path!=browse_root){browse_path=parent_path(browse_path,browse_root);browse_refresh();selection=0;}else{page=2;selection=0;}}
            else if(page==6){page=2;selection=0;}
            else if(page){page=0;selection=0;}else menu=false;}else running=false;show();}
        if(settings){menu=!menu;page=0;selection=0;notice.clear();show();}
        if(audio)cycle_track("audio");
        if(subs)cycle_track("sub");
        if(slower)cycle_speed(-1);
        if(faster)cycle_speed(1);
        if(ratio)cycle_aspect();
        if(mute){int value=!flag("mute");mpv_set_property(mpv,"mute",MPV_FORMAT_FLAG,&value);show();}
        if(jump)seek_to(jump);
        if(item_delta&&!(menu&&page==5))request_item(options.item+item_delta);
        if(menu){auto rows=menu_rows();selection=std::clamp(selection+vertical+(page==5?item_delta*10:0),0,static_cast<int>(rows.size())-1);
            const auto row=rows[selection];
            if(activate||(horizontal&&page!=5)){const int direction=horizontal?horizontal:1;
                switch(row.action){
                    case 1:toggle_pause();break;case 2:page=1;selection=0;break;case 3:page=2;selection=0;break;
                    case 4:page=3;selection=0;break;case 5:page=4;selection=0;break;case 6:cycle_speed(direction);break;case 7:cycle_aspect();break;
                    case 8:set_number("volume",std::clamp(property("volume")+direction*5,0.0,100.0));break;
                    case 9:{int value=!flag("mute");mpv_set_property(mpv,"mute",MPV_FORMAT_FLAG,&value);break;}
                    case 10:set_number("audio-delay",property("audio-delay")+direction*0.1);break;
                    case 11:set_number("sub-delay",property("sub-delay")+direction*0.25);break;
                    case 12:set_number("sub-scale",std::clamp(property("sub-scale")+direction*0.1,0.5,3.0));break;
                    case 13:set_number("brightness",std::clamp(property("brightness")+direction*5,-100.0,100.0));break;
                    case 14:seek_to(-60);break;case 15:seek_to(60);break;
                    case 16:if(flag("seekable"))command("seek","0","absolute+exact");else notice="Esta emisión no permite reiniciar.";break;
                    case 17:{char* format=mpv_get_property_string(mpv,"video-format");notice=(format?std::string(format):"Video")+" | "+std::to_string(static_cast<int>(property("width")))+"x"+std::to_string(static_cast<int>(property("height")))+" | "+std::to_string(property("container-fps")).substr(0,5)+" fps";mpv_free(format);break;}
                    case 18:running=false;break;
                    case 19:mpv_set_property_string(mpv,"aid",std::to_string(row.value).c_str());note_track("audio",row.value);notice="Audio seleccionado";page=0;selection=1;break;
                    case 20:{mpv_set_property_string(mpv,"sid",row.value<0?"no":std::to_string(row.value).c_str());int value=row.value>=0;mpv_set_property(mpv,"sub-visibility",MPV_FORMAT_FLAG,&value);keep_file_only_if(row.value);note_track("sub",row.value);notice="Subtítulos actualizados";page=0;selection=2;break;}
                    case 21:if(row.value!=options.variant){result.next_variant=static_cast<int>(row.value);running=false;}break;
                    case 22:if(row.value!=options.item)request_item(static_cast<int>(row.value));break;
                    case 24:auto_next=!auto_next;break;
                    case 28:{const int next_quality=(quality+1)%3;
                        bool ok=allocate_frame(next_quality),trimmed=false;
                        if(!ok){   // sin memoria: se libera el buffer de red (lo ya visto y casi todo lo adelantado) y se reintenta
                            mpv_set_property_string(mpv,"demuxer-max-back-bytes","0");mpv_set_property_string(mpv,"demuxer-max-bytes","8388608");
                            trimmed=true;ok=allocate_frame(next_quality);}
                        if(ok){quality=next_quality;dirty.store(true);notice="Imagen: "+qualities[quality]+(trimmed?" (se redujo el buffer de red)":"");}
                        else notice="No hay memoria suficiente para esa resolución; se redujo el buffer de red. Prueba otra vez.";
                        break;}
                    case 27:{browse_path=last_browse.empty()?browse_root:last_browse;
                        if(last_browse.empty()){bool ok=false;list_subtitle_dir(browse_pref,&ok);if(ok)browse_path=browse_pref;}
                        // Si la serie ya uso un archivo, abre su carpeta con el cursor en el siguiente.
                        std::string hint_name;
                        const size_t cut=options.subtitle_hint.find_last_of('/');
                        if(cut!=std::string::npos&&cut>0){bool ok=false;const std::string dir=options.subtitle_hint.substr(0,cut);list_subtitle_dir(dir,&ok);
                            if(ok){browse_path=dir;hint_name=options.subtitle_hint.substr(cut+1);}}
                        browse_refresh();page=5;selection=0;
                        if(!hint_name.empty()){const int offset=browse_path!=browse_root?1:0;
                            for(size_t i=0;i<browse_items.size();++i)if(!browse_items[i].dir&&browse_items[i].name==hint_name){
                                size_t next=i+1;while(next<browse_items.size()&&browse_items[next].dir)++next;
                                selection=offset+static_cast<int>(next<browse_items.size()?next:i);break;}}
                        break;}
                    case 25:{if(row.value<0)browse_path=parent_path(browse_path,browse_root);
                        else if(static_cast<size_t>(row.value)<browse_items.size())browse_path=join_path(browse_path,browse_items[static_cast<size_t>(row.value)].name);
                        else break;
                        browse_refresh();selection=0;break;}
                    case 26:{if(row.value<0||static_cast<size_t>(row.value)>=browse_items.size())break;
                        const std::string name=browse_items[static_cast<size_t>(row.value)].name,full=join_path(browse_path,name);
                        if(external_id>=0){command("sub-remove",std::to_string(external_id));external_id=-1;}
                        external_index=-1;pref_service_sub.clear();
                        // Mejor esfuerzo: muchos .srt en espanol vienen en Latin-1; UTF-8 sigue teniendo prioridad.
                        mpv_set_property_string(mpv,"sub-codepage","utf8:cp1252");
                        const char* args[]={"sub-add",full.c_str(),"select",name.c_str(),nullptr};
                        if(mpv_command(mpv,args)<0)notice="No se pudo cargar ese archivo de subtítulos.";
                        else{mpv_get_property(mpv,"sid",MPV_FORMAT_INT64,&external_id);int on=1;mpv_set_property(mpv,"sub-visibility",MPV_FORMAT_FLAG,&on);
                            loaded_sub_file=full;notice="Subtítulo cargado: "+clip_utf8(name,60);page=0;selection=2;}
                        break;}
                    case 31:{auto_enabled=!auto_enabled;
                        if(auto_enabled){notice="Subtítulos automáticos: activados";auto_tried=false;
                            if(loaded_sub_file.empty()){page=0;selection=2;start_auto(true);}}
                        else{notice="Subtítulos automáticos: desactivados";if(auto_state.load()==1)auto_cancel=true;}
                        break;}
                    case 29:case 30:{
                        auto busy=[&](const char* text){SDL_SetRenderDrawColor(renderer,10,17,28,255);SDL_RenderClear(renderer);SDL_RenderCopy(renderer,texture,nullptr,nullptr);
                            SDL_SetRenderDrawColor(renderer,10,17,28,255);SDL_Rect loading{20,550,1240,140};SDL_RenderFillRect(renderer,&loading);
                            draw_text(48,580,text,{238,243,250,255});SDL_RenderPresent(renderer);};
                        std::string problem;
                        if(row.action==29){
                            busy("Buscando subtítulos en internet...");
                            online_labels.clear();
                            if(auto_state.load()==1){notice="Espera: ya se están buscando subtítulos automáticamente.";break;}
                            if(!options.online_search||!options.online_search(media_info(),online_labels,problem)||online_labels.empty()){online_labels.clear();notice=problem.empty()?"No se encontraron subtítulos.":problem;break;}
                            page=6;selection=0;break;}
                        if(!options.online_loader||row.value<0||static_cast<size_t>(row.value)>=online_labels.size())break;
                        busy("Descargando subtítulo...");
                        std::string data;problem.clear();
                        if(!options.online_loader(static_cast<size_t>(row.value),data,problem)){notice=problem.empty()?"No se pudo descargar el subtítulo.":problem;break;}
                        if(external_id>=0){command("sub-remove",std::to_string(external_id));external_id=-1;}
                        loaded_sub_file.clear();
                        // Al exito el cargador deja en "problem" la ruta donde guardo el archivo (si pudo).
                        std::string target=problem;
                        if(target.empty()){
                            std::ofstream file(subtitle_cache,std::ios::binary|std::ios::trunc);file.write(data.data(),static_cast<std::streamsize>(data.size()));file.close();
                            if(!file){notice="No se pudo guardar el subtítulo temporal.";break;}
                            subtitle_written=true;target=subtitle_cache;}
                        mpv_set_property_string(mpv,"sub-codepage","utf8:cp1252");
                        const std::string name=online_labels[static_cast<size_t>(row.value)];
                        const char* args[]={"sub-add",target.c_str(),"select",name.c_str(),nullptr};
                        if(mpv_command(mpv,args)<0)notice="Formato de subtítulos no compatible.";
                        else{external_index=-1;pref_service_sub.clear();mpv_get_property(mpv,"sid",MPV_FORMAT_INT64,&external_id);int on=1;mpv_set_property(mpv,"sub-visibility",MPV_FORMAT_FLAG,&on);
                            if(target!=subtitle_cache){loaded_sub_file=target;}
                            notice="Subtítulo cargado";page=0;selection=2;}
                        break;}
                    case 23:{
                        if(external_index==row.value&&external_id>=0){loaded_sub_file.clear();mpv_set_property_string(mpv,"sid",std::to_string(external_id).c_str());int on=1;mpv_set_property(mpv,"sub-visibility",MPV_FORMAT_FLAG,&on);pref_service_sub=options.subtitles[static_cast<size_t>(row.value)];notice="Subtítulo seleccionado";page=0;selection=2;break;}
                        if(load_service_sub(static_cast<size_t>(row.value),true)){notice="Subtítulo cargado";page=0;selection=2;}
                        break;}

                }show();
            }
        }else{if(activate)toggle_pause();
        if(horizontal)seek_to(horizontal*10);
        if(vertical){menu=true;page=0;selection=0;show();}}
        for(;;){
            mpv_event* e=mpv_wait_event(mpv,0);
        if(e->event_id==MPV_EVENT_NONE)break;
            if(e->event_id==MPV_EVENT_SHUTDOWN)running=false;
            if(e->event_id==MPV_EVENT_LOG_MESSAGE){
                const auto* log=static_cast<mpv_event_log_message*>(e->data);
                const std::string hint=log&&log->text?playback_error_hint(log->text):"";
                if(!hint.empty()&&hint!=media_hint&&(media_hint.empty()||hint.rfind("HTTP ",0)==0||media_hint.rfind("HTTP ",0)!=0)){
                    media_hint=hint;trace(("play:classified_error "+hint).c_str());
                }
            }
            if(e->event_id==MPV_EVENT_FILE_LOADED){loaded=true;
                if(!options.subtitle_file.empty()&&loaded_sub_file.empty()&&std::ifstream(options.subtitle_file).good()){
                    const size_t slash=options.subtitle_file.find_last_of('/');
                    const std::string base=slash==std::string::npos?options.subtitle_file:options.subtitle_file.substr(slash+1);
                    mpv_set_property_string(mpv,"sub-codepage","utf8:cp1252");
                    const char* args[]={"sub-add",options.subtitle_file.c_str(),"select",base.c_str(),nullptr};
                    if(mpv_command(mpv,args)>=0){external_index=-1;external_id=-1;mpv_get_property(mpv,"sid",MPV_FORMAT_INT64,&external_id);
                        int on=1;mpv_set_property(mpv,"sub-visibility",MPV_FORMAT_FLAG,&on);loaded_sub_file=options.subtitle_file;notice="Subtítulo cargado: "+clip_utf8(base,60);}
                }
                if(loaded_sub_file.empty()&&!pref_service_sub.empty()){
                    auto low=[](std::string v){for(char& c:v)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return v;};
                    for(size_t i=0;i<options.subtitles.size();++i)if(low(options.subtitles[i])==low(pref_service_sub)){
                        if(load_service_sub(i,false)){notice="Subtítulo cargado: "+clip_utf8(options.subtitles[i],60);}
                        break;}}
                start_auto(false);
        if(pending_start>0&&flag("seekable"))command("seek",std::to_string(pending_start),"absolute+exact");
                pending_start=0;}
            if(e->event_id==MPV_EVENT_END_FILE){trace("play:end_file");auto* end=static_cast<mpv_event_end_file*>(e->data);
                if(end&&end->reason==MPV_END_FILE_REASON_ERROR){
                    error=media_hint.empty()?mpv_error_string(end->error):"No se pudo abrir el video: "+media_hint;
                    char failure[80];std::snprintf(failure,sizeof(failure),"play:end_error mpv=%d",end->error);trace(failure);
                }
                result.ended=end&&end->reason==MPV_END_FILE_REASON_EOF;running=false;}
        }
        if(auto_state.load()>=2){
            if(auto_worker.joinable())auto_worker.join();
            std::string label,path,message;{std::lock_guard<std::mutex> lock(auto_mutex);label=auto_label;path=auto_path;message=auto_message;}
            const bool ok=auto_state.load()==2;auto_state=0;
            if(ok&&loaded_sub_file.empty()&&std::ifstream(path).good()){
                if(external_id>=0){command("sub-remove",std::to_string(external_id));external_id=-1;}
                const size_t slash=path.find_last_of('/');const std::string base=slash==std::string::npos?path:path.substr(slash+1);
                mpv_set_property_string(mpv,"sub-codepage","utf8:cp1252");
                const char* args[]={"sub-add",path.c_str(),"select",base.c_str(),nullptr};
                if(mpv_command(mpv,args)>=0){external_index=-1;external_id=-1;mpv_get_property(mpv,"sid",MPV_FORMAT_INT64,&external_id);
                    int on=1;mpv_set_property(mpv,"sub-visibility",MPV_FORMAT_FLAG,&on);loaded_sub_file=path;notice="Subtítulo automático: "+clip_utf8(label.empty()?base:label,70);}
                else notice="No se pudo cargar el subtítulo automático.";
            }else notice=message.empty()?"No se encontraron subtítulos automáticos. Puedes buscarlos a mano en el menú.":clip_utf8(message,110);
            show();}
        if(!running)break;
        bool fresh=dirty.exchange(false);
        if(fresh){int size[2]={width,height};mpv_render_param frame[]={{MPV_RENDER_PARAM_SW_SIZE,size},{MPV_RENDER_PARAM_SW_FORMAT,const_cast<char*>("rgb0")},
            {MPV_RENDER_PARAM_SW_STRIDE,&stride},{MPV_RENDER_PARAM_SW_POINTER,pixels},{MPV_RENDER_PARAM_INVALID,nullptr}};
            rc=mpv_render_context_render(context,frame);
        if(rc<0){error=mpv_error_string(rc);break;}
            if(SDL_UpdateTexture(texture,nullptr,pixels,static_cast<int>(stride))<0){error=SDL_GetError();break;}
            if(first_frame&&loaded){trace("play:first_frame");first_frame=false;}
        }
        const Uint32 now=SDL_GetTicks();paused=flag("pause");
        if(fresh||now-last_draw>=100){last_draw=now;SDL_SetRenderDrawColor(renderer,0,0,0,255);SDL_RenderClear(renderer);SDL_RenderCopy(renderer,texture,nullptr,nullptr);
            if(options.font&&(menu||paused||now<visible_until)){
                SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);SDL_SetRenderDrawColor(renderer,10,17,28,230);
                SDL_Rect panel{20,menu?35:526,1240,menu?650:175};SDL_RenderFillRect(renderer,&panel);
                const SDL_Color white{238,243,250,255},cyan{73,219,223,255},muted{180,192,209,255};
                if(menu){draw_text(48,48,options.title.empty()?"Reproductor":options.title,white);
                    draw_text(48,84,page==0?"A: elegir | izq/der: ajustar | B: volver":page==5?"SD: "+clip_utf8(browse_path,70)+" | A: abrir | B: subir | L/R: +-10":page==6?"A: descargar y usar | B: volver":"A: seleccionar | B: ajustes",muted);
                    auto rows=menu_rows();const int start=selection/10*10;
                    for(int i=start;i<static_cast<int>(rows.size())&&i<start+10;++i){const int y=130+(i-start)*41;
                        if(i==selection){SDL_SetRenderDrawColor(renderer,28,63,78,255);SDL_Rect highlight{35,y-2,1207,39};SDL_RenderFillRect(renderer,&highlight);}
                        draw_text(50,y,(i==selection?"> ":"  ")+rows[i].label,i==selection?cyan:white,1150);}
                    draw_text(48,650,"Opciones "+std::to_string(start+1)+"-"+std::to_string(std::min(start+10,static_cast<int>(rows.size())))+" / "+std::to_string(rows.size())+"  |  Arriba/abajo: navegar",muted);
                    draw_text(48,560,notice.empty()?"L/R: anterior/siguiente | Y: cambiar audio":notice,cyan,1150);
                }else{draw_text(44,539,(paused?"PAUSA  ":"")+options.title,white);draw_text(44,578,"A: pausa | X/+: opciones | Y: audio | izq/der: 10 s | ZL/ZR: 60 s | B: salir",muted);}
                const double pos=property("time-pos"),duration=property("duration");
                draw_text(48,menu?606:618,clock(pos)+(duration>0?" / "+clock(duration):" / EN VIVO"),white);
                SDL_SetRenderDrawColor(renderer,65,79,94,255);SDL_Rect bar{330,menu?619:631,870,8};SDL_RenderFillRect(renderer,&bar);
                if(duration>0){bar.w=static_cast<int>(870*std::clamp(pos/duration,0.0,1.0));SDL_SetRenderDrawColor(renderer,73,219,223,255);SDL_RenderFillRect(renderer,&bar);}
                if(!menu)draw_text(44,660,notice.empty()?"Toque: arriba=menú | centro=pausa | barra=saltar":notice,notice.empty()?muted:cyan);
                SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);
            }SDL_RenderPresent(renderer);
        }else SDL_Delay(5);
    }
    auto_cancel=true;if(auto_worker.joinable())auto_worker.join();
    result.auto_next=auto_next;result.auto_subtitles=auto_enabled;result.service_subtitle=pref_service_sub;
    // A classified network message can arrive after END_FILE in the same queue drain.
    if(!error.empty()&&!media_hint.empty())error="No se pudo abrir el video: "+media_hint;
    result.position=property("time-pos");result.speed=property("speed");result.volume=property("volume");result.paused=flag("pause");
    result.quality=quality;result.alang=pref_alang;result.slang=pref_slang;result.subtitle_file=loaded_sub_file;
    result.audio_delay=property("audio-delay");result.sub_delay=property("sub-delay");result.sub_scale=property("sub-scale");result.aspect=property("video-aspect-override");
    mpv_get_property(mpv,"aid",MPV_FORMAT_INT64,&result.audio);mpv_get_property(mpv,"sid",MPV_FORMAT_INT64,&result.subtitle);
    if(outcome)*outcome=result;
    cleanup();return error;
}
}
