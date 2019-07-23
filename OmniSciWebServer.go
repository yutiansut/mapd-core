package main

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/ioutil"
	"net"
	"net/http"
	"net/http/httputil"
	"net/http/pprof"
	"net/url"
	"os"
	"os/user"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/Jeffail/gabs"
	"github.com/andrewseidl/viper"
	"github.com/gorilla/handlers"
	"github.com/gorilla/sessions"
	metrics "github.com/rcrowley/go-metrics"
	"github.com/rs/cors"
	log "github.com/sirupsen/logrus"
	"github.com/spf13/pflag"
	graceful "gopkg.in/tylerb/graceful.v1"
)

var (
	port                int
	httpsRedirectPort   int
	backendURL          *url.URL
	frontend            string
	serversJSON         string
	dataDir             string
	tmpDir              string
	certFile            string
	peerCertFile        string
	keyFile             string
	docsDir             string
	readOnly            bool
	verbose             bool
	enableHTTPS         bool
	enableHTTPSAuth     bool
	enableHTTPSRedirect bool
	profile             bool
	compress            bool
	enableMetrics       bool
	connTimeout         time.Duration
	version             string
	proxies             []reverseProxy
)

var (
	registry          metrics.Registry
	sessionStore      *sessions.CookieStore
	serversJSONParams []string
)

type server struct {
	Username string `json:"username"`
	Password string `json:"password"`
	Port     int    `json:"port"`
	Host     string `json:"host"`
	Database string `json:"database"`
	Master   bool   `json:"master"`
}

type thriftMethodTimings struct {
	Regex  *regexp.Regexp
	Start  string
	Units  string
	Labels []string
}

type reverseProxy struct {
	Path   string
	Target *url.URL
}

var (
	thriftMethodMap map[string]thriftMethodTimings
)

const (
	// The name of the cookie that holds the real session ID from SAML login
	thriftSessionCookieName = "omnisci_session"
	// The name of the JavaScript visible cookie indicating SAML auth has succeeded
	samlAuthCookieName = "omnisci_saml_authorized"
	// The magic value used as the "fake" session ID when Immerse is operating in SAML mode
	samlPlaceholderSessionID = "8f61e7d0-b515-49d9-ad77-37ed6e2868ea"
	// The page to redirect the user to when there are errors with SAML auth
	samlErrorPage = "/saml-error.html"
)

func getLogName(lvl string) string {
	n := filepath.Base(os.Args[0])
	h, _ := os.Hostname()
	us, _ := user.Current()
	u := us.Username
	t := time.Now().Format("20060102-150405")
	p := strconv.Itoa(os.Getpid())

	return n + "." + h + "." + u + ".log." + lvl + "." + t + "." + p
}

