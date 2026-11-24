#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "subtitle_search.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <curl/curl.h>
#include <zlib.h>
#include "json.hpp"
namespace xs {
using Json=nlohmann::json;
namespace {
constexpr size_t kMaxBody=4*1024*1024,kMaxSubtitle=2*1024*1024;
const char* kAgent="XuperSwitch v1.0";
std::string lower(std::string s){for(char& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;}
std::string str(const Json& o,const char* k){
    if(!o.is_object())return {};auto it=o.find(k);if(it==o.end())return {};
    return it->is_string()?it->get<std::string>():it->is_number()?it->dump():std::string();}
long num(const Json& o,const char* k){
    if(!o.is_object())return 0;auto it=o.find(k);if(it==o.end())return 0;
    if(it->is_number())return static_cast<long>(it->get<double>());
    if(it->is_string()){char* e=nullptr;const long v=std::strtol(it->get<std::string>().c_str(),&e,10);return v;}
    return 0;}
std::string url_encode(const std::string& s){
    static const char* hex="0123456789ABCDEF";std::string out;
    for(unsigned char c:s){if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')out+=static_cast<char>(c);
        else{out+='%';out+=hex[c>>4];out+=hex[c&15];}}
    return out;}
std::string join(const std::vector<std::string>& v,const char* sep,bool up=false){
    std::string out;for(size_t i=0;i<v.size();++i){if(i)out+=sep;std::string x=v[i];if(up)for(char& c:x)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));out+=x;}return out;}
struct Reply {long status=0;std::string body;std::string error;std::string redirect;};
struct Sink {std::string data;bool big=false;};
size_t sink(char* p,size_t s,size_t n,void* u){auto* b=static_cast<Sink*>(u);const size_t bytes=s*n;
    if(b->data.size()+bytes>kMaxBody){b->big=true;return 0;}b->data.append(p,bytes);return bytes;}
// Una peticion. Las que llevan claves nunca siguen redirecciones (no se reenvian a otro servidor).
Reply http(const SubtitleSearchConfig& cfg,const std::string& url,const std::string* post,
           const std::vector<std::string>& headers,bool follow){
    Reply reply;
    if(url.rfind("https://",0)!=0&&!(cfg.allow_http&&url.rfind("http://",0)==0)){reply.error="Direccion no segura.";return reply;}
    CURL* curl=curl_easy_init();if(!curl){reply.error="No se pudo iniciar HTTP.";return reply;}
    Sink out;curl_slist* list=nullptr;
    for(const auto& h:headers)if(h.find_first_of("\r\n")==std::string::npos)list=curl_slist_append(list,h.c_str());
    curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
    curl_easy_setopt(curl,CURLOPT_ACCEPT_ENCODING,"");
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,10L);curl_easy_setopt(curl,CURLOPT_TIMEOUT,25L);
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,follow&&!post?1L:0L);curl_easy_setopt(curl,CURLOPT_MAXREDIRS,3L);
#if LIBCURL_VERSION_NUM >= 0x075500
    const char* protos=cfg.allow_http?"http,https":"https";
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,protos);curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS_STR,protos);
#else
    const long protos=cfg.allow_http?(CURLPROTO_HTTP|CURLPROTO_HTTPS):CURLPROTO_HTTPS;
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS,protos);curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS,protos);
#endif
    curl_easy_setopt(curl,CURLOPT_USERAGENT,kAgent);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,sink);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&out);
    if(!cfg.ca_file.empty())curl_easy_setopt(curl,CURLOPT_CAINFO,cfg.ca_file.c_str());
    if(post){curl_easy_setopt(curl,CURLOPT_POSTFIELDS,post->data());curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE_LARGE,static_cast<curl_off_t>(post->size()));}
    if(list)curl_easy_setopt(curl,CURLOPT_HTTPHEADER,list);
    const CURLcode rc=curl_easy_perform(curl);curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&reply.status);
    {char* loc=nullptr;if(curl_easy_getinfo(curl,CURLINFO_REDIRECT_URL,&loc)==CURLE_OK&&loc)reply.redirect=loc;}
    curl_slist_free_all(list);curl_easy_cleanup(curl);
    if(rc!=CURLE_OK)reply.error=out.big?"Respuesta demasiado grande.":std::string("Conexion: ")+curl_easy_strerror(rc);
    reply.body=std::move(out.data);return reply;}
