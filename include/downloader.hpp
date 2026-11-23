#pragma once
// Descarga de un archivo de video por HTTP(S) a la tarjeta SD, con reanudacion.
// Solo transporte: no conoce el servicio ni sus sesiones. La URL y las cabeceras
// llegan ya resueltas; nada de eso se escribe en disco ni en los registros.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
namespace xs {
enum class DownloadStatus {
    Ok,         // archivo completo e instalado en final_path
    Cancelled,  // el usuario cancelo (el .part se conserva; quien llama decide si lo borra)
    Paused,     // interrumpida a proposito para reanudar despues
    Hls,        // la fuente es una lista .m3u8: no es un archivo unico
    Invalid,    // la respuesta no parece video (HTML/JSON de error, etc.)
    Space,      // no hay espacio libre suficiente en la SD
    Write,      // fallo al escribir en la SD
    Network,    // corte o error de red: se puede reintentar y reanuda
    Http        // el servidor respondio con error (4xx/5xx)
};
struct DownloadRequest {
    std::string url;
    std::string part_path;   // archivo temporal; si existe, se reanuda desde su tamano
    std::string final_path;  // destino definitivo (se reemplaza si ya existia)
    std::string ca_file;     // paquete CA para verificar TLS (vacio: el del sistema)
    std::vector<std::pair<std::string,std::string>> headers;
    uint64_t free_margin=256ull<<20;  // espacio que debe sobrar tras la descarga
};
struct DownloadResult {
    DownloadStatus status=DownloadStatus::Network;
    long http=0;
    int curl=0;
    uint64_t bytes=0;        // tamano final (o actual) del archivo
    bool retry=false;        // vale la pena reintentar
    bool refresh_url=false;  // la URL probablemente caduco: pedir una nueva antes de reintentar
    std::string message;     // texto corto para mostrar al usuario; nunca incluye URLs
};
using DownloadProgress=std::function<void(uint64_t done,uint64_t total)>;
// Bloquea hasta terminar. `cancel` y `pause` se consultan varias veces por segundo.
DownloadResult download_file(const DownloadRequest& request,const std::atomic<bool>& cancel,
                             const std::atomic<bool>& pause,const DownloadProgress& progress={});
// Texto como "1.4 GB" / "512 MB" para la interfaz.
std::string format_bytes(uint64_t bytes);
// Extension de video deducida de la ruta de la URL (sin consulta); "mp4" si no hay una conocida.
std::string video_extension(const std::string& url);
}