func init() {
	var err error
	pflag.IntP("port", "p", 6273, "frontend server port")
	pflag.IntP("http-to-https-redirect-port", "", 6280, "frontend server port for http redirect, when https enabled")
	pflag.StringP("backend-url", "b", "", "url to http-port on omnisci_server [http://localhost:6278]")
	pflag.StringSliceP("reverse-proxy", "", nil, "additional endpoints to act as reverse proxies, format '/endpoint/:http://target.example.com'")
	pflag.StringP("frontend", "f", "frontend", "path to frontend directory")
	pflag.StringP("servers-json", "", "", "path to servers.json")
	pflag.StringP("data", "d", "data", "path to OmniSci data directory")
	pflag.StringP("tmpdir", "", "", "path for temporary file storage [/tmp]")
	pflag.StringP("config", "c", "", "path to OmniSci configuration file")
	pflag.StringP("docs", "", "docs", "path to documentation directory")

	pflag.BoolP("read-only", "r", false, "enable read-only mode")
	pflag.BoolP("quiet", "q", true, "suppress non-error messages")
	pflag.BoolP("verbose", "v", false, "print all log messages to stdout")
	pflag.BoolP("enable-https", "", false, "enable HTTPS support")
	pflag.BoolP("enable-https-authentication", "", false, "enable PKI authentication")
	pflag.BoolP("enable-https-redirect", "", false, "enable HTTP to HTTPS redirect")
	pflag.StringP("cert", "", "cert.pem", "certificate file for HTTPS")
	pflag.StringP("peer-cert", "", "peercert.pem", "peer CA certificate PKI authentication")
	pflag.StringP("key", "", "key.pem", "key file for HTTPS")
	pflag.DurationP("timeout", "", 60*time.Minute, "maximum request duration")
	pflag.Bool("profile", false, "enable profiling, accessible from /debug/pprof")
	pflag.Bool("compress", false, "enable gzip compression")
	pflag.Bool("metrics", false, "enable Thrift call metrics, accessible from /metrics")
	pflag.Bool("version", false, "return version")
	pflag.CommandLine.MarkHidden("compress")
	pflag.CommandLine.MarkHidden("profile")
	pflag.CommandLine.MarkHidden("metrics")
	pflag.CommandLine.MarkHidden("quiet")
	pflag.CommandLine.MarkHidden("reverse-proxy")

	pflag.Parse()

	viper.BindPFlag("web.port", pflag.CommandLine.Lookup("port"))
	viper.BindPFlag("web.http-to-https-redirect-port", pflag.CommandLine.Lookup("http-to-https-redirect-port"))
	viper.BindPFlag("web.backend-url", pflag.CommandLine.Lookup("backend-url"))
	viper.BindPFlag("web.reverse-proxy", pflag.CommandLine.Lookup("reverse-proxy"))
	viper.BindPFlag("web.frontend", pflag.CommandLine.Lookup("frontend"))
	viper.BindPFlag("web.servers-json", pflag.CommandLine.Lookup("servers-json"))
	viper.BindPFlag("web.enable-https", pflag.CommandLine.Lookup("enable-https"))
	viper.BindPFlag("web.enable-https-authentication", pflag.CommandLine.Lookup("enable-https-authentication"))
	viper.BindPFlag("web.enable-https-redirect", pflag.CommandLine.Lookup("enable-https-redirect"))
	viper.BindPFlag("web.cert", pflag.CommandLine.Lookup("cert"))
	viper.BindPFlag("web.peer-cert", pflag.CommandLine.Lookup("peer-cert"))
	viper.BindPFlag("web.key", pflag.CommandLine.Lookup("key"))
	viper.BindPFlag("web.timeout", pflag.CommandLine.Lookup("timeout"))
	viper.BindPFlag("web.profile", pflag.CommandLine.Lookup("profile"))
	viper.BindPFlag("web.compress", pflag.CommandLine.Lookup("compress"))
	viper.BindPFlag("web.metrics", pflag.CommandLine.Lookup("metrics"))
	viper.BindPFlag("web.docs", pflag.CommandLine.Lookup("docs"))

	viper.BindPFlag("data", pflag.CommandLine.Lookup("data"))
	viper.BindPFlag("tmpdir", pflag.CommandLine.Lookup("tmpdir"))
	viper.BindPFlag("config", pflag.CommandLine.Lookup("config"))
	viper.BindPFlag("read-only", pflag.CommandLine.Lookup("read-only"))
	viper.BindPFlag("quiet", pflag.CommandLine.Lookup("quiet"))
	viper.BindPFlag("verbose", pflag.CommandLine.Lookup("verbose"))
	viper.BindPFlag("version", pflag.CommandLine.Lookup("version"))

	viper.SetDefault("http-port", 6278)

	viper.SetEnvPrefix("MAPD")
	r := strings.NewReplacer(".", "_")
	viper.SetEnvKeyReplacer(r)
	viper.AutomaticEnv()

	viper.SetConfigType("toml")
	viper.AddConfigPath("/etc/mapd")
	viper.AddConfigPath("$HOME/.config/mapd")
	viper.AddConfigPath(".")

	if viper.GetBool("version") {
		fmt.Println("omnisci_web_server " + version)
		os.Exit(0)
	}

	if viper.IsSet("config") {
		viper.SetConfigFile(viper.GetString("config"))
		err := viper.ReadInConfig()
		if err != nil {
			log.Warn("Error reading config file: " + err.Error())
		}
	}

	port = viper.GetInt("web.port")
	httpsRedirectPort = viper.GetInt("web.http-to-https-redirect-port")
	frontend = viper.GetString("web.frontend")
	docsDir = viper.GetString("web.docs")
	serversJSON = viper.GetString("web.servers-json")

	if viper.IsSet("quiet") && !viper.IsSet("verbose") {
		log.Println("Option --quiet is deprecated and has been replaced by --verbose=false, which is enabled by default.")
		verbose = !viper.GetBool("quiet")
	} else {
		verbose = viper.GetBool("verbose")
	}
	dataDir = viper.GetString("data")
	readOnly = viper.GetBool("read-only")
	connTimeout = viper.GetDuration("web.timeout")
	profile = viper.GetBool("web.profile")
	compress = viper.GetBool("web.compress")
	enableMetrics = viper.GetBool("web.metrics")

	backendURLStr := viper.GetString("web.backend-url")
	if backendURLStr == "" {
		backendURLStr = "http://localhost:" + strconv.Itoa(viper.GetInt("http-port"))
	}

	backendURL, err = url.Parse(backendURLStr)
	if err != nil {
		log.Fatal(err)
	}

	for _, rp := range viper.GetStringSlice("web.reverse-proxy") {
		s := strings.SplitN(rp, ":", 2)
		if len(s) != 2 {
			log.Fatalln("Could not parse reverse proxy string:", rp)
		}
		path := s[0]
		if len(path) == 0 {
			log.Fatalln("Zero-length path passed for reverse proxy:", rp)
		}
		if path[len(path)-1] != '/' {
			path += "/"
		}
		target, err := url.Parse(s[1])
		if err != nil {
			log.Fatal(err)
		}
		if target.Scheme == "" {
			log.Fatalln("Missing URL scheme, need full URL including http/https:", target)
		}
		proxies = append(proxies, reverseProxy{path, target})
	}

	if os.Getenv("TMPDIR") != "" {
		tmpDir = os.Getenv("TMPDIR")
	}
	if viper.IsSet("tmpdir") {
		tmpDir = viper.GetString("tmpdir")
	}
	if tmpDir != "" {
		err = os.MkdirAll(tmpDir, 0750)
		if err != nil {
			log.Fatal("Could not create temp dir: ", err)
		}
		os.Setenv("TMPDIR", tmpDir)
	}

	enableHTTPS = viper.GetBool("web.enable-https")
	enableHTTPSAuth = viper.GetBool("web.enable-https-authentication")
	enableHTTPSRedirect = viper.GetBool("web.enable-https-redirect")
	certFile = viper.GetString("web.cert")
	keyFile = viper.GetString("web.key")
	peerCertFile = viper.GetString("web.peer-cert")

	registry = metrics.NewRegistry()

	// TODO(andrew): this should be auto-gen'd by Thrift
	thriftMethodMap = make(map[string]thriftMethodTimings)
	thriftMethodMap["render"] = thriftMethodTimings{
		Regex:  regexp.MustCompile(`"?":{"i64":(\d+)`),
		Start:  `"3":{"i64":`,
		Units:  "ms",
		Labels: []string{"execution_time_ms", "render_time_ms", "total_time_ms"},
	}
	thriftMethodMap["sql_execute"] = thriftMethodTimings{
		Regex:  regexp.MustCompile(`"?":{"i64":(\d+)`),
		Start:  `"2":{"i64":`,
		Units:  "ms",
		Labels: []string{"execution_time_ms", "total_time_ms"},
	}

	c := 64
	b := make([]byte, c)
	_, err = rand.Read(b)
	if err != nil {
		fmt.Println("error:", err)
		return
	}
	sessionStore = sessions.NewCookieStore(b)
	sessionStore.MaxAge(0)
	serversJSONParams = []string{"username", "password", "database"}
}

