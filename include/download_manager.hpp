#pragma once
// Cola de descargas en segundo plano: pide la fuente de video al servicio, baja el archivo
// y los subtitulos a la SD y los registra en la biblioteca offline. Una descarga a la vez.
#include "catalog.hpp"
#include "offline_library.hpp"
#include "service.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace xs {
// Piden una fuente nueva (URL firmada) y un subtitulo; las implementa main con la sesion actual.
using SourceResolver=std::function<SourceResult(const Entry&)>;
using SubtitleFetcher=std::function<bool(const SubtitleSource&,std::string& data,std::string& error)>;
enum class EnqueueResult {Added,Retrying,AlreadyQueued,AlreadyDownloaded,NotSupported,NoSession,QueueFull};
class DownloadManager {
public:
    struct Config {
        std::string directory;     // carpeta de videos (se crea si falta)
        std::string library_path;  // JSON de la biblioteca
        std::string ca_file;       // paquete CA para TLS
        int max_attempts=4;
        int retry_delay_ms=2000;
        uint64_t free_margin=256ull<<20;
    };
    explicit DownloadManager(Config config);
    ~DownloadManager();
    DownloadManager(const DownloadManager&)=delete;DownloadManager& operator=(const DownloadManager&)=delete;
    // Se llama tras iniciar sesion. Con funciones vacias, las descargas nuevas se rechazan.
    void set_resolver(SourceResolver resolver,SubtitleFetcher fetcher);
    // `show` es el nombre de la serie o pelicula (titulo del catalogo abierto); `order` su posicion en la lista.
    EnqueueResult enqueue(const Entry& episode,const std::string& show,int order);
    // Cancela una descarga en curso, en cola o fallida (borra lo descargado a medias).
    bool cancel(const std::string& content_id);
    // Borra un contenido ya descargado de la SD.
    bool remove_downloaded(const std::string& content_id);
    // Pausa/reanuda las descargas (se usa mientras se reproduce un video).
    void set_paused(bool paused);
    bool downloaded(const std::string& content_id)const;
    bool find_downloaded(const std::string& content_id,OfflineItem& out)const;
    // Texto corto de estado para una tarjeta: "DESCARGADO 1.4 GB", "DESCARGANDO 37%", "EN COLA", "ERROR..." o "".
    std::string status_text(const std::string& content_id)const;
    // Pantalla "Descargas": primero las descargas en curso/fallidas (source vacio), luego lo guardado.
    std::vector<Entry> entries()const;
    size_t downloaded_count()const;
    // Resumen para la barra superior; vacio si no hay nada pendiente.
    std::string overlay()const;
    bool busy()const;   // hay una descarga activa o en cola (aunque este en pausa)
    // Mensaje de un evento terminado (completa, fallida...) o cadena vacia.
    std::string take_event();
    // Descarta del registro lo que ya no esta en la SD.
    void prune();
private:
    struct Job {
        Entry entry;
        std::string title,base,error;
        int order=0;
        enum class State {Queued,Active,Failed} state=State::Queued;
        uint64_t done=0,total=0;
    };
    enum class Outcome {Done,Failed,Cancelled,Paused};
    Config config;
    mutable std::mutex mutex;
    std::condition_variable wake;
    OfflineLibrary library;
    std::vector<Job> jobs;
    SourceResolver resolver;SubtitleFetcher fetcher;
    std::vector<std::string> events;
    std::string active_id;
    std::atomic<bool> stop{false},paused{false},cancel_active{false};
    std::atomic<uint64_t> active_done{0},active_total{0};
    size_t batch_done=0,batch_total=0;
    std::thread worker;
    void run();
    Outcome process(const Job& job,std::string& message);
    void sleep_interruptible(int milliseconds);
};
}