// Peticiones a OpenSubtitles: si responde con una redireccion (p. ej. 301 hacia la forma
// canonica de la consulta) se repite UNA vez por vez, solo hacia un dominio opensubtitles.com
// por HTTPS (o el mismo origen en pruebas locales): la clave nunca viaja a otro sitio.
Reply os_http(const SubtitleSearchConfig& cfg,std::string url,const std::string* post,const std::vector<std::string>& headers){
    auto origin=[](const std::string& u){const size_t s=u.find("://");if(s==std::string::npos)return std::string();const size_t e=u.find('/',s+3);return u.substr(0,e==std::string::npos?u.size():e);};
    auto host_ok=[&](const std::string& u){
        if(cfg.allow_http&&origin(u)==origin(cfg.opensubtitles_base))return true;
        if(u.rfind("https://",0)!=0)return false;
        std::string h=origin(u).substr(8);const size_t at=h.find('@');if(at!=std::string::npos)return false;
        const size_t colon=h.find(':');if(colon!=std::string::npos)h=h.substr(0,colon);
        static const std::string d="opensubtitles.com";
        return h==d||(h.size()>d.size()&&h.compare(h.size()-d.size()-1,d.size()+1,"."+d)==0);};
    Reply r;
    for(int hop=0;hop<4;++hop){
        r=http(cfg,url,post,headers,false);
        const bool moved=r.status==301||r.status==302||r.status==303||r.status==307||r.status==308;
        if(!moved||r.redirect.empty()||!host_ok(r.redirect))return r;
        url=r.redirect;}
    return r;}
std::string http_problem(const Reply& r,const char* who){
    if(!r.error.empty())return std::string(who)+": "+r.error;
    if(r.status>=300&&r.status<400)return std::string(who)+": redireccion no permitida (HTTP "+std::to_string(r.status)+")";
    if(r.status==401||r.status==403)return std::string(who)+": clave o usuario no validos";
    if(r.status==429)return std::string(who)+": limite de consultas alcanzado";
    if(r.status==406)return std::string(who)+": limite de descargas diarias alcanzado";
    return std::string(who)+": error HTTP "+std::to_string(r.status);}
bool lang_ok(const std::string& l){if(l.size()<2||l.size()>3)return false;for(char c:l)if(!std::isalpha(static_cast<unsigned char>(c)))return false;return true;}
int lang_rank(const SubtitleSearchConfig& c,const std::string& l){
    for(size_t i=0;i<c.languages.size();++i)if(lower(c.languages[i])==l)return static_cast<int>(i);return 99;}
std::string clip(const std::string& s,size_t n){if(s.size()<=n)return s;size_t cut=n;while(cut>0&&(static_cast<unsigned char>(s[cut])&0xC0)==0x80)--cut;return s.substr(0,cut)+"...";}
std::string clean_label(const std::string& s){std::string o;for(unsigned char c:s)o+=(c<32||c==127)?' ':static_cast<char>(c);return o;}
uint32_t le32(const unsigned char* p){return p[0]|(p[1]<<8)|(p[2]<<16)|(static_cast<uint32_t>(p[3])<<24);}
uint16_t le16(const unsigned char* p){return static_cast<uint16_t>(p[0]|(p[1]<<8));}
}
bool load_subtitle_config(const std::string& path,SubtitleSearchConfig& config,std::string& error){
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file.good())return true;                       // sin archivo: simplemente no hay claves
    const auto n=file.tellg();if(n<=0||n>64*1024){error="subtitulos.json vacio o demasiado grande.";return false;}
    file.seekg(0);std::string text(static_cast<size_t>(n),'\0');file.read(&text[0],n);
    Json j=Json::parse(text,nullptr,false);
    if(j.is_discarded()||!j.is_object()){error="subtitulos.json no es un JSON valido.";return false;}
    auto it=j.find("idiomas");
    if(it!=j.end()&&it->is_array()){std::vector<std::string> langs;
        for(const auto& v:*it)if(v.is_string()){const std::string l=lower(v.get<std::string>());if(lang_ok(l)&&langs.size()<6)langs.push_back(l);}
        if(!langs.empty())config.languages=langs;}
    auto os=j.find("opensubtitles");
    if(os!=j.end()&&os->is_object()){config.opensubtitles_key=str(*os,"api_key");config.opensubtitles_user=str(*os,"usuario");config.opensubtitles_password=str(*os,"clave");}
    auto sd=j.find("subdl");
    if(sd!=j.end()&&sd->is_object())config.subdl_key=str(*sd,"api_key");
    return true;}