func uploadHandler(rw http.ResponseWriter, r *http.Request) {
	var (
		status int
		err    error
	)

	defer func() {
		if err != nil {
			http.Error(rw, err.Error(), status)
		}
	}()

	err = r.ParseMultipartForm(32 << 20)
	if err != nil {
		status = http.StatusInternalServerError
		return
	}

	if readOnly {
		status = http.StatusUnauthorized
		err = errors.New("Uploads disabled: server running in read-only mode")
		return
	}

	uploadDir := dataDir + "/mapd_import/"
	sid := r.Header.Get("sessionid")
	samlAuthCookie, samlAuthCookieErr := r.Cookie(samlAuthCookieName)
	sessionIDCookie, sessionIDCookieErr := r.Cookie(thriftSessionCookieName)
	if samlAuthCookieErr == nil && sessionIDCookieErr == nil && samlAuthCookie.Value == "true" && sessionIDCookie != nil {
		sid = sessionIDCookie.Value
	} else if len(r.FormValue("sessionid")) > 0 {
		sid = r.FormValue("sessionid")
	}

	sessionIDSha256 := sha256.Sum256([]byte(filepath.Base(filepath.Clean(sid))))
	sessionID := hex.EncodeToString(sessionIDSha256[:])
	uploadDir = dataDir + "/mapd_import/" + sessionID + "/"

	for _, fhs := range r.MultipartForm.File {
		for _, fh := range fhs {
			infile, err := fh.Open()
			if err != nil {
				status = http.StatusInternalServerError
				return
			}
			err = os.MkdirAll(uploadDir, 0755)
			if err != nil {
				status = http.StatusInternalServerError
				return
			}
			fn := filepath.Base(filepath.Clean(fh.Filename))
			outfile, err := os.Create(uploadDir + fn)
			if err != nil {
				status = http.StatusInternalServerError
				return
			}
			_, err = io.Copy(outfile, infile)
			if err != nil {
				status = http.StatusInternalServerError
				return
			}
			fp := filepath.Base(outfile.Name())
			rw.Write([]byte(fp))
		}
	}
}

