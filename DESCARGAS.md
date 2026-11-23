# Descargas para ver sin conexion

Permite guardar peliculas y episodios en la tarjeta SD de la consola y verlos despues
sin internet (por ejemplo en modo portatil).

## Como se usa

1. Con conexion, abre una pelicula o serie y entra a su lista de episodios.
2. Sobre el episodio (o sobre "Reproducir: ...") pulsa **ZR**. **L3** descarga ese episodio
   y todos los siguientes de la serie. En PC: **G** y **H**.
3. Sigue navegando: la descarga corre en segundo plano y el avance aparece arriba a la
   derecha. Las tarjetas muestran DESCARGADO, DESCARGANDO, EN COLA o ERROR.
4. Abre **Descargas** (menu lateral o tarjeta de Inicio) para ver todo lo guardado.
   **A** reproduce desde la SD. **ZR** cancela una descarga en curso; pulsado dos veces
   sobre algo ya guardado, lo borra de la SD (video y subtitulos).

Si abres la app sin conexion, aparece un inicio reducido con Descargas, Continuar viendo y
Apoyar. Un contenido descargado tambien se reproduce desde la SD cuando lo abres desde el
catalogo con conexion. La posicion de "Continuar viendo" es la misma en ambos casos.

## Donde se guarda

- Videos y subtitulos: `sdmc:/switch/xuperswitch/descargas/`
- Registro: `sdmc:/switch/xuperswitch/offline_library.json` (solo titulos, identificadores
  y rutas; nunca direcciones de video, cabeceras ni datos de cuenta).
- Una descarga a medias queda como `*.part` y se reanuda sola desde donde iba, incluso
  despues de cerrar la app. Cancelarla desde la lista borra el `.part`.

## Comportamiento y limites

- Solo peliculas y series. La TV en vivo no se puede descargar.
- Para empezar una descarga hace falta conexion y sesion activa: la direccion de cada video
  la entrega el servicio y caduca, asi que se pide justo antes de bajarlo y de nuevo si
  hace falta tras un corte (hasta 4 intentos).
- Se descarga la misma fuente que se reproduce por defecto (la primera que devuelve el
  servicio). No hay selector de calidad para descargas todavia.
- Fuentes en formato HLS (lista `.m3u8` por segmentos) no se pueden guardar como un archivo:
  la descarga falla con un aviso claro. Solo se descargan archivos de video unicos.
- Una tarjeta SD en FAT32 no admite archivos de mas de 4 GB; en ese caso la descarga falla
  con un aviso. En exFAT no hay ese limite. Se verifica el espacio libre antes de bajar.
- Mientras se reproduce un video, las descargas se pausan y se reanudan al salir (cambia
  `kPauseDownloadsWhilePlaying` en `source/main.cpp` si prefieres que sigan).
- Mientras hay descargas pendientes, la app evita que la consola entre en reposo.
- Subtitulos: los del servicio se guardan junto al video y aparecen en Menu > Subtitulos.
  Al abrir, se carga el de tu ultimo idioma elegido si coincide; si no, eliges uno.
- Usa esta funcion solo con contenido al que tu cuenta tenga derecho y respetando los
  terminos del servicio.

## Pruebas

```sh
make -f Makefile.host test-offline test-downloads test-offline-app
```

`test-downloads` levanta un servidor HTTP local y comprueba: descarga completa e identica,
reanudar tras cancelar o cortar la conexion, servidor sin soporte de reanudacion, parcial ya
completo, HLS, paginas de error, 404, cabeceras de autorizacion, falta de espacio, cola,
pausa, cancelacion y borrado. `test-offline-app` arranca la app real sin conexion y
reproduce un archivo guardado con libmpv.

## Estado de validacion

Probado en el PC de desarrollo: descargador y cola contra un servidor HTTP local, biblioteca,
pantallas con SDL real y la app completa arrancando sin conexion y reproduciendo un archivo
guardado con libmpv. Compilado tambien con las opciones de Switch (`-fno-exceptions -fno-rtti`)
y revisada la sintaxis de las rutas `__SWITCH__`. **No se ha probado en una Switch real**: la
velocidad con los buffers de red de libnx, el limite FAT32 y el bloqueo del reposo deben
validarse en consola. Tampoco se probo contra el servicio real, solo con una fuente simulada.