SubtitleQuery guess_subtitle_query(const std::string& input){
    SubtitleQuery q;std::string t=input;
    // Quita [..] y (..); un (YYYY) se toma como anio.
    std::string flat;int depth=0;char open=0;std::string group;
    for(char c:t){
        if(depth==0&&(c=='('||c=='[')){depth=1;open=c;group.clear();continue;}
        if(depth>0){if((open=='('&&c==')')||(open=='['&&c==']')){depth=0;
                if(group.size()==4&&std::all_of(group.begin(),group.end(),[](unsigned char d){return std::isdigit(d)!=0;})){const int y=std::atoi(group.c_str());if(y>1900&&y<2100)q.year=y;}
                flat+=' ';}else group+=c;continue;}
        flat+=c;}
    t=flat;
    // SxxExx, 1x05, "Temporada N", "Episodio N"
    auto digits=[&](size_t i,int& v){size_t j=i;v=0;while(j<t.size()&&std::isdigit(static_cast<unsigned char>(t[j]))&&j-i<4){v=v*10+(t[j]-'0');++j;}return j;};
    size_t cut=std::string::npos;
    for(size_t i=0;i+3<t.size()&&cut==std::string::npos;++i){
        const bool boundary=i==0||!std::isalnum(static_cast<unsigned char>(t[i-1]));
        if(!boundary)continue;
        if((t[i]=='S'||t[i]=='s')&&std::isdigit(static_cast<unsigned char>(t[i+1]))){
            int s,e;size_t j=digits(i+1,s);
            if(j<t.size()&&(t[j]=='E'||t[j]=='e')&&j+1<t.size()&&std::isdigit(static_cast<unsigned char>(t[j+1]))){digits(j+1,e);q.season=s;q.episode=e;cut=i;}}
        else if(std::isdigit(static_cast<unsigned char>(t[i]))){
            int s,e;size_t j=digits(i,s);
            if(j<t.size()&&(t[j]=='x'||t[j]=='X')&&j+1<t.size()&&std::isdigit(static_cast<unsigned char>(t[j+1]))&&s>0){digits(j+1,e);q.season=s;q.episode=e;cut=i;}}}
    // Palabras clave: "Temporada 2", "Season 2", "Episodio 5", "Capitulo 5", "Episode 5", "Ep 5".
    if(cut==std::string::npos){
        const std::string lt=lower(t);
        static const struct{const char* word;bool season;}marks[]={{"temporada",true},{"season",true},{"episodio",false},{"episode",false},{"capitulo",false},{"cap",false},{"ep",false}};
        for(const auto& m:marks){const size_t w=std::strlen(m.word);size_t at=0;
            while((at=lt.find(m.word,at))!=std::string::npos){
                const bool left=at==0||!std::isalnum(static_cast<unsigned char>(lt[at-1]));size_t j=at+w;
                if(j<lt.size()&&lt[j]=='.')++j;
                while(j<lt.size()&&lt[j]==' ')++j;
                if(left&&j<lt.size()&&std::isdigit(static_cast<unsigned char>(lt[j]))){int v;digits(j,v);
                    if(m.season){if(!q.season)q.season=v;}else if(!q.episode)q.episode=v;
                    if(cut==std::string::npos||at<cut)cut=at;at=j;}
                else at+=w;}}}
    if(cut!=std::string::npos)t=t.substr(0,cut);
    {const std::string pre="Reproducir:";if(t.compare(0,pre.size(),pre)==0)t=t.substr(pre.size());}
    // separadores finales y espacios repetidos
    std::string out;for(char c:t){if(c=='_'||c=='.')c=' ';if(c==' '&&!out.empty()&&out.back()==' ')continue;out+=c;}
    while(!out.empty()&&(out.back()==' '||out.back()=='-'||out.back()==':'))out.pop_back();
    size_t start=0;while(start<out.size()&&(out[start]==' '||out[start]=='-'))++start;
    q.title=out.substr(start);return q;}