func deleteUploadHandler(rw http.ResponseWriter, r *http.Request) {
	// not yet implemented
}

func recordTiming(name string, dur time.Duration) {
	t := registry.GetOrRegister(name, metrics.NewTimer())
	// TODO(andrew): change units to milliseconds if it does not impact other
	// calculations
	t.(metrics.Timer).Update(dur)
}

func recordTimingDuration(name string, then time.Time) {
	dur := time.Since(then)
	recordTiming(name, dur)
}

// ResponseMultiWriter implements an http.ResponseWriter with support for
// outputting to an additional io.Writer.
type ResponseMultiWriter struct {
	io.Writer
	http.ResponseWriter
}

func (w *ResponseMultiWriter) writeHeader(c int) {
	w.ResponseWriter.Header().Del("Content-Length")
	w.ResponseWriter.WriteHeader(c)
}

func (w *ResponseMultiWriter) Write(b []byte) (int, error) {
	h := w.ResponseWriter.Header()
	h.Del("Content-Length")
	return w.Writer.Write(b)
}

func hasCustomServersJSONParams(r *http.Request) bool {
	// Checking for form values requires calling ParseForm, which modifies the
	// request buffer and causes issues with the proxy. Solution is to duplicate
	// the request body and reset it after reading.
	b, _ := ioutil.ReadAll(r.Body)
	rdr1 := ioutil.NopCloser(bytes.NewReader(b))
	rdr2 := ioutil.NopCloser(bytes.NewReader(b))
	r.Body = rdr1
	defer func() { r.Body = rdr2 }()
	for _, k := range serversJSONParams {
		if len(r.FormValue(k)) > 0 {
			return true
		}
	}
	return false
}

