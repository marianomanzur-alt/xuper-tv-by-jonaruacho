// Arranque sin conexion con una descarga ya guardada: abre Descargas, reproduce el archivo
// local con libmpv (ao=null), comprueba "Continuar viendo" y borra la descarga con doble ZR.
#define main xuperswitch_app_main
#include "../source/main.cpp"
#undef main
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
static void key(SDL_Keycode key){SDL_Event e{};e.type=SDL_KEYDOWN;e.key.keysym.sym=key;SDL_PushEvent(&e);SDL_Delay(200);}
static std::string file_text(const char* path){std::ifstream f(path);return std::string(std::istreambuf_iterator<char>(f),{});}
template<class Predicate>static void until(Predicate test,int seconds=40){const auto start=std::chrono::steady_clock::now();while(!test()){assert(std::chrono::steady_clock::now()-start<std::chrono::seconds(seconds));SDL_Delay(25);}SDL_Delay(200);}
int main(int argc,char** argv){
    std::thread input([]{
        until([]{return file_text("diagnostico.log").find("bootstrap:failed")!=std::string::npos;});  // sin service.json: modo sin conexion
        key(SDLK_RETURN);                                    // "Descargas"
        key(SDLK_RETURN);                                    // reproduce el unico archivo
        until([]{return file_text("diagnostico.log").find("main:video_returned")!=std::string::npos;});
        until([]{return xs::SavedCatalog("library_state.json").recent.size()==1;});   // lo visto desde la SD cuenta en Continuar viendo
        key(SDLK_g);key(SDLK_g);                             // ZR dos veces: confirma y borra
        until([]{return xs::OfflineLibrary("offline_library.json",downloads_directory()).items.empty();});
        key(SDLK_ESCAPE);
    });
    const auto result=xuperswitch_app_main(argc,argv);input.join();assert(result==0);
    assert(file_text("diagnostico.log").find("main:exit")!=std::string::npos);
    std::cout<<"App sin conexion: inicio offline, Descargas, reproduccion desde la SD, Continuar viendo, borrado con doble ZR y cierre limpio\n";
}