bool extract_subtitle_zip(const std::string& zip,std::string& data,std::string& error){
    const auto* b=reinterpret_cast<const unsigned char*>(zip.data());const size_t n=zip.size();
    if(n<22){error="El archivo descargado no es un zip.";return false;}
    size_t eocd=std::string::npos;
    for(size_t i=n-22+1;i-->0;){if(le32(b+i)==0x06054b50u){eocd=i;break;}if(n-i>22+65535)break;}
    if(eocd==std::string::npos){error="El archivo descargado no es un zip.";return false;}
    const size_t count=le16(b+eocd+10);size_t pos=le32(b+eocd+16);
    struct Cand{size_t local,csize,usize;int method;std::string name;int score;};
    std::vector<Cand> cands;
    for(size_t k=0;k<count&&k<2000;++k){
        if(pos+46>n||le32(b+pos)!=0x02014b50u)break;
        const int method=le16(b+pos+10);const size_t cs=le32(b+pos+20),us=le32(b+pos+24),nl=le16(b+pos+28),el=le16(b+pos+30),cl=le16(b+pos+32),lo=le32(b+pos+42);
        if(pos+46+nl>n)break;
        std::string name(reinterpret_cast<const char*>(b+pos+46),nl);const std::string ln=lower(name);
        const bool bad=name.find("..")!=std::string::npos||(!name.empty()&&name.back()=='/')||ln.find("__macosx")!=std::string::npos;
        auto ends=[&](const char* e){const size_t l=std::strlen(e);return ln.size()>=l&&ln.compare(ln.size()-l,l,e)==0;};
        const int score=ends(".srt")?3:ends(".vtt")?2:ends(".ass")?1:0;
        if(!bad&&score>0&&us>0&&us<=kMaxSubtitle)cands.push_back({lo,cs,us,method,name,score});
        pos+=46+nl+el+cl;}
    if(cands.empty()){error="El zip no trae subtitulos .srt/.ass/.vtt.";return false;}
    std::stable_sort(cands.begin(),cands.end(),[](const Cand& a,const Cand& c){return a.score>c.score;});
    const Cand& c=cands.front();
    if(c.local+30>n||le32(b+c.local)!=0x04034b50u){error="Zip danado.";return false;}
    const size_t start=c.local+30+le16(b+c.local+26)+le16(b+c.local+28);
    if(start>n||c.csize>n-start){error="Zip danado.";return false;}
    if(c.method==0){if(c.csize!=c.usize){error="Zip danado.";return false;}data.assign(zip,start,c.csize);return true;}
    if(c.method!=8){error="Compresion de zip no compatible.";return false;}
    z_stream zs{};if(inflateInit2(&zs,-15)!=Z_OK){error="No se pudo descomprimir.";return false;}
    data.assign(c.usize,'\0');
    zs.next_in=const_cast<Bytef*>(b+start);zs.avail_in=static_cast<uInt>(c.csize);
    zs.next_out=reinterpret_cast<Bytef*>(&data[0]);zs.avail_out=static_cast<uInt>(c.usize);
    const int rc=inflate(&zs,Z_FINISH);const size_t produced=zs.total_out;inflateEnd(&zs);
    if(rc!=Z_STREAM_END||produced!=c.usize){data.clear();error="No se pudo descomprimir el subtitulo.";return false;}
    return true;}

bool SubtitleSearch::login(std::string& error){
    if(!token_.empty()||config_.opensubtitles_user.empty()||config_.opensubtitles_password.empty())return true;
    Json body={{"username",config_.opensubtitles_user},{"password",config_.opensubtitles_password}};
    const std::string payload=body.dump();
    const Reply r=os_http(config_,config_.opensubtitles_base+"/login",&payload,{"Api-Key: "+config_.opensubtitles_key,"Content-Type: application/json","Accept: application/json"});
    if(!r.error.empty()||r.status!=200){error=http_problem(r,"OpenSubtitles (inicio de sesion)");return false;}
    Json j=Json::parse(r.body,nullptr,false);token_=str(j,"token");
    if(token_.empty()||token_.find_first_of("\r\n")!=std::string::npos){token_.clear();error="OpenSubtitles: respuesta de inicio de sesion invalida";return false;}
    return true;}