// thriftTimingHandler records timings for all Thrift method calls. It also
// records timings reported by the backend, as defined by ThriftMethodMap.
// TODO(andrew): use proper Thrift-generated parser
func thriftTimingHandler(h http.Handler) http.Handler {
	return http.HandlerFunc(func(rw http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/" && hasCustomServersJSONParams(r) {
			setServersJSONHandler(rw, r)
			http.Redirect(rw, r, r.URL.Path, http.StatusSeeOther)
			return
		}

		if !enableMetrics || r.Method != "POST" || (r.Method == "POST" && r.URL.Path != "/") {
			h.ServeHTTP(rw, r)
			return
		}

		var thriftMethod string
		body, _ := ioutil.ReadAll(r.Body)
		r.Body = ioutil.NopCloser(bytes.NewReader(body))

		elems := strings.SplitN(string(body), ",", 3)
		if len(elems) > 1 {
			thriftMethod = strings.Trim(elems[1], `"`)
		}

		if len(thriftMethod) < 1 {
			h.ServeHTTP(rw, r)
			return
		}

		tm, exists := thriftMethodMap[thriftMethod]
		defer recordTimingDuration("all", time.Now())
		defer recordTimingDuration(thriftMethod, time.Now())

		if !exists {
			h.ServeHTTP(rw, r)
			return
		}

		buf := new(bytes.Buffer)
		mw := io.MultiWriter(buf, rw)

		rw = &ResponseMultiWriter{
			Writer:         mw,
			ResponseWriter: rw,
		}

		h.ServeHTTP(rw, r)

		go func() {
			offset := strings.LastIndex(buf.String(), tm.Start)
			if offset >= 0 {
				timings := tm.Regex.FindAllStringSubmatch(buf.String()[offset:], len(tm.Labels))
				for k, v := range timings {
					dur, _ := time.ParseDuration(v[1] + tm.Units)
					recordTiming(thriftMethod+"."+tm.Labels[k], dur)
				}
			}
		}()
	})
}

func metricsHandler(rw http.ResponseWriter, r *http.Request) {
	if len(r.FormValue("enable")) > 0 {
		enableMetrics = true
	} else if len(r.FormValue("disable")) > 0 {
		enableMetrics = false
	}
	jsonBuf := new(bytes.Buffer)
	metrics.WriteJSONOnce(registry, jsonBuf)
	ijsonBuf := new(bytes.Buffer)
	json.Indent(ijsonBuf, jsonBuf.Bytes(), "", "  ")
	rw.Write(ijsonBuf.Bytes())
}

func metricsResetHandler(rw http.ResponseWriter, r *http.Request) {
	registry.UnregisterAll()
	metricsHandler(rw, r)
}

func setServersJSONHandler(rw http.ResponseWriter, r *http.Request) {
	session, _ := sessionStore.Get(r, "servers-json")

	for _, key := range serversJSONParams {
		if len(r.FormValue(key)) > 0 {
			session.Values[key] = r.FormValue(key)
		}
	}

	session.Save(r, rw)
}

func clearServersJSONHandler(rw http.ResponseWriter, r *http.Request) {
	session, _ := sessionStore.Get(r, "servers-json")

	session.Options.MaxAge = -1

	session.Save(r, rw)
}

func docsHandler(rw http.ResponseWriter, r *http.Request) {
	h := http.StripPrefix("/docs/", http.FileServer(http.Dir(docsDir)))
	h.ServeHTTP(rw, r)
}

// samlPostHandler receives a XML SAML payload from a provider (e.g. Okta) and
// then makes a connect call to OmniSciDB with the base64'd payload. If the call succeeds
// we then set a session cookie (`omnisci_session`) for Immerse to use for login, as well
// as the username (`omnisci_username`) and db name (`omnisci_db`).
func samlPostHandler(rw http.ResponseWriter, r *http.Request) {
	var err error
	ok := false
	targetPage := "/"

	if r.Method == "POST" {
		var sessionToken string

		b64ResponseXML := r.FormValue("SAMLResponse")

		// This is what a Thrift connect call to OmniSciDB looks like. Here, the username and database
		// name are left blank, per SAML login conventions. Hand-crafting Thrift messages like this
		// isn't exactly "best practices", but it beats importing a whole Thrift lib for just this.
		var jsonString = []byte(`[1,"connect",1,0,{"2":{"str":"` + b64ResponseXML + `"},"3":{"str":""}}]`)

		resp, err := http.Post(backendURL.String(), "application/vnd.apache.thrift.json", bytes.NewBuffer(jsonString))
		if err != nil {
			return
		}

		bodyBytes, _ := ioutil.ReadAll(resp.Body)
		resp.Body.Close()

		jsonParsed, _ := gabs.ParseJSON(bodyBytes)
		if err != nil {
			return
		}

		relayState := r.FormValue("RelayState")
		if relayState != "" {
			targetPage = relayState
		}

		// We should have one of the two following payloads at this point:
		// 		Success => [1,"connect",2,0,{"0":{"str":"5h6KW9NTv1ef1kOfOlAGN9q63usKOg0i"}}]
		// 		Failure => [1,"connect",2,0,{"1":{"rec":{"1":{"str":"Invalid credentials."}}}}]
		// Only set the cookie if we can parse a success payload.
		sessionToken, ok = jsonParsed.Index(4).Search("0", "str").Data().(string)
		if ok {
			sessionIDCookie := http.Cookie{
				Name:     thriftSessionCookieName,
				Value:    sessionToken,
				HttpOnly: true,
			}
			http.SetCookie(rw, &sessionIDCookie)

			samlFlagCookie := http.Cookie{
				Name:  samlAuthCookieName,
				Value: "true",
			}
			http.SetCookie(rw, &samlFlagCookie)
		}
	}

	defer func() {
		if ok {
			http.Redirect(rw, r, targetPage, 301)
		} else {
			var errorString string
			if err != nil {
				errorString = err.Error()
			} else {
				errorString = "invalid credentials"
			}
			http.Redirect(rw, r, samlErrorPage, 303)
			log.Infoln("Error logging user in via SAML: ", errorString)
		}
	}()
}

type ServeIndexOn404FileSystem struct {
	http.FileSystem
	Filename string
}

func (fs ServeIndexOn404FileSystem) Open(name string) (http.File, error) {
	file, err := fs.FileSystem.Open(name)
	if os.IsNotExist(err) {
		if strings.HasPrefix(name, "/beta/") {
			file, err = fs.FileSystem.Open("/beta/index.html")
		} else {
			file, err = fs.FileSystem.Open("/index.html")
		}
	}

	if err != nil {
		if stat, statErr := file.Stat(); statErr != nil {
			fs.Filename = stat.Name()
		}
	}

	return file, err
}

func thriftOrFrontendHandler(rw http.ResponseWriter, r *http.Request) {
	fs := ServeIndexOn404FileSystem{http.Dir(frontend), ""}
	h := http.StripPrefix("/", http.FileServer(fs))

	if r.Method == "POST" {
		h = httputil.NewSingleHostReverseProxy(backendURL)
		rw.Header().Del("Access-Control-Allow-Origin")

		// If the thriftSessionCookieName is present, it holds the real session ID, while the Thrift
		// call is using a placeholder. This code replaces the fake session ID in the Thrift call
		// with the real one from the cookie.
		samlAuthCookie, samlAuthCookieErr := r.Cookie(samlAuthCookieName)
		sessionIDCookie, sessionIDCookieErr := r.Cookie(thriftSessionCookieName)
		if samlAuthCookieErr == nil && sessionIDCookieErr == nil && samlAuthCookie.Value == "true" && sessionIDCookie != nil {
			bodyBytes, _ := ioutil.ReadAll(r.Body)
			defer r.Body.Close()

			// In general, if we encounter any errors, we want to make this session code a noop
			jsonParsed, err := gabs.ParseJSON(bodyBytes)
			if err == nil {
				// Grab the session ID from the thrift call
				sessionToken, ok := jsonParsed.Index(4).Search("1", "str").Data().(string)

				// If the session ID is our known placeholder ID, replace it with the real one
				if ok && sessionToken == samlPlaceholderSessionID {
					jsonParsed.Index(4).Set(sessionIDCookie.Value, "1", "str")

					r.Body = ioutil.NopCloser(bytes.NewReader([]byte(jsonParsed.String())))
					r.ContentLength = int64(len(jsonParsed.String()))
				} else {
					r.Body = ioutil.NopCloser(bytes.NewReader(bodyBytes))
				}
			} else {
				r.Body = ioutil.NopCloser(bytes.NewReader(bodyBytes))
			}
		}
	}

	if r.Method == "GET" && (r.URL.Path == "/" || r.URL.Path == "/beta/" || strings.HasSuffix(fs.Filename, ".html")) {
		rw.Header().Del("Cache-Control")
		rw.Header().Add("Cache-Control", "no-cache, no-store, must-revalidate")
	}

	h.ServeHTTP(rw, r)
}

func betaOrRedirectFrontendHandler(rw http.ResponseWriter, r *http.Request) {
	cookie, err := r.Cookie("omnisci-beta")
	if err != nil || cookie.Value != "true" {
		http.Redirect(rw, r, "/", http.StatusTemporaryRedirect)
		return
	}

	thriftOrFrontendHandler(rw, r)
}