bool SubtitleSearch::search_opensubtitles(const SubtitleQuery& q,std::vector<SubtitleHit>& hits,std::string& error){
    std::string lq=lower(q.title);
    // Parametros en minuscula y orden alfabetico (lo pide la API para aprovechar su cache).
    std::string url=config_.opensubtitles_base+"/subtitles?";
    if(q.episode>0)url+="episode_number="+std::to_string(q.episode)+"&";
    url+="languages="+join(config_.languages,",")+"&query="+url_encode(lq);
    if(q.episode>0)url+=(q.season>0?"&season_number="+std::to_string(q.season):std::string())+"&type=episode";else url+="&type=movie";
    if(q.year>0&&q.episode==0)url+="&year="+std::to_string(q.year);
    std::vector<std::string> headers={"Api-Key: "+config_.opensubtitles_key,"Accept: application/json"};
    std::string note;
    Reply r=os_http(config_,url,nullptr,headers);
    if(!r.error.empty()||r.status!=200){error=http_problem(r,"OpenSubtitles");return false;}
    Json j=Json::parse(r.body,nullptr,false);
    if(!j.is_object()||!j.contains("data")||!j["data"].is_array()){error="OpenSubtitles: respuesta inesperada";return false;}
    for(const auto& item:j["data"]){
        if(!item.is_object())continue;auto at=item.find("attributes");if(at==item.end()||!at->is_object())continue;
        const std::string lang=lower(str(*at,"language"));if(!lang_ok(lang)||lang_rank(config_,lang)==99)continue;
        auto files=at->find("files");if(files==at->end()||!files->is_array()||files->empty())continue;
        const Json& f=(*files)[0];const std::string id=str(f,"file_id");if(id.empty()||id.size()>20||!std::all_of(id.begin(),id.end(),[](unsigned char c){return std::isdigit(c)!=0;}))continue;
        SubtitleHit h;h.provider="OpenSubtitles";h.language=lang;h.id=id;h.downloads=static_cast<int>(std::min<long>(num(*at,"download_count"),1000000000));
        const std::string rel=clean_label(str(*at,"release"));std::string name=rel.empty()?clean_label(str(f,"file_name")):rel;if(name.empty())name="(sin nombre)";
        auto fd=at->find("feature_details");if(fd!=at->end()){h.season=static_cast<int>(num(*fd,"season_number"));h.episode=static_cast<int>(num(*fd,"episode_number"));}
        h.label=clean_label("[OS] "+std::string(1,static_cast<char>(std::toupper(static_cast<unsigned char>(lang[0]))))+lang.substr(1)+"  "+clip(name,70)+"  ("+std::to_string(h.downloads)+")");
        hits.push_back(std::move(h));}
    return true;}
bool SubtitleSearch::search_subdl(const SubtitleQuery& q,std::vector<SubtitleHit>& hits,std::string& error){
    std::string url=config_.subdl_base+"/subtitles?api_key="+url_encode(config_.subdl_key)+"&film_name="+url_encode(q.title)+"&languages="+url_encode(join(config_.languages,",",true));
    url+=q.episode>0?"&type=tv"+(q.season>0?"&season_number="+std::to_string(q.season):std::string())+"&episode_number="+std::to_string(q.episode):"&type=movie";
    if(q.year>0&&q.episode==0)url+="&year="+std::to_string(q.year);
    url+="&subs_per_page=30";
    Reply r=http(config_,url,nullptr,{"Accept: application/json"},false);
    if(!r.error.empty()||r.status!=200){error=http_problem(r,"SubDL");return false;}
    Json j=Json::parse(r.body,nullptr,false);
    if(!j.is_object()){error="SubDL: respuesta inesperada";return false;}
    auto st=j.find("status");if(st!=j.end()&&st->is_boolean()&&!st->get<bool>()){const std::string e=str(j,"error");error="SubDL: "+(e.empty()?std::string("sin resultados"):clip(clean_label(e),80));return false;}
    auto subs=j.find("subtitles");if(subs==j.end()||!subs->is_array()){return true;}
    for(const auto& s:*subs){
        if(!s.is_object())continue;const std::string path=str(s,"url");
        // Solo rutas "/subtitle/<id>.zip" (o "subtitle/..."): nunca direcciones arbitrarias.
        std::string p=path;if(!p.empty()&&p[0]!='/')p="/"+p;
        if(p.rfind("/subtitle/",0)!=0||p.size()>120||p.find("..")!=std::string::npos||p.find_first_of(" \r\n?#")!=std::string::npos||p.size()<5||p.compare(p.size()-4,4,".zip")!=0)continue;
        std::string lang=lower(str(s,"language"));if(lang.empty())lang=lower(str(s,"lang"));
        if(!lang_ok(lang)){// SubDL tambien devuelve nombres ("spanish"): se mapea a 2 letras
            static const std::pair<const char*,const char*> names[]={{"spanish","es"},{"english","en"},{"portuguese","pt"},{"brazilian_portuguese","pt"},{"french","fr"},{"italian","it"},{"german","de"}};
            std::string m;for(const auto& nm:names)if(lang==nm.first)m=nm.second;if(m.empty())continue;lang=m;}
        if(lang_rank(config_,lang)==99)continue;
        SubtitleHit h;h.provider="SubDL";h.language=lang;h.id=p;
        h.season=static_cast<int>(num(s,"season"));h.episode=static_cast<int>(num(s,"episode"));
        h.downloads=0;std::string name=clean_label(str(s,"release_name"));if(name.empty())name=clean_label(str(s,"name"));
        h.label=clean_label("[SubDL] "+std::string(1,static_cast<char>(std::toupper(static_cast<unsigned char>(lang[0]))))+lang.substr(1)+"  "+clip(name,70));
        hits.push_back(std::move(h));}
    return true;}