func httpToHTTPSRedirectHandler(rw http.ResponseWriter, r *http.Request) {
	// Redirect HTTP request to same URL with only two changes: https scheme,
	// and the main server port configured in the 'port' param, rather than the
	// incoming port ('http-to-https-redirect-port')
	requestHost, _, _ := net.SplitHostPort(r.Host)
	redirectURL := url.URL{Scheme: "https", Host: requestHost + ":" + strconv.Itoa(port), Path: r.URL.Path, RawQuery: r.URL.RawQuery}
	http.Redirect(rw, r, redirectURL.String(), http.StatusTemporaryRedirect)
}

func (rp *reverseProxy) proxyHandler(rw http.ResponseWriter, r *http.Request) {
	h := http.StripPrefix(rp.Path, httputil.NewSingleHostReverseProxy(rp.Target))
	h.ServeHTTP(rw, r)
}

func downloadsHandler(rw http.ResponseWriter, r *http.Request) {
	if r.RequestURI == "/downloads/" {
		rw.Write([]byte(""))
		return
	}
	h := http.StripPrefix("/downloads/", http.FileServer(http.Dir(dataDir+"/mapd_export/")))
	h.ServeHTTP(rw, r)
}

func modifyServersJSON(r *http.Request, orig []byte) ([]byte, error) {
	session, _ := sessionStore.Get(r, "servers-json")
	j, err := gabs.ParseJSON(orig)
	if err != nil {
		return nil, err
	}

	jj, err := j.Children()
	if err != nil {
		return nil, err
	}

	for _, key := range serversJSONParams {
		if session.Values[key] != nil {
			_, err = jj[0].Set(session.Values[key].(string), key)
			if err != nil {
				return nil, err
			}
		}
	}

	return j.BytesIndent("", "  "), nil
}

func serversHandler(rw http.ResponseWriter, r *http.Request) {
	var j []byte
	servers := ""
	subDir := filepath.Dir(r.URL.Path)
	if len(serversJSON) > 0 {
		servers = serversJSON
	} else {
		servers = frontend + subDir + "/servers.json"
		if _, err := os.Stat(servers); os.IsNotExist(err) {
			servers = frontend + "/servers.json"
		}
	}
	j, err := ioutil.ReadFile(servers)
	if err != nil {
		s := server{}
		s.Master = true
		s.Username = "admin"
		s.Password = "HyperInteractive"
		s.Database = "omnisci"

		h, p, _ := net.SplitHostPort(r.Host)
		s.Port, _ = net.LookupPort("tcp", p)
		s.Host = h
		// handle IPv6 addresses
		ip := net.ParseIP(h)
		if ip != nil && ip.To4() == nil {
			s.Host = "[" + h + "]"
		}

		ss := []server{s}
		j, _ = json.Marshal(ss)
	}

	jj, err := modifyServersJSON(r, j)
	if err != nil {
		msg := "Error processing servers.json: " + err.Error()
		http.Error(rw, msg, http.StatusInternalServerError)
		log.Println(msg)
		return
	}

	rw.Header().Del("Cache-Control")
	rw.Header().Add("Cache-Control", "no-cache, no-store, must-revalidate")
	rw.Write(jj)
}

func versionHandler(rw http.ResponseWriter, r *http.Request) {
	outVers := "OmniSciDB:\n" + version
	versTxt := frontend + "/version.txt"
	feVers, err := ioutil.ReadFile(versTxt)
	if err == nil {
		outVers += "\n\n"
		outVers += "Immerse:\n"
		outVers += string(feVers)
	}
	rw.Write([]byte(outVers))
}

func main() {
	if _, err := os.Stat(dataDir + "/mapd_log/"); os.IsNotExist(err) {
		os.MkdirAll(dataDir+"/mapd_log/", 0755)
	}
	lf, err := os.OpenFile(dataDir+"/mapd_log/"+getLogName("ALL"), os.O_WRONLY|os.O_CREATE, 0644)
	if err != nil {
		log.Fatal("Error opening log file: ", err)
	}
	defer lf.Close()

	alf, err := os.OpenFile(dataDir+"/mapd_log/"+getLogName("ACCESS"), os.O_WRONLY|os.O_CREATE, 0644)
	if err != nil {
		log.Fatal("Error opening log file: ", err)
	}
	defer alf.Close()

	var alog io.Writer
	if !verbose {
		log.SetOutput(lf)
		log.SetFormatter(&log.TextFormatter{
			DisableColors: true,
			FullTimestamp: true,
		})

		alog = alf
	} else {
		log.SetOutput(io.MultiWriter(os.Stdout, lf))
		alog = io.MultiWriter(os.Stdout, alf)
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/saml-post", samlPostHandler)
	mux.HandleFunc("/upload", uploadHandler)
	mux.HandleFunc("/downloads/", downloadsHandler)
	mux.HandleFunc("/deleteUpload", deleteUploadHandler)
	mux.HandleFunc("/servers.json", serversHandler)
	mux.HandleFunc("/", thriftOrFrontendHandler)
	mux.HandleFunc("/beta/", betaOrRedirectFrontendHandler)
	mux.HandleFunc("/docs/", docsHandler)
	mux.HandleFunc("/metrics/", metricsHandler)
	mux.HandleFunc("/metrics/reset/", metricsResetHandler)
	mux.HandleFunc("/version.txt", versionHandler)
	mux.HandleFunc("/_internal/set-servers-json", setServersJSONHandler)
	mux.HandleFunc("/_internal/clear-servers-json", clearServersJSONHandler)

	if profile {
		mux.HandleFunc("/debug/pprof/", pprof.Index)
		mux.HandleFunc("/debug/pprof/cmdline", pprof.Cmdline)
		mux.HandleFunc("/debug/pprof/profile", pprof.Profile)
		mux.HandleFunc("/debug/pprof/symbol", pprof.Symbol)
	}

	for k := range proxies {
		rp := proxies[k]
		log.Infoln("Proxy:", rp.Path, "to", rp.Target)
		mux.HandleFunc(rp.Path, rp.proxyHandler)
	}

	c := cors.New(cors.Options{
		AllowedHeaders: []string{"Accept", "Cache-Control", "Content-Type", "sessionid", "X-Requested-With"},
	})
	cmux := c.Handler(mux)
	cmux = handlers.LoggingHandler(alog, cmux)
	cmux = thriftTimingHandler(cmux)
	if compress {
		cmux = handlers.CompressHandler(cmux)
	}

	tlsConfig := &tls.Config{}
	if enableHTTPSAuth {
		caCert, err := ioutil.ReadFile(peerCertFile)
		if err != nil {
			log.Fatalln("Errors opening peer file:", err, peerCertFile)
		}
		caCertPool := x509.NewCertPool()
		caCertPool.AppendCertsFromPEM(caCert)
		tlsConfig = &tls.Config{
			ClientCAs:  caCertPool,
			ClientAuth: tls.RequireAndVerifyClientCert,
		}
		tlsConfig.BuildNameToCertificate()

	}

	srv := &graceful.Server{
		Timeout: 5 * time.Second,
		Server: &http.Server{
			Addr:         ":" + strconv.Itoa(port),
			Handler:      cmux,
			ReadTimeout:  connTimeout,
			WriteTimeout: connTimeout,
			TLSConfig:    tlsConfig,
		},
	}

	if enableHTTPS {
		if _, err := os.Stat(certFile); err != nil {
			log.Fatalln("Error opening certificate:", err)
		}
		if _, err := os.Stat(keyFile); err != nil {
			log.Fatalln("Error opening keyfile:", err)
		}

		if enableHTTPSRedirect {
			go func() {
				err := http.ListenAndServe(":"+strconv.Itoa(httpsRedirectPort), http.HandlerFunc(httpToHTTPSRedirectHandler))

				if err != nil {
					log.Fatalln("Error starting http redirect listener:", err)
				}
			}()
		}

		err = srv.ListenAndServeTLS(certFile, keyFile)
	} else {
		err = srv.ListenAndServe()
	}

	if err != nil {
		log.Fatal("Error starting http server: ", err)
	}
}