bool SubtitleSearch::search(const SubtitleQuery& q,std::vector<SubtitleHit>& hits,std::string& error){
    hits.clear();notes_.clear();used_.clear();error.clear();
    if(q.title.empty()){error="No hay titulo para buscar.";return false;}
    if(!config_.any_provider()){error="Faltan las claves de busqueda. Mira LEEME-SUBTITULOS.txt";return false;}
    const bool have_os=!config_.opensubtitles_key.empty(),have_sd=!config_.subdl_key.empty();
    for(int step=0;step<2;++step){
        if(step==0&&!have_os)continue;if(step==1&&!have_sd)continue;
        std::vector<SubtitleHit> found;std::string problem;
        const bool ok=step==0?search_opensubtitles(q,found,problem):search_subdl(q,found,problem);
        // Episodio: se descartan los que declaran otro capitulo (los que no lo declaran se conservan).
        if(q.episode>0)found.erase(std::remove_if(found.begin(),found.end(),[&](const SubtitleHit& h){return (h.episode>0&&h.episode!=q.episode)||(q.season>0&&h.season>0&&h.season!=q.season);}),found.end());
        if(ok&&!found.empty()){
            std::stable_sort(found.begin(),found.end(),[&](const SubtitleHit& a,const SubtitleHit& b){
                const int ra=lang_rank(config_,a.language),rb=lang_rank(config_,b.language);return ra!=rb?ra<rb:a.downloads>b.downloads;});
            if(found.size()>40)found.resize(40);
            hits=std::move(found);used_=step==0?"OpenSubtitles":"SubDL";return true;}
        notes_.push_back(ok?std::string(step==0?"OpenSubtitles":"SubDL")+": sin resultados":problem);}
    error=notes_.empty()?"Sin resultados.":notes_.back();
    if(notes_.size()>1)error=notes_[0]+" | "+notes_[1];
    return false;}
bool SubtitleSearch::fetch(const SubtitleHit& hit,std::string& data,std::string& error){
    data.clear();
    if(hit.provider=="OpenSubtitles"){
        if(config_.opensubtitles_key.empty()||hit.id.empty()){error="Falta la clave de OpenSubtitles.";return false;}
        std::string lerr;if(!login(lerr)){error=lerr;return false;}
        Json body={{"file_id",std::strtoll(hit.id.c_str(),nullptr,10)}};const std::string payload=body.dump();
        std::vector<std::string> headers={"Api-Key: "+config_.opensubtitles_key,"Content-Type: application/json","Accept: application/json"};
        if(!token_.empty())headers.push_back("Authorization: Bearer "+token_);
        Reply r=os_http(config_,config_.opensubtitles_base+"/download",&payload,headers);
        if(!r.error.empty()||r.status!=200){error=http_problem(r,"OpenSubtitles");return false;}
        Json j=Json::parse(r.body,nullptr,false);const std::string link=str(j,"link");
        if(link.empty()){error="OpenSubtitles: no entrego el enlace de descarga.";return false;}
        // El enlace es temporal y se pide sin cabeceras ni claves.
        Reply file=http(config_,link,nullptr,{},true);
        if(!file.error.empty()||file.status!=200){error=http_problem(file,"OpenSubtitles (descarga)");return false;}
        if(file.body.empty()||file.body.size()>kMaxSubtitle){error="El subtitulo recibido no es valido.";return false;}
        data=std::move(file.body);return true;}
    if(hit.provider=="SubDL"){
        if(hit.id.rfind("/subtitle/",0)!=0){error="Subtitulo no valido.";return false;}
        Reply r=http(config_,config_.subdl_download_base+hit.id,nullptr,{},true);
        if(!r.error.empty()||r.status!=200){error=http_problem(r,"SubDL (descarga)");return false;}
        return extract_subtitle_zip(r.body,data,error);}
    error="Fuente desconocida.";return false;}
}
